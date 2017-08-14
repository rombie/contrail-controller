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
#include "bgp/extended-community/vrf_route_import.h"
#include "bgp/mvpn/mvpn_table.h"
#include "bgp/routing-instance/path_resolver.h"
#include "bgp/routing-instance/routing_instance.h"

using std::make_pair;
using std::ostringstream;
using std::string;

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
        manager_->table_->DestroyMvpnProjectManager();
    }

private:
    MvpnProjectManager *manager_;
};

MvpnProjectManager::MvpnProjectManager(ErmVpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MvpnProjectManager::~MvpnProjectManager() {
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
    tbb::reader_writer_lock::scoped_lock_read lock(neighbors_mutex_);

    NeighborsMap::const_iterator iter = neighbors_.find(address);
    if (iter != neighbors_.end()) {
        *nbr = iter->second;
        return true;
    }
    return false;
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

ErmVpnRoute *MvpnState::global_ermvpn_tree_rt() {
    return global_ermvpn_tree_rt_;
}

const ErmVpnRoute *MvpnState::global_ermvpn_tree_rt() const {
    return global_ermvpn_tree_rt_;
}

const MvpnState::RoutesSet &MvpnState::leaf_ad_routes() const {
    return leaf_ad_routes_originated_;
}

MvpnState::RoutesSet *MvpnState::leaf_ad_routes() {
    return &leaf_ad_routes_originated_;
}

const MvpnState::RoutesSet &MvpnState::cjoin_routes_received() const {
    return cjoin_routes_received_;
}

MvpnState::RoutesSet *MvpnState::cjoin_routes_received() {
    return &cjoin_routes_received_;
}

void MvpnState::set_global_ermvpn_tree_rt(ErmVpnRoute *global_ermvpn_tree_rt) {
    global_ermvpn_tree_rt = global_ermvpn_tree_rt_;
}

MvpnDBState::MvpnDBState() : state(NULL), route(NULL) {
    if (state)
        state->refcount_++;
}

MvpnDBState::MvpnDBState(MvpnState *state) : state(state) , route(NULL) {
    if (state)
        state->refcount_++;
}

MvpnDBState::MvpnDBState(MvpnRoute *route) : state(NULL) , route(route) {
    if (state)
        state->refcount_++;
}

MvpnDBState::MvpnDBState(MvpnState *state, MvpnRoute *route) :
        state(state) , route(route) {
    if (state)
        state->refcount_++;
}

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
        manager_->table_->DestroyManager();
    }

private:
    MvpnManager *manager_;
};

