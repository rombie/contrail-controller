/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_mvpn.h"

#include <boost/foreach.hpp>

#include "base/task_annotations.h"
#include "bgp/ermvpn/ermvpn_route.h"
#include "bgp/ermvpn/ermvpn_table.h"
#include "bgp/bgp_multicast.h"
#include "bgp/bgp_server.h"
#include "bgp/mvpn/mvpn_table.h"
#include "bgp/routing-instance/path_resolver.h"
#include "bgp/routing-instance/routing_instance.h"

using std::make_pair;
using std::ostringstream;
using std::string;

class MvpnManager::DeleteActor : public LifetimeActor {
public:
    explicit DeleteActor(MvpnManager *manager)
        : LifetimeActor(manager->table_->routing_instance()->server()->
                lifetime_manager()), manager_(manager) {
    }
    virtual ~DeleteActor() {
    }

    virtual bool MayDelete() const {
        return true;
    }

    virtual void Shutdown() {
    }

    virtual void Destroy() {
    }

private:
    MvpnManager *manager_;
};

MvpnManager::MvpnManager(MvpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          resolver_(new PathResolver(table, true)),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MvpnManager::~MvpnManager() {
    Terminate();
}

bool MvpnManager::IsMaster() const {
    return table_->IsMaster();
}

MvpnTable *MvpnManager::table() {
    return table_;
}

const MvpnTable *MvpnManager::table() const {
    return table_;
}

int MvpnManager::listener_id() const {
    return listener_id_;
}

PathResolver *MvpnManager::resolver() {
    return resolver_;
}

MvpnState::SG::SG(const Ip4Address &source, const Ip4Address &group) :
    source(IpAddress(source)), group(IpAddress(group)) {
}

MvpnState::SG::SG(const IpAddress &source, const IpAddress &group) :
    source(source), group(group) {
}

bool MvpnState::SG::operator<(const SG &other)  const {
    return (source < other.source) ?  true : (group < other.source);
}

const MvpnState::SG &MvpnState::sg() const {
    return sg_;
}

const MvpnState::RoutesSet &MvpnState::cjoin_routes() const {
    return cjoin_routes_received_;
}

ErmVpnRoute *MvpnState::global_ermvpn_tree_rt() {
    return global_ermvpn_tree_rt_;
}

const ErmVpnRoute *MvpnState::global_ermvpn_tree_rt() const {
    return global_ermvpn_tree_rt_;
}

const MvpnState::RoutesSet &MvpnState::leaf_ad_routes() const {
    return leaf_ad_routes_originated_;
}

void MvpnState::set_global_ermvpn_tree_rt(ErmVpnRoute *global_ermvpn_tree_rt) {
    global_ermvpn_tree_rt = global_ermvpn_tree_rt_;
}

MvpnDBState::MvpnDBState(MvpnState *state) : state(state) {
    if (state)
        state->refcount_++;
}

const MvpnProjectManager *MvpnManager::GetProjectManager() const {
    std::string project_manager_network =
        table_->routing_instance()->mvpn_project_manager_network();
    if (project_manager_network.empty())
        return NULL;
    RoutingInstance *rtinstance =
        table_->routing_instance()->manager()->
            GetRoutingInstance(project_manager_network);
    if (!rtinstance || rtinstance->deleted())
        return NULL;
    MvpnTable *table =
        dynamic_cast<MvpnTable *>(rtinstance->GetTable(Address::MVPN));
    if (!table || table->IsDeleted())
        return NULL;
    return table->project_manager();
}

// Call const version of the GetProjectManager().
MvpnProjectManager *MvpnManager::GetProjectManager() {
    return const_cast<MvpnProjectManager *>(
        static_cast<const MvpnManager *>(this)->GetProjectManager());
}

void MvpnManager::Initialize() {
    assert(!IsMaster());
    AllocPartitions();

    listener_id_ = table_->Register(
        boost::bind(&MvpnManager::RouteListener, this, _1, _2),
        "MvpnManager");

    // Originate Type1 Internal Auto-Discovery Route.
    table_->CreateType1Route();

    // Originate Type2 External Auto-Discovery Route.
    table_->CreateType2Route();
}

void MvpnManager::Terminate() {
    table_->Unregister(listener_id_);
    FreePartitions();
}

LifetimeActor *MvpnManager::deleter() {
    return deleter_.get();
}

const LifetimeActor *MvpnManager::deleter() const {
    return deleter_.get();
}

bool MvpnManager::deleted() const {
    return deleter_->IsDeleted();
}

void MvpnManager::RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    if (IsMaster())
        return;

    MvpnRoute *route = dynamic_cast<MvpnRoute *>(db_entry);
    if (!route)
        return;

    if (route->GetPrefix().type() == MvpnPrefix::IntraASPMSIADRoute ||
            route->GetPrefix().type() == MvpnPrefix::InterASPMSIADRoute) {
        UpdateNeighbor(route);
        return;
    }

    if (route->GetPrefix().type() == MvpnPrefix::SPMSIADRoute) {
        MvpnManagerPartition *partition = partitions_[tpart->index()];
        partition->ProcessSPMSIRoute(route);
        return;
    }

    if (route->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute) {
        BgpPath *src_path = route->BestPath();
        if (!src_path)
            return;
        MvpnManagerPartition *partition = partitions_[tpart->index()];
        if (partition->ProcessSourceTreeJoinRoute(route)) {
            route->Notify();
        }
        return;
    }

    if (route->GetPrefix().type() == MvpnPrefix::LeafADRoute) {
        MvpnManagerPartition *partition = partitions_[tpart->index()];
        partition->ProcessLeafADRoute(route);
        return;
    }
}

