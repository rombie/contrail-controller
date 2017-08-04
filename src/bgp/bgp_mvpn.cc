/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_mvpn.h"
#include "bgp/mvpn/mvpn_table.h"

MvpnState::~MvpnState() {
    assert(!global_ermvpn_tree_rt_);
    assert(leaf_ad_routes_originated_.empty());
    assert(cjoin_routes_received_.empty());
}

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

void MvpnManager::Terminate() {
    table_->Unregister(listener_id_);
    FreePartitions();
}

bool MvpnManager::IsMaster() const {
    return table_->IsMaster();
}

string MvpnManager::GetProjectMasterName() const {
    return table_->routing_instance()->GetProjectName() + "_fabric__";
}

bool MvpnManager::IsProjectMaster() const {
    return table_->routing_instance()->name() == GetProjectMasterName();
}

const MvpnManagerPartition *MvpnManagerPartition::GetProjectManagerPartition()
        const {
    return GetProjectManagerPartition();
}

MvpnManagerPartition *MvpnManagerPartition::GetProjectManagerPartition() {
    MvpnProjectManager *project_manager = GetProjectManager();
    return project_manager ? project_manager->GetPartition(part_id_) : NULL;
}

const MvpnManager *MvpnManager::GetProjectManager() const {
    return GetProjectManager();
}

MvpnProjectManager *MvpnManager::GetProjectManager() {
    RoutingInstance *instance =  table_->routing_instance()->manager()->
        GetRoutingInstance(GetProjectMasterName());
    if (!instance)
        return NULL;
    MvpnTable *table = instance->GetTable(Address::MvPN);
    if (!table)
        return NULL;
    return table->mvpn_project_manager();
}

void MvpnManager::Initialize() {
    assert(!IsMaster());
    AllocPartitions();

    listener_id_ = table_->Register(
        boost::bind(&MvpnManager::RouteListener, this, _1, _2),
        "MvpnManager");
    }

    // Originate Type1 Internal Auto-Discovery Route.
    BgpServer *server = table_->routing_instance()->server();
    table_->CreateType1Route(server->bgp_identifier(),
                             table_->routing_instance()->index(),
                             server->autonomous_system());


    // Originate Type2 External Auto-Discovery Route.
    table_->CreateType2Route(server->bgp_identifier(),
                             table_->routing_instance()->index(),
                             server->autonomous_system());
}