MvpnManager::MvpnManager(MvpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MvpnManager::~MvpnManager() {
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

PathResolver *MvpnManager::path_resolver() {
    return table_->path_resolver();
}

PathResolver *MvpnManager::path_resolver() const {
    return table_->path_resolver();
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

void MvpnManager::ManagedDelete() {
    deleter_->Delete();
}

bool MvpnManager::deleted() const {
    return deleter_->IsDeleted();
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

void MvpnManager::NotifyAllRoutes() {
    table_->NotifyAllEntries();
}

MvpnTable *MvpnManagerPartition::table() {
    return manager_->table();
}

const MvpnTable *MvpnManagerPartition::table() const {
    return manager_->table();
}

bool MvpnManagerPartition::IsMaster() {
    return table()->IsMaster();
}

bool MvpnManagerPartition::IsMaster() const {
    return table()->IsMaster();
}

int MvpnManagerPartition::listener_id() const {
    return manager_->listener_id();
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

// Call const version of the GetProjectManager().
// Use C++ casts to call const version of the same and avoid code duplication!
MvpnProjectManager *MvpnManager::GetProjectManager() {
    return const_cast<MvpnProjectManager *>(
        static_cast<const MvpnManager *>(this)->GetProjectManager());
}

MvpnState *MvpnManagerPartition::LocateState(MvpnRoute *rt) {
    MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return NULL;
    MvpnState::SG sg = MvpnState::SG(rt->GetPrefix().sourceIpAddress(),
                                     rt->GetPrefix().groupIpAddress());
    return project_manager_partition->GetState(sg);
}

const MvpnState *MvpnManagerPartition::GetState(MvpnRoute *rt) const {
    const MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return NULL;
    MvpnState::SG sg = MvpnState::SG(rt->GetPrefix().sourceIpAddress(),
                                     rt->GetPrefix().groupIpAddress());
    return project_manager_partition->GetState(sg);
}

MvpnState *MvpnManagerPartition::GetState(MvpnRoute *rt) {
    return const_cast<MvpnState *>(
        static_cast<const MvpnManagerPartition *>(this)->GetState(rt));
}

const MvpnState *MvpnManagerPartition::GetState(ErmVpnRoute *rt) const {
    const MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return NULL;
    MvpnState::SG sg = MvpnState::SG(rt->GetPrefix().source(),
                                     rt->GetPrefix().group());
    return project_manager_partition->GetState(sg);
}

MvpnState *MvpnManagerPartition::GetState(ErmVpnRoute *rt) {
    return const_cast<MvpnState *>(
        static_cast<const MvpnManagerPartition *>(this)->GetState(rt));
}

void MvpnManagerPartition::DeleteState(MvpnState *state) {
    MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return;
    project_manager_partition->DeleteState(state);
}

bool MvpnProjectManagerPartition::GetLeafAdTunnelInfo(ErmVpnRoute *rt,
    uint32_t *label, Ip4Address *address) const {
    return true;
}

////////////////////////////////////////////////////////////////////////////////

// Get MvpnProjectManager object for this Mvpn. Each MVPN network is associated
// with a parent project maanger network via configuration. MvpnProjectManager
// is retrieved from this parent network RoutingInstance object.
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
    ErmVpnTable *table =
        dynamic_cast<ErmVpnTable *>(rtinstance->GetTable(Address::ERMVPN));
    if (!table || table->IsDeleted())
        return NULL;
    return table->mvpn_project_manager();
}

// Initialize MvpnManager by allcating one MvpnManagerPartition for each DB
// partition, and register a route listener for the MvpnTable.
void MvpnManager::Initialize() {
    assert(!IsMaster());
    AllocPartitions();

    listener_id_ = table_->Register(
        boost::bind(&MvpnManager::RouteListener, this, _1, _2),
        "MvpnManager");

    // Originate Type1 Intra AS Auto-Discovery Route.
    table_->LocateType1ADRoute();

    // Originate Type2 Inter AS Auto-Discovery Route.
    table_->LocateType2ADRoute();
}

// MvpnTable route listener callback function.
//
// Process changes (create/update/delete) to all different types of MvpnRoute.
void MvpnManager::RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    // We only care about vrf[s].vpn.0 tables here.
    if (IsMaster())
        return;

    MvpnRoute *route = dynamic_cast<MvpnRoute *>(db_entry);
    assert(route);

    MvpnManagerPartition *partition = partitions_[tpart->index()];

    // Process Type1 Intra-AS and Type2 Inter-AS routes.
    if (route->GetPrefix().type() == MvpnPrefix::IntraASPMSIADRoute ||
            route->GetPrefix().type() == MvpnPrefix::InterASPMSIADRoute) {
        UpdateNeighbor(route);
        return;
    }

    // Process Type3 S-PMSI route.
    if (route->GetPrefix().type() == MvpnPrefix::SPMSIADRoute) {
        partition->ProcessType3SPMSIRoute(route);
        return;
    }

    // Process Type7 C-Join route.
    if (route->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute) {
        if (partition->ProcessType7SourceTreeJoinRoute(route))
            route->front() ? route->Notify() : route->Delete();
        return;
    }

    // Process Type4 LeadAD route.
    if (route->GetPrefix().type() == MvpnPrefix::LeafADRoute) {
        partition->ProcessType4LeafADRoute(route);
        return;
    }
}

// Update MVPN neighbor list with create/delete/update of auto-discovery routes.
//
// Protect access to neighbors_ map with a mutex as the same be 'read' off other
// DB tasks in parallel. (Type-1 and Type-2 do not carrry any <S,G> information.
void MvpnManager::UpdateNeighbor(MvpnRoute *route) {
    // TODO(Ananth) Shouldn't this be best-path's nexthop address ?
    IpAddress address = route->GetPrefix().originatorIpAddress();
    RouteDistinguisher rd = route->GetPrefix().route_distinguisher();

    // Check if an entry is already present.
    MvpnNeighbor old_neighbor;
    bool found = findNeighbor(address, &old_neighbor);

    if (!route->IsValid()) {
        if (found) {
            tbb::reader_writer_lock::scoped_lock lock(neighbors_mutex_);
            neighbors_.erase(address);
            NotifyAllRoutes();
        }
        return;
    }

    MvpnNeighbor neighbor(address, route->GetPrefix().asn(), rd.GetVrfId(),
        route->GetPrefix().type() == MvpnPrefix::InterASPMSIADRoute);

    // Ignore if there is no change.
    if (found && old_neighbor == neighbor)
        return;

    tbb::reader_writer_lock::scoped_lock lock(neighbors_mutex_);
    if (found)
        neighbors_.erase(address);
    assert(neighbors_.insert(make_pair(address, neighbor)).second);

    // TODO(Ananth) Only need to re-evaluate all type-7 join routes.
    NotifyAllRoutes();
}

// ErmVpnTable route listener callback function.
//
// Process changes (create/update/delete) to GlobalErmVpnRoute.
void MvpnProjectManager::RouteListener(DBTablePartBase *tpart,
        DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    // We only care about vrf[s].vpn.0 tables here.
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

// Notify all LeafAD routes for re-evaluation during replication if the
// associated GlobalErmVpnRoute has gotten changed.
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
    BOOST_FOREACH(MvpnRoute *leaf_ad_route, *(mvpn_state->leaf_ad_routes())) {
        leaf_ad_route->Notify();
    }
}

void MvpnManager::ResolvePath(RoutingInstance *rtinstance, BgpRoute *rt,
        BgpPath *path) {
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(rt);
    assert(mvpn_rt->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute);

    IpAddress address = mvpn_rt->GetPrefix().sourceIpAddress();
    BgpTable *table = address.is_v4() ? rtinstance->GetTable(Address::INET) :
                                        rtinstance->GetTable(Address::INET6);
    path_resolver()->StartPathResolution(rt, path, table, &address);
}

bool MvpnManager::FindResolvedNeighbor(MvpnRoute *src_rt,
        const BgpPath *src_path, MvpnNeighbor *neighbor,
        ExtCommunity::ExtCommunityValue *rt_import) const {
    const BgpPath *path = path_resolver()->FindResolvedPath(src_rt, src_path);
    if (!path)
        return false;

    const BgpAttr *attr = path->GetAttr();
    if (!attr)
        return false;

    if (!attr->ext_community())
        return false;

    bool vrf_route_import_found = false;
    ExtCommunity::ExtCommunityValue rt_import_s;
    if (!rt_import)
        rt_import = &rt_import_s;

    // Use rt-import from the resolved path as export route-target.
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &value,
                  attr->ext_community()->communities()) {
        if (ExtCommunity::is_vrf_route_import(value)) {
            vrf_route_import_found = true;
            *rt_import = value;
            break;
        }
    }

    if (!vrf_route_import_found)
        return false;

    VrfRouteImport vrf_import(*rt_import);

    // Find if the resolved path points to an active Mvpn neighbor based on the
    // IP address encoded inside the vrf import route target extended community.
    if (!findNeighbor(vrf_import.GetIPv4Address(), neighbor))
        return false;

    return true;
}

bool MvpnManagerPartition::ProcessType7SourceTreeJoinRoute(MvpnRoute *join_rt) {
    MvpnDBState *mvpn_dbstate = dynamic_cast<MvpnDBState *>(
        join_rt->GetState(table(), listener_id()));

    if (!mvpn_dbstate && !join_rt->IsValid())
        return false;

    if (!join_rt->IsValid()) {
        MvpnState *state = GetState(join_rt);
        if (state)
            state->cjoin_routes_received()->erase(join_rt);
        if (mvpn_dbstate) {
            // Delete any S-PMSI route originated earlier as there is no
            // interested receivers for this route (S,G).
            if (mvpn_dbstate->route) {
                BgpPath *path = mvpn_dbstate->route->FindPath(NULL);
                if (path)
                    mvpn_dbstate->route->DeletePath(path);
                mvpn_dbstate->route = NULL;
            }
            join_rt->ClearState(table(), listener_id());
            return true;
        }
        return false;
    }

    MvpnState *state = LocateState(join_rt);
    const BgpPath *path = join_rt->BestPath();
    assert(!path);

    if (!mvpn_dbstate) {
        mvpn_dbstate = new MvpnDBState(state);
        join_rt->SetState(table(), listener_id(), mvpn_dbstate);
    }

    if (dynamic_cast<const BgpSecondaryPath *>(path)) {
        if (IsMaster())
            return false;

        // Originate/Update S-PMSI route towards the receivers.
        if (!mvpn_dbstate->route)
            mvpn_dbstate->route = table()->LocateType3SPMSIRoute(join_rt);
        return true;
    }

    // Maintain list of all primary Type7 cjoin routes.
    state->cjoin_routes_received()->insert(join_rt);
    const BgpAttr *src_path_attr = path->GetAttr();
    bool resolved = src_path_attr && !src_path_attr->source_rd().IsZero();

    // Reset source rd first to mark the route as unresolved.
    BgpAttrPtr new_attr = table()->server()->attr_db()->
        ReplaceSourceRdAndLocate(src_path_attr, RouteDistinguisher());
    const_cast<BgpPath *>(path)->SetAttr(new_attr);

    // Find if the resolved path points to an active Mvpn neighbor.
    MvpnNeighbor neighbor;
    ExtCommunity::ExtCommunityValue rt_import;
    if (!manager_->FindResolvedNeighbor(join_rt, path, &neighbor, &rt_import))
        return resolved;

    ExtCommunity::ExtCommunityList export_target;
    export_target.push_back(rt_import);
    ExtCommunityPtr ext_community =
        table()->server()->extcomm_db()->ReplaceRTargetAndLocate(
            new_attr->ext_community(), export_target);

    // Update extended communty of the route with route-target equal to the
    // vrf import route target found above.
    const_cast<BgpAttr *>(src_path_attr)->set_ext_community(ext_community);
    RouteDistinguisher rd(neighbor.address.to_v4().to_ulong(), neighbor.vn_id);
    new_attr = table()->server()->attr_db()->
        ReplaceSourceRdAndLocate(path->GetAttr(), rd);
    new_attr = table()->server()->attr_db()->
        ReplaceExtCommunityAndLocate(new_attr.get(), ext_community);
    const_cast<BgpAttr *>(src_path_attr)->set_source_rd(rd);

    // Ignore if there is no chage in the computed path attributes.
    if (new_attr.get() == src_path_attr)
        return false;
    const_cast<BgpPath *>(path)->SetAttr(new_attr);
    return true;
}

void MvpnManagerPartition::ProcessType3SPMSIRoute(MvpnRoute *spmsi_rt) {
    if (IsMaster())
        return;

    MvpnState::SG sg = MvpnState::SG(spmsi_rt->GetPrefix().sourceIpAddress(),
                                     spmsi_rt->GetPrefix().groupIpAddress());
    MvpnProjectManagerPartition *project_manager_partition =
        GetProjectManagerPartition();
    if (!project_manager_partition)
        return;

    // Retrieve any state associcated with this S-PMSI route.
    MvpnDBState *mvpn_dbstate = dynamic_cast<MvpnDBState *>(
        spmsi_rt->GetState(table(), listener_id()));

    MvpnRoute *leaf_ad_rt = NULL;
    if (!spmsi_rt->IsValid()) {
        if (!mvpn_dbstate)
            return;
        MvpnState *mvpn_state = GetState(spmsi_rt);
        assert(mvpn_dbstate->state == mvpn_state);
        if (mvpn_dbstate->route) {
            BgpPath *path = mvpn_dbstate->route->FindPath(NULL);
            if (path)
                mvpn_dbstate->route->DeletePath(path);
            mvpn_dbstate->route = NULL;
        }

        assert(mvpn_state->leaf_ad_routes_originated_.erase(
                   mvpn_dbstate->route));
        spmsi_rt->ClearState(table(), listener_id());
        DeleteState(mvpn_state);
    } else {
        MvpnState *mvpn_state = LocateState(spmsi_rt);
        if (!mvpn_state)
            mvpn_state = project_manager_partition->CreateState(sg);

        if (!mvpn_dbstate) {
            mvpn_dbstate = new MvpnDBState(mvpn_state);
            spmsi_rt->SetState(table(), listener_id(), mvpn_dbstate);
        } else {
            leaf_ad_rt = mvpn_dbstate->route;
        }

        if (!leaf_ad_rt) {
            // Use route target <pe-router-id>:0
            leaf_ad_rt = table()->LocateType4LeafADRoute(spmsi_rt);
            mvpn_dbstate->route = leaf_ad_rt;
            assert(mvpn_state->leaf_ad_routes_originated_.insert(leaf_ad_rt).
                    second);
            mvpn_state->refcount_++;

            ExtCommunity::ExtCommunityValue rt_import;
            ExtCommunity::ExtCommunityList export_target;
            export_target.push_back(rt_import);
            ExtCommunityPtr ext_community =
                table()->server()->extcomm_db()->ReplaceRTargetAndLocate(NULL,
                    export_target);
        }
    }

    if (!leaf_ad_rt)
        return;

    if (!leaf_ad_rt->front()) {
        leaf_ad_rt->Delete();
    } else {
        leaf_ad_rt->Notify();
    }
}

void MvpnManagerPartition::ProcessType4LeafADRoute(MvpnRoute *leaf_ad) {
    if (IsMaster())
        return;
    const BgpPath *src_path = leaf_ad->BestPath();
    if (!dynamic_cast<const BgpSecondaryPath *>(src_path))
        return;

    // LeafAD route has been imported into a table. Retrieve PMSI information
    // from the path attribute and update the ingress sender (agent).
}

////////////////////////////////////////////////////////////////////////////////
///                     Route Replication Functions                          ///
////////////////////////////////////////////////////////////////////////////////

BgpRoute *MvpnManager::RouteReplicate(BgpServer *server, BgpTable *table,
    BgpRoute *rt, const BgpPath *src_path, ExtCommunityPtr community) {
    CHECK_CONCURRENCY("db::DBTable");

    MvpnRoute *src_rt = dynamic_cast<MvpnRoute *>(rt);
    MvpnTable *src_table = dynamic_cast<MvpnTable *>(table);

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

    return mvpn_manager_partition->ReplicatePath(server, src_rt->GetPrefix(),
            src_table, src_rt, src_path, community);
}

// At the moment, we only do resolution for C-<S,G> Type-7 routes.
BgpRoute *MvpnManagerPartition::ReplicateType7SourceTreeJoin(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {

    // If src_path is not marked for resolution requested, replicate it right
    // away.
    if (!src_path->NeedsResolution()) {
        return ReplicatePath(server, src_rt->GetPrefix(), src_table, src_rt,
                src_path, community);
    }

    const BgpAttr *attr = src_path->GetAttr();
    if (!attr)
        return NULL;

    // If source is resolved, only then replicate the path, not otherwise.
    if (attr->source_rd().IsZero())
        return NULL;

    // Find if the resolved path points to an active Mvpn neighbor.
    MvpnNeighbor neighbor;
    if (!manager_->FindResolvedNeighbor(src_rt, src_path, &neighbor))
        return NULL;

    // Replicate path using <C-S,G>, source_rd and mvpn neighbror ASN as part
    // if the Type-7 prefix.
    MvpnPrefix prefix(MvpnPrefix::SourceTreeJoinRoute, attr->source_rd(),
                      neighbor.asn, src_rt->GetPrefix().group(),
                      src_rt->GetPrefix().source());
    return ReplicatePath(server, prefix, src_table, src_rt, src_path,
                         community);
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
        MvpnRoute *spmsi_rt = table()->FindSPMSIRoute(src_rt);
        if (!spmsi_rt)
            return NULL;

        if (src_table->IsMaster()) {
            return ReplicatePath(server, src_rt->GetPrefix(), src_table,
                                 src_rt, src_path, community, attr);
        }
    }

    MvpnState *mvpn_state = GetState(src_rt);
    assert(mvpn_state);

    // Do not replicate if there is no receiver interested for this <S,G>.
    if (mvpn_state->cjoin_routes_received()->empty())
        return NULL;

    uint32_t label;
    Ip4Address address;
    if (!GetProjectManagerPartition()->GetLeafAdTunnelInfo(
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
    // router-id + 0 (Page 254).
    BgpAttrPtr new_attr = server->attr_db()->ReplacePmsiTunnelAndLocate(
        src_path->GetAttr(), pmsi_spec);
    bool replicated;

    BgpRoute *replicated_path = ReplicatePath(server, src_rt->GetPrefix(),
            src_table, src_rt, src_path, community, new_attr, &replicated);

    if (replicated) {
        // Notify GlobalErmVpnTreeRoute forest node so that its input tunnel
        // attributes can be updated with the MvpnNeighbor information of the
        // S-PMSI route associated with this LeadAD route.
        mvpn_state->global_ermvpn_tree_rt()->Notify();
    }

    return replicated_path;
}

BgpRoute *MvpnManagerPartition::ReplicatePath(BgpServer *server,
        const MvpnPrefix &prefix, MvpnTable *src_table, MvpnRoute *src_rt,
        const BgpPath *src_path, ExtCommunityPtr community, BgpAttrPtr new_attr,
        bool *replicated) {
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(src_rt);
    assert(mvpn_rt);
    MvpnPrefix mvpn_prefix(mvpn_rt->GetPrefix());
    BgpAttrDB *attr_db = server->attr_db();
    assert(src_table->family() == Address::MVPN);

    if (replicated)
        *replicated = false;

    if (!new_attr)
        new_attr = BgpAttrPtr(src_path->GetAttr());

    // Find or create the route.
    MvpnRoute rt_key(prefix);
    DBTablePartition *rtp = static_cast<DBTablePartition *>(
        table()->GetTablePartition(&rt_key));
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