void MvpnManager::AllocPartitions() {
    for (int part_id = 0; part_id < table_->PartitionCount(); part_id++)
        partitions_.push_back(new MvpnManagerPartition(this, part_id));
}

void MvpnManager::FreePartitions() {
    for (size_t part_id = 0; part_id < partitions_.size(); part_id++) {
        delete partitions_[part_id];
    }
    partitions_.clear();
}

MvpnManagerPartition *MvpnManager::GetPartition(int part_id) {
    return partitions_[part_id];
}

const MvpnManagerPartition *MvpnManager::GetPartition(int part_id) const {
    return GetPartition(part_id);
}

void MvpnManager::ManagedDelete() {
    deleter_->Delete();
}

MvpnNeighbor::MvpnNeighbor() : asn(0), vn_id(0), external(false) {
}

MvpnNeighbor::MvpnNeighbor(const IpAddress &address, uint32_t asn,
        uint16_t vn_id, bool external) :
    address(address), asn(asn), vn_id(vn_id), external(external) {
    ostringstream os;
    os << address << ":" << asn << ":" << vn_id << ":" << external;
    name = os.str();
}

string MvpnNeighbor::ToString() const {
    return name;
}

bool MvpnNeighbor::operator==(const MvpnNeighbor &rhs) const {
    return address == rhs.address && asn == rhs.asn &&
           vn_id == rhs.vn_id && external == rhs.external;
}

bool MvpnManager::findNeighbor(const IpAddress &address, MvpnNeighbor *nbr)
        const {
    tbb::mutex::scoped_lock(neighbors_mutex_);

    NeighborsMap::const_iterator iter = neighbors_.find(address);
    if (iter != neighbors_.end()) {
        *nbr = iter->second;
        return true;
    }
    return false;
}

void MvpnManager::UpdateNeighbor(MvpnRoute *route) {
    tbb::mutex::scoped_lock(neighbors_mutex_);

    MvpnNeighbor old_neighbor;
    RouteDistinguisher rd = route->GetPrefix().route_distinguisher();
    IpAddress address = route->GetPrefix().originator2();
    bool found = findNeighbor(address, &old_neighbor);
    MvpnNeighbor neighbor(address, route->GetPrefix().asn(), rd.GetVrfId(),
        route->GetPrefix().type() == MvpnPrefix::InterASPMSIADRoute);

    if (!route->IsValid()) {
        if (found) {
            neighbors_.erase(address);
            NotifyAllRoutes();
        }
        return;
    }

    // Ignore if there is no change.
    if (found && old_neighbor == neighbor)
        return;

    if (found)
        neighbors_.erase(address);
    assert(neighbors_.insert(make_pair(address, neighbor)).second);

    // TODO(Ananth) Only need to re-evaluate all type-7 join routes.
    NotifyAllRoutes();
}