void MvpnManager::RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    if (IsMaster())
        return;

    MvpnRoute *route = dynamic_cast<MvpnRoute *>(db_entry);
    if (!route)
        return;

    if (route->GetPrefix().type() == MvpnPrefix::Type1AD ||
            route->GetPrefix().type() == MvpnPrefix::Type2AD) {
        UpdateNeighbor(route);
        return;
    }

    if (route->GetPrefix().type() == MvpnPrefix::Type3SPMSI) {
        MvpnManagerPartition *partition = partitions_[tpart->index()];
        partition->ProcessSPMSIRoute();
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

void MvpnManager::GetPartition(int part_id) {
    return partitions_[part_id];
}

MvpnManagerPartition::MvpnManagerPartition(MvpnManager *manager, size_t part_id)
    : manager_(manager), part_id_(part_id) {
}

MvpnManagerPartition::~MvpnManagerPartition() {
}

bool MvpnManager::findNeighbor(const IpAddress &address, MvpnNeighbor &nbr)
        const {
    tbb:scoped_lock(neighbors_mutex_);

    NeighborsMap::iterator iter = neighbors_.find(address);
    if (iter != neighbors_.end()) {
        nbr = *iter;
        return true;
    }
    return false;
}

void MvpnManager::UpdateNeighbor(MvpnRoute *route) {
    tbb:scoped_lock(neighbors_mutex_);

    bool found = findNeighbor(route->address(), &old_neighbor);

    if (!route->IsValid()) {
        neighbors_.erase(route->address());
        if (found)
            NotifyAllRoutes();
        return;
    }

    MvpnNeighbor neighbor(route->address(), route->vn_id, route->external);
    neighbors_.insert(make_pair(route->address(), neighbor);

    if (found && old_neighbor != neighbor)
        NotifyAllRoutes();
}

void MvpnManager::NotifyAllRoutes() {
    table_->NotifyAllEntries();
}

void MvpnManagerPartition::ProcessSPMSIRoute(MvpnRoute *spmsi_rt) {
    if (manager_->IsMaster())
        return;

    SG sg = SG(spmsi_rt->source(), spmsi_rt->group());
    MvpnManagerPartition *project_manager_partition =
        GetProjectManagerPartition(sg);
    MvpnState *mvpn_state =
        project_manager_partition->findMvpnState(sg);

    // Retrieve any state associcated with this S-PMSI route.
    MvpnDBState *mvpn_dbstate = dynamic_cast<MvpnDBState *>(
        spmsi_rt->GetState(table_, listener_id_));

    MvpnRoute *leaf_ad_rt = NULL;
    if (!spmsi_rt->Isvalid()) {
        if (!mvpn_dbstate)
            return;

        // Delete any Type 4 LeafAD Route originated route.
        if (!mvpn_dbstate->path)
            return;
        assert(mvpn_dbstate->route);
        table_->DeletePath(mvpn_dbstate->route, mvpn_dbstate->path);
        if (mvpn_state) {
            if (mvpn_state->leaf_ad_routes_originated_->erase(
                    vpn_dbstate->route)) {
                project_manager_partition->DeleteState(mvpn_state);
            }
        }
    } else {
        if (!mvpn_state)
            mvpn_state = CreateState(sg);

        if (!mvpn_dbstate) {
            mvpn_dbstate = new MvpnDBState(mvpn_state);
            spmsi_rt->SetState(table_, listener_id_, mvpn_dbstate);
        } else {
            leaf_ad_rt = mvpn_dbstate->route;
        }

        if (!leaf_ad_rt) {
            leaf_ad_rt = CreateLeafADRoute(spmsi_rt);
            assert(mvpn_state->leaf_ad_routes_originated_.insert(
                leaf_ad_rt));
            mvpn_state->refcount__++;
        }
    }

    if (leaf_ad_rt)
        leaf_ad_rt->Notify();
}

BgpRoute *MvpnManagerPartition::ReplicateType12AD(MvpnTable *src_table,
    MvpnTable *src_table, MvpnRoute *source_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {
}

// At the moment, we only do resolution for C-<S,G> Type-7 routes.
BgpRoute *MvpnManagerPartition::ReplicateType7SourceTreeJoin(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *source_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {

    // For Phase1, senders are expected to be always outside the cluster.
    if (!manager_->IsMaster())
        return NULL;

    // In case of bgp.mvpn.0 table, if src_rt is type-7 <C-S,G> route and it is
    // now resolvable (or otherwise), replicate or delete C-S,G route to
    // advertise/withdraw from the ingress root PE node.
    const BgpRoute *resolved_rt = NULL;
    if (src_table->resolver()->rnexthop())
        resolved_rt = src_table->resolver()->rnexthop()->route();

    if (!resolved_rt)
        return NULL;

    const BgpPath *path =
        src_table->resolver()->FindResolvedPath(source_rt, src_path);
    if (!path)
        return NULL;

    const BgpAttr *attr = path->GetAttr();
    if (!attr)
        return NULL;

    // Find if resolved path points to an active Mvpn neighbor.
    MvpnNeighbor neighbor;
    if (!manager_->findNeighbor(path->nexthop(attr->nexthop(), neighbor)))
        return NULL;

    ExtCommunityPtr ext_community = attr->ext_community();
    if (!ext_community)
        return NULL;

    bool rt_import_found = false;
    ExtCommunity::ExtCommunityValue rt_import;

    // Use rt-import from the resolved path as export route-target.
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &value,
                  ext_community->communities()) {
        if (ExtCommunity::is_vrf_route_import(value)) {
            rt_import = value;
            rt_import_found = true;
            break;
        }
    }

    // Do not replicate if there is no vrf rt import community attached to the
    // resolved route.
    if (!rt_import_found)
        return NULL;

    // Replicate path using rd of the resolved path as part of the prefix
    // and append ASN and C-S,G also to the prefix.
    RouteDistinguisher rd(neighbor.address, neighbor.vn_id);
    MvpnPrefix mprefix(MvpnPrefix::SourceTreeJoinRoute, rd, neighbor.asn,
                       source_rt->group(), source_rt->source());
    MvpnRoute rt_key(mprefix);

    // Find or create the route.
    DBTablePartition *rtp =
        static_cast<DBTablePartition *>(table_->GetTablePartition(&rt_key));
    BgpRoute *dest_route = static_cast<BgpRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new MvpnRoute(mprefix);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }

    BgpAttrPtr new_attr =
        server->attr_db()->ReplaceExtCommunityAndLocate(src_path->GetAttr(),
                                                        rt_import);

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
    rtp->Notify(dest_route);

    return dest_route;
}

// Check if GlobalErmVpnTreeRoute is present. If so, only then can we replicate
// this path for advertisement to ingress routers with associated PMSI tunnel
// information.
void MvpnManagerPartition::ReplicateType4LeafAD(MvpnTable *src_table,
    MvpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    if (!IsMaster())
        return;

    SG sg = SG(spmsi_rt->source(), spmsi_rt->group());
    MvpnManagerPartition *project_manager_partition =
        GetProjectManagerPartition(sg);
    MvpnState *mvpn_state = project_manager_partition->findMvpnState(sg);
    assert(mvpn_state);

    uint32_t label;
    IpAddress address;
    project_manager_partition->GetLeafAdTunnelInfo(
        mvpn_state->global_ermvpn_tree_rt(), &label, &address);
    if (!pmsi)
        return NULL;
    PmsiTunnelSpec *pmsi_tunnel_spec = new PmsiTunnelSpec();
    pmsi_spec->tunnel_flags = 0;
    pmsi_spec->tunnel_type = PmsiTunnelSpec::IngressReplication;
    pmsi_spec->SetLabel(label);
    pmsi_spec->SetIdentifier(address);

    // Replicate the LeafAD path with appropriate PMSI tunnel info as part of
    // the path attributes. Community should be route-target with root PE
    // router-id + 0. (Page 254)
    new_attr = server->attr_db->ReplacePmsiTunnelAndLocate(
        src_path->GetAttr().get(), pmsi_spec);
    bool replicated;
    BgpRoute *replicated_path = ReplicatePath(src_table, source_rt, src_path,
                                              community, new_attr, &replicated);

    if (replicated) {
        // Notify GlobalErmVpnTreeRoute so that forest node.
        mvpn_state->global_ermvpn_tree_rt()->Notify();
    }

    return replicated_path;
}

BgpRoute *MvpnManagerPartition::ReplicatePath(MvpnTable *src_table,
    MvpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community,
    BgpAttrPtr new_attr, bool *replicated) {
    if (replicated)
        *replicated = false;
    assert(src_table->family() == Address::MvPN);
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(src_rt);
    assert(mvpn_rt);
    MvpnPrefix mvpn_prefix(mvpn_rt->GetPrefix());
    BgpAttrDB *attr_db = server->attr_db();

    if (!new_attr)
        new_attr = BgpAttrPtr(src_path->GetAttr());

    if (IsMaster()) {
        if (mvpn_prefix.route_distinguisher().IsZero()) {
            mvpn_prefix.set_route_distinguisher(new_attr->source_rd());
        }
    }
    MvpnRoute rt_key(mvpn_prefix);

    // Find or create the route.
    DBTablePartition *rtp =
        static_cast<DBTablePartition *>(GetTablePartition(&rt_key));
    BgpRoute *dest_route = static_cast<BgpRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new MvpnRoute(mvpn_prefix);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }

    new_attr = attr_db->ReplaceExtCommunityAndLocate(new_attr.get(), community);

    // Check whether peer already has a path
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
    rtp->Notify(dest_route);

    return dest_route;
}

BgpRoute *MvpnManager::RouteReplicate(BgpServer *server, BgpTable *src_table,
    BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    CHECK_CONCURRENCY("db::DBTable");
    MvpnRoute *source_mvpn_rt = dynamic_cast<MvpnRoute *>(source_rt);

    // Phase 1 support for only senders outside the cluster and receivers inside
    // the cluster. Hence don't replicate to a VRF from other VRF tables.
    if (!IsMaster()) {
        MvpnTable *src_mvpn_table = dynamic_cast<MvpnTable *>(src_table);
        if (!src_mvpn_table->IsMaster())
            return NULL;
    }

    if (source_rt->IsType1() || source_rt->IsType2()) {
        return ReplicateType12AD(dynamic_cast <MvpnTable *>(src_table),
            dynamic_cast<MvpnRoute *>(source_rt), src_path, community);
    }

    MvpnManagerPartition *mvpn_manager_partition =
        GetPartitionManager(source_rt->index());

    if (source_rt->IsType7()) {
        return mvpn_manager_partition->ReplicateType7SourceTreeJoin(
            dynamic_cast <MvpnTable *>(src_table),
            dynamic_cast<MvpnRoute *>(source_rt), src_path, community);
    }

    if (source_rt->IsType4()) {
        return mvpn_manager_partition->ReplicateType4LeafAd(
            dynamic_cast <MvpnTable *>(src_table),
            dynamic_cast<MvpnRoute *>(source_rt), src_path, community);
    }

    return mvpn_manager_partition->ReplicatePath(
        dynamic_cast <MvpnTable *>(src_table),
        dynamic_cast<MvpnRoute *>(source_rt), src_path, community);
    }
}

void MvpnManager::ResolvePath(BgpRoute *rt, BgpPath *path) {
    MvpnRoute *mvpn_route = static_cast<MvpnRoute *>(rt);
    BgpTable *table = rtinstance_->GetTable(Address::INET);
    table->path_resolver()->StartPathResolution(
        rt->get_table_partition()->index(), path, rt, table, mvpn_rt->source());
}