void MvpnManager::NotifyAllRoutes() {
    table_->NotifyAllEntries();
}

BgpRoute *MvpnManager::RouteReplicate(BgpServer *server, BgpTable *table,
    BgpRoute *rt, const BgpPath *src_path, ExtCommunityPtr community) {
    CHECK_CONCURRENCY("db::DBTable");

    MvpnRoute *src_rt = dynamic_cast<MvpnRoute *>(rt);
    MvpnTable *src_table = dynamic_cast<MvpnTable *>(table);

    // Phase 1 support for only senders outside the cluster and receivers inside
    // the cluster. Hence don't replicate to a VRF from other VRF tables.
    if (!IsMaster()) {
        MvpnTable *src_mvpn_table = dynamic_cast<MvpnTable *>(src_table);
        if (!src_mvpn_table->IsMaster())
            return NULL;
    }

    MvpnManagerPartition *mvpn_manager_partition =
        GetPartition(src_rt->get_table_partition()->index());

    if (src_rt->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute) {
        return mvpn_manager_partition->ReplicateType7SourceTreeJoin(server,
            src_table, src_rt, src_path, community);
    }

    if (src_rt->GetPrefix().type() == MvpnPrefix::LeafADRoute) {
        return mvpn_manager_partition->ReplicateType4LeafAD(server,
            src_table, src_rt, src_path, community);
    }

    return mvpn_manager_partition->ReplicatePath(server, src_table, src_rt,
            src_path, community, NULL, NULL);
}

void MvpnManager::ResolvePath(RoutingInstance *rtinstance, BgpRoute *rt,
        BgpPath *path) {
    MvpnRoute *mvpn_rt = static_cast<MvpnRoute *>(rt);
    BgpTable *table = rtinstance->GetTable(Address::INET);

    IpAddress address = IpAddress(mvpn_rt->GetPrefix().source());
    table->path_resolver()->StartPathResolution(
        rt->get_table_partition()->index(), path, rt, table, &address);
}

MvpnManagerPartition::MvpnManagerPartition(MvpnManager *manager, int part_id)
    : manager_(manager), part_id_(part_id) {
}

MvpnManagerPartition::~MvpnManagerPartition() {
}

MvpnProjectManagerPartition *
MvpnManagerPartition::GetProjectManagerPartition() {
    MvpnProjectManager *project_manager = manager_->GetProjectManager();
    return project_manager ? project_manager->GetPartition(part_id_) : NULL;
}

const MvpnProjectManagerPartition *
MvpnManagerPartition::GetProjectManagerPartition() const {
    MvpnProjectManager *project_manager = manager_->GetProjectManager();
    return project_manager ? project_manager->GetPartition(part_id_) : NULL;
}

void MvpnManagerPartition::ProcessSPMSIRoute(MvpnRoute *spmsi_rt) {
    if (manager_->IsMaster())
        return;

    MvpnState::SG sg = MvpnState::SG(spmsi_rt->GetPrefix().source2(),
                                     spmsi_rt->GetPrefix().group2());
    MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    MvpnState *mvpn_state = project_manager_partition->GetState(sg);

    // Retrieve any state associcated with this S-PMSI route.
    MvpnDBState *mvpn_dbstate = dynamic_cast<MvpnDBState *>(
        spmsi_rt->GetState(manager_->table(), manager_->listener_id()));

    MvpnRoute *leaf_ad_rt = NULL;
    if (!spmsi_rt->IsValid()) {
        if (!mvpn_dbstate)
            return;

        // Delete any Type 4 LeafAD Route originated route.
        if (!mvpn_dbstate->path)
            return;
        assert(mvpn_dbstate->route);
        mvpn_dbstate->route->DeletePath(mvpn_dbstate->path);
        if (mvpn_state) {
            if (mvpn_state->leaf_ad_routes_originated_.erase(
                    mvpn_dbstate->route)) {
                project_manager_partition->DeleteState(mvpn_state);
            }
        }
    } else {
        if (!mvpn_state)
            mvpn_state = project_manager_partition->CreateState(sg);

        if (!mvpn_dbstate) {
            mvpn_dbstate = new MvpnDBState(mvpn_state);
            spmsi_rt->SetState(manager_->table(), manager_->listener_id(),
                               mvpn_dbstate);
        } else {
            leaf_ad_rt = mvpn_dbstate->route;
        }

        if (!leaf_ad_rt) {
            // Use route target <pe-router-id>:0
            leaf_ad_rt = manager_->table()->CreateLeafADRoute(spmsi_rt);
            assert(mvpn_state->leaf_ad_routes_originated_.insert(
                    leaf_ad_rt).second);
            mvpn_state->refcount_++;
        }
    }

    if (leaf_ad_rt)
        leaf_ad_rt->Notify();
}


// At the moment, we only do resolution for C-<S,G> Type-7 routes.
BgpRoute *MvpnManagerPartition::ReplicateType7SourceTreeJoin(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {

    // If src_path is not marked for resolution requested, replicate it right
    // away.
    if (!src_path->NeedsResolution()) {
        return ReplicatePath(server, src_table, src_rt, src_path, community,
                             NULL, NULL);
    }

    const BgpAttr *attr = src_path->GetAttr();
    if (!attr)
        return NULL;

    // If source is resolved, only then replicate the path, not otherwise.
    if (attr->source_rd().IsZero())
        return NULL;

    // Find if the resolved path points to an active Mvpn neighbor.
    MvpnNeighbor neighbor;
    if (!manager_->findNeighbor(attr->nexthop(), &neighbor))
        return NULL;

    // Replicate path using rd of the resolved path as part of the prefix
    // and append ASN and C-S,G also to the prefix.
    RouteDistinguisher rd(neighbor.address.to_v4().to_ulong(), neighbor.vn_id);
    MvpnPrefix mprefix(MvpnPrefix::SourceTreeJoinRoute, rd, neighbor.asn,
        src_rt->GetPrefix().group(), src_rt->GetPrefix().source());
    MvpnRoute rt_key(mprefix);

    // Find or create the route.
    DBTablePartition *rtp = static_cast<DBTablePartition *>(
        manager_->table()->GetTablePartition(&rt_key));
    BgpRoute *dest_route = static_cast<BgpRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new MvpnRoute(mprefix);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }

    BgpAttrPtr new_attr = BgpAttrPtr(src_path->GetAttr());

    // Check whether peer already has a path.
    BgpPath *dest_path = dest_route->FindSecondaryPath(src_rt,
            src_path->GetSource(), src_path->GetPeer(),
            src_path->GetPathId());
    if (dest_path != NULL) {
        if (new_attr != dest_path->GetOriginalAttr() ||
            src_path->GetFlags() != dest_path->GetFlags()) {
            bool success = dest_route->RemoveSecondaryPath(src_rt,
                src_path->GetSource(), src_path->GetPeer(),
                src_path->GetPathId());
            assert(success);
        } else {
            return dest_route;
        }
    }

    // Create replicated path and insert it on the route.
    BgpSecondaryPath *replicated_path =
        new BgpSecondaryPath(src_path->GetPeer(), src_path->GetPathId(),
                             src_path->GetSource(), new_attr,
                             src_path->GetFlags(), src_path->GetLabel());
    replicated_path->SetReplicateInfo(src_table, src_rt);
    dest_route->InsertPath(replicated_path);
    dest_route->Notify();

    return dest_route;
}

void MvpnManagerPartition::ProcessLeafADRoute(MvpnRoute *join_rt) {
    if (IsMaster())
        return;
    BgpPath *src_path = join_rt->BestPath();
    if (!dynamic_cast<BgpSecondaryPath>(src_path))
        return;

    // LeafAD route has been imported into a table. Retrieve PMSI information
    // from the path attribute and update the ingress sender (agent).
}

void MvpnManagerPartition::ProcessSourceTreeJoinRoute(MvpnRoute *join_rt) {
    BgpPath *src_path = join_rt->BestPath();

    assert(src_path);
    if (dynamic_cast<BgpSecondaryPath>(src_path)) {
        if (IsMaster())
            return;
        // Originate S-PMSI route towards the receivers.
        return;
    }

    const BgpAttr *src_path_attr = !src_path->GetAttr();
    bool resolved = src_path_attr && !src_path_attr->source_rd().IsZero();

    // Reset source rd first to mark the route as unresolved.
    src_path->set_source_rd(RouteDistinguisher());

    // In case of bgp.mvpn.0 table, if src_rt is type-7 <C-S,G> route and it is
    // now resolvable (or otherwise), replicate/delete C-S,G route to advertise/
    // withdraw from the ingress root PE node.
    const BgpPath *path = manager_->table()->manager()->resolver()->
        FindResolvedPath(join_rt, src_path);
    if (!path)
        return resolved;

    const BgpAttr *attr = path->GetAttr();
    if (!attr)
        return resolved;

    ExtCommunityPtr ext_community = attr->ext_community();
    if (!ext_community)
        return resolved;

    // Find if the resolved path points to an active Mvpn neighbor.
    MvpnNeighbor neighbor;
    if (!manager_->findNeighbor(attr->nexthop(), &neighbor))
        return resolved;

    bool rt_import_found = false;

    // Use rt-import from the resolved path as export route-target.
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &value,
                  ext_community->communities()) {
        if (ExtCommunity::is_vrf_route_import(value)) {
            ExtCommunity::ExtCommunityList export_target;
            export_target.push_back(value);
            ext_community = server->extcomm_db()->ReplaceRTargetAndLocate(
                ext_community.get(), export_target);
            rt_import_found = true;
            break;
        }
    }

    // Do not replicate if there is no vrf rt import community attached to the
    // resolved route.
    if (!rt_import_found)
        return resolved;

    // Update extended communty of the route with route-target equal to the
    // vrf import route target found above.
    src_path_attr->set_ext_community(ext_community);
    RouteDistinguisher rd(neighbor.address.to_v4().to_ulong(), neighbor.vn_id);
    src_path->set_source_rd(rd);

    // TODO(Ananth) Return false if there is no change, true otherwise.
    return true;
}

// Check if GlobalErmVpnTreeRoute is present. If so, only then can we replicate
// this path for advertisement to ingress routers with associated PMSI tunnel
// information.
BgpRoute *MvpnManagerPartition::ReplicateType4LeafAD(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {
    const BgpAttr *attr = src_path->GetAttr();

    // Do not replicate into non-master tables if the [only] route-target of the
    // route does not match the auto-created vrf-import route target of 'this'.
    if (!attr || !attr->ext_community())
        return NULL;

    // Do not replicate if there is no matching type-3 S-PMSI route.
    if (!IsMaster()) {
        bool found = false;
        BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &value,
                attr->ext_community()->communities()) {
            if (ExtCommunity::is_route_target(value)) {
                if (value == table()->GetAutoVrfImportRouteTarget()) {
                    found = true;
                    break;
                }
            }
        }

        if (!found)
            return NULL;

        // Make sure that there is an associated Type3 S-PMSI route.
        MvpnRoute *spmsi_rt = manager_->table()->FindSPMSIRoute(src_rt);
        if (!spmsi_rt)
            return NULL;

        if (src_table->IsMaster()) {
            return ReplicatePath(server, src_table, src_rt, src_path, community,
                                 new_attr, &replicated);
        }
    }

    MvpnState::SG sg = MvpnState::SG(src_rt->GetPrefix().source2(),
                                     src_rt->GetPrefix().group2());
    MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    MvpnState *mvpn_state = project_manager_partition->GetState(sg);
    assert(mvpn_state);

    uint32_t label;
    Ip4Address address;
    if (!project_manager_partition->GetLeafAdTunnelInfo(
        mvpn_state->global_ermvpn_tree_rt(), &label, &address)) {
        // TODO(Ananth) old forest node must be updated to reset input tunnel
        // attribute, if encoded.
        return NULL;
    }
    PmsiTunnelSpec *pmsi_spec = new PmsiTunnelSpec();
    pmsi_spec->tunnel_flags = 0;
    pmsi_spec->tunnel_type = PmsiTunnelSpec::IngressReplication;
    pmsi_spec->SetLabel(label);
    pmsi_spec->SetIdentifier(address);

    // Replicate the LeafAD path with appropriate PMSI tunnel info as part of
    // the path attributes. Community should be route-target with root PE
    // router-id + 0. (Page 254)
    BgpAttrPtr new_attr = server->attr_db()->ReplacePmsiTunnelAndLocate(
        src_path->GetAttr(), pmsi_spec);
    bool replicated;

    BgpRoute *replicated_path = ReplicatePath(server, src_table, src_rt,
        src_path, community, new_attr, &replicated);

    if (replicated) {
        // Notify GlobalErmVpnTreeRoute forest node so that its input tunnel
        // attributes can be updated with the MvpnNeighbor information of the
        // S-PMSI route associated with this LeadAD route.
        mvpn_state->global_ermvpn_tree_rt()->Notify();
    }

    return replicated_path;
}

BgpRoute *MvpnManagerPartition::ReplicatePath(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
    ExtCommunityPtr community,
    BgpAttrPtr new_attr, bool *replicated) {
    if (replicated)
        *replicated = false;
    assert(src_table->family() == Address::MVPN);
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(src_rt);
    assert(mvpn_rt);
    MvpnPrefix mvpn_prefix(mvpn_rt->GetPrefix());
    BgpAttrDB *attr_db = server->attr_db();

    if (!new_attr)
        new_attr = BgpAttrPtr(src_path->GetAttr());

    if (manager_->IsMaster()) {
        if (mvpn_prefix.route_distinguisher().IsZero()) {
            mvpn_prefix.set_route_distinguisher(new_attr->source_rd());
        }
    }
    MvpnRoute rt_key(mvpn_prefix);

    // Find or create the route.
    DBTablePartition *rtp = static_cast<DBTablePartition *>(
        manager_->table()->GetTablePartition(&rt_key));
    BgpRoute *dest_route = static_cast<BgpRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new MvpnRoute(mvpn_prefix);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }

    new_attr = attr_db->ReplaceExtCommunityAndLocate(new_attr.get(), community);

    // Check whether peer already has a path.
    BgpPath *dest_path = dest_route->FindSecondaryPath(src_rt,
            src_path->GetSource(), src_path->GetPeer(),
            src_path->GetPathId());
    if (dest_path != NULL) {
        if ((new_attr != dest_path->GetOriginalAttr()) ||
            (src_path->GetFlags() != dest_path->GetFlags()) ||
            (src_path->GetLabel() != dest_path->GetLabel()) ||
            (src_path->GetL3Label() != dest_path->GetL3Label())) {
            bool success = dest_route->RemoveSecondaryPath(src_rt,
                src_path->GetSource(), src_path->GetPeer(),
                src_path->GetPathId());
            assert(success);
        } else {
            return dest_route;
        }
    }

    if (replicated)
        *replicated = true;

    // Create replicated path and insert it on the route
    BgpSecondaryPath *replicated_path =
        new BgpSecondaryPath(src_path->GetPeer(), src_path->GetPathId(),
                             src_path->GetSource(), new_attr,
                             src_path->GetFlags(), src_path->GetLabel(),
                             src_path->GetL3Label());
    replicated_path->SetReplicateInfo(src_table, src_rt);
    dest_route->InsertPath(replicated_path);

    // Always trigger notification.
    dest_route->Notify();

    return dest_route;
}

MvpnState::MvpnState(const SG &sg) :
        sg_(sg), global_ermvpn_tree_rt_(NULL), refcount_(0) {
}

MvpnState::~MvpnState() {
    assert(!global_ermvpn_tree_rt_);
    assert(leaf_ad_routes_originated_.empty());
    assert(cjoin_routes_received_.empty());
}

class MvpnProjectManager::DeleteActor : public LifetimeActor {
public:
    explicit DeleteActor(MvpnProjectManager *manager)
        : LifetimeActor(manager->table_->routing_instance()->server()->
                lifetime_manager()), manager_(manager) {
    }
    virtual ~DeleteActor() {
    }

    virtual bool MayDelete() const {
        return true;
    }

    virtual void Shutdown() {
    }

    virtual void Destroy() {
    }

private:
    MvpnProjectManager *manager_;
};

MvpnProjectManager::MvpnProjectManager(MvpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MvpnProjectManager::~MvpnProjectManager() {
    Terminate();
}

void MvpnProjectManager::Initialize() {
    assert(!table_->IsMaster());
    AllocPartitions();

    listener_id_ = table_->Register(
        boost::bind(&MvpnProjectManager::RouteListener, this, _1, _2),
        "MvpnProjectManager");
}

void MvpnProjectManager::Terminate() {
    table_->Unregister(listener_id_);
    FreePartitions();
}

void MvpnProjectManager::AllocPartitions() {
    for (int part_id = 0; part_id < table_->PartitionCount(); part_id++)
        partitions_.push_back(new MvpnProjectManagerPartition(this, part_id));
}

void MvpnProjectManager::FreePartitions() {
    for (size_t part_id = 0; part_id < partitions_.size(); part_id++) {
        delete partitions_[part_id];
    }
    partitions_.clear();
}

MvpnProjectManagerPartition *MvpnProjectManager::GetPartition(int part_id) {
    return partitions_[part_id];
}

const MvpnProjectManagerPartition *MvpnProjectManager::GetPartition(
        int part_id) const {
    return partitions_[part_id];
}

void MvpnProjectManager::ManagedDelete() {
    deleter_->Delete();
}

MvpnProjectManagerPartition::MvpnProjectManagerPartition(
        MvpnProjectManager *manager, int part_id)
    : manager_(manager), part_id_(part_id) {
}

MvpnProjectManagerPartition::~MvpnProjectManagerPartition() {
}

MvpnState *MvpnProjectManagerPartition::CreateState(const SG &sg) {
    MvpnState *state = new MvpnState(sg);
    assert(states_.insert(make_pair(sg, state)).second);
    return state;
}

MvpnState *MvpnProjectManagerPartition::LocateState(const SG &sg) {
    MvpnState *mvpn_state = GetState(sg);
    return mvpn_state ?: CreateState(sg);
}

const MvpnState *MvpnProjectManagerPartition::GetState(const SG &sg) const {
    StateMap::const_iterator iter = states_.find(sg);
    return iter != states_.end() ?  iter->second : NULL;
}

MvpnState *MvpnProjectManagerPartition::GetState(const SG &sg) {
    StateMap::iterator iter = states_.find(sg);
    return iter != states_.end() ?  iter->second : NULL;
}

void MvpnProjectManagerPartition::DeleteState(MvpnState *mvpn_state) {
    assert(mvpn_state->refcount_);
    if (--mvpn_state->refcount_)
        return;
    states_.erase(mvpn_state->sg());
    delete mvpn_state;
}

void MvpnProjectManager::RouteListener(DBTablePartBase *tpart,
        DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    if (table_->IsMaster())
        return;

    ErmVpnRoute *ermvpn_route = dynamic_cast<ErmVpnRoute *>(db_entry);
    assert(ermvpn_route);
    ErmVpnTable *ermvpn_table = dynamic_cast<ErmVpnTable *>(tpart->parent());
    assert(ermvpn_table);

    // Notify all T-4 Leaf AD routes already originated for this S,G.
    if (ermvpn_table->tree_manager()->IsGlobalTreeRootRoute(ermvpn_route)) {
        MvpnProjectManagerPartition *partition = partitions_[tpart->index()];
        partition->NotifyLeafAdRoutes(ermvpn_route);
    }
}

void MvpnProjectManagerPartition::NotifyLeafAdRoutes(ErmVpnRoute *ermvpn_rt) {
    SG sg = SG(ermvpn_rt->GetPrefix().source(), ermvpn_rt->GetPrefix().group());
    MvpnState *mvpn_state = GetState(sg);
    assert(mvpn_state);

    if (!ermvpn_rt->IsValid()) {
        mvpn_state->set_global_ermvpn_tree_rt(NULL);
    } else {
        mvpn_state->set_global_ermvpn_tree_rt(ermvpn_rt);
    }

    // Notify all originated t-4 routes for PMSI re-computation.
    BOOST_FOREACH(MvpnRoute *leaf_ad_route, mvpn_state->leaf_ad_routes()) {
        leaf_ad_route->Notify();
    }
}

bool MvpnProjectManagerPartition::GetLeafAdTunnelInfo(ErmVpnRoute *rt,
    uint32_t *label, Ip4Address *address) const {
    return true;
}
