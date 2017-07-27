/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_mvpn.h"
#include "bgp/mvpn/mvpn_table.h"

MVpnState::~MVpnState() {
    assert(!global_ermvpn_tree_rt_);
    assert(leaf_ad_routes_originated_.empty());
    assert(cjoin_routes_received_.empty());
}

MVpnManager::MVpnManager(MVpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          resolver_(new PathResolver(table, true)),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MVpnManager::~MVpnManager() {
    Terminate();
}

void MVpnManager::Terminate() {
    table_->Unregister(listener_id_);
    FreePartitions();
}

bool MVpnManager::IsMaster() const {
    return table_->IsMaster();
}

string MVpnManager::GetProjectMasterName() const {
    return table_->routing_instance()->GetProjectName() + "_fabric__";
}

bool MVpnManager::IsProjectMaster() const {
    return table_->routing_instance()->name() == GetProjectMasterName();
}

const MVpnManagerPartition *MVpnManagerPartition::GetProjectManagerPartition()
        const {
    return GetProjectManagerPartition();
}

MVpnManagerPartition *MVpnManagerPartition::GetProjectManagerPartition() {
    MVpnManager *project_manager = GetProjectManager();
    return project_manager ? project_manager->GetPartition(part_id_) : NULL;
}

const MVpnManager *MVpnManager::GetProjectManager() const {
    return GetProjectManager();
}

MVpnManager *MVpnManager::GetProjectManager() {
    RoutingInstance *instance =  table_->routing_instance()->manager()->
        GetRoutingInstance(GetProjectMasterName());
    if (!instance)
        return NULL;
    MvpnTable *table = instance->GetTable(Address::MVPN);
    if (!table)
        return NULL;
    return table->mvpn_manager();
}

void MVpnManager::Initialize() {
    assert(!IsMaster());
    AllocPartitions();

    listener_id_ = table_->Register(
        boost::bind(&MVpnManager::RouteListener, this, _1, _2),
        "MVpnManager");
    }

    if (IsProjectMaster())
        return;

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

void MVpnManager::RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    if (IsMaster())
        return;

    ErmVpnRoute *ermvpn_route = dynamic_cast<ErmVpnRoute *>(db_entry);
    if (ermvpn_route) {
        ErmVpnTable *ermvpn_table =
            dynamic_cast<ErmVpnTable *>(tpart->parent());
        assert(ermvpn_table);

        // Notify all T-4 Leaf AD routes already originated for this S,G.
        if (ermvpn_table->IsGlobalTreeRootRoute(route)) {
            MVpnManagerPartition *partition = partitions_[tpart->index()];
            partition->NotifyLeafAdRoutes(route);
        }
        return;
    }

    MVpnRoute *route = dynamic_cast<MVpnRoute *>(db_entry);
    if (!route)
        return;

    if (route->GetPrefix().type() == MVpnPrefix::Type1AD ||
            route->GetPrefix().type() == MVpnPrefix::Type2AD) {
        UpdateNeighbor(route);
        return;
    }

    if (route->GetPrefix().type() == MVpnPrefix::Type3SPMSI) {
        MVpnManagerPartition *partition = partitions_[tpart->index()];
        partition->ProcessSPMSIRoute();
        return;
    }
}

void MVpnManagerPartition::NotifyLeafAdRoutes(ErmVpnRoute *ermvpn_rt) {
    SG sg = SG(ermvpn_rt->source(), ermvpn_rt->group());
    MVpnState *mvpn_state = GetState(sg);
    assert(mvpn_state);

    if (!ermvpn_rt->IsValid()) {
        mvpn_state->global_ermvpn_tree_rt_ = NULL;
    } else {
        mvpn_state->global_ermvpn_tree_rt_ = ermvpn_rt;
    }

    // Notify all originated t-4 routes for PMSI re-computation.
    BOOST_FOREACH(MVpnRoute *leaf_ad_route, mvpn_state->leaf_ad_routes()) {
        leaf_ad_route->Notify();
    }
}

void MVpnManager::AllocPartitions() {
    for (int part_id = 0; part_id < table_->PartitionCount(); part_id++)
        partitions_.push_back(new MVpnManagerPartition(this, part_id));
}

void MVpnManager::FreePartitions() {
    for (size_t part_id = 0; part_id < partitions_.size(); part_id++) {
        delete partitions_[part_id];
    }
    partitions_.clear();
}

void MVpnManager::GetPartition(int part_id) {
    return partitions_[part_id];
}

// Constructor for MVpnManagerPartition.
MVpnManagerPartition::MVpnManagerPartition(MVpnManager *manager, size_t part_id)
    : manager_(manager), part_id_(part_id) {
}

// Destructor for MVpnManagerPartition.
MVpnManagerPartition::~MVpnManagerPartition() {
}

MVpnState *MVpnManagerPartition::CreateState(const SG &sg) {
    MVpnState *state = new MVpnState();
    assert(routes_state_.insert(make_pair(sg, state)).second);
    return state;
}

MVpnState *MVpnManagerPartition::LocateState(const SG &sg) {
    MVpnState *mvpn_state = GetState(sg);
    return mvpn_state ?: CreateState(sg);
}

const MVpnState *MVpnManagerPartition::GetState(const SG &sg) const {
    RoutesStateMap::const_iterator iter = routes_state_.find(sg);
    retirn iter != routes_state_.end() ?  *iter : NULL;
}

MVpnState *MVpnManagerPartition::GetState(const SG &sg) {
    RoutesStateMap::iterator iter = routes_state_.find(sg);
    retirn iter != routes_state_.end() ?  *iter : NULL;
}

MVpnState *MVpnManagerPartition::DeleteState(MVpnState *mvpn_sg_state) {
    assert(mvpn_sg_state->refcount);
    if (--mvpn_sg_state->refcount)
        return;
    states_.erase(mvpn_sg_state->sg());
    delete mvpn_sg_state;
}

bool MVpnManager::findNeighbor(const IpAddress &address, MVpnNeighbor &nbr)
        const {
    tbb:scoped_lock(neighbors_mutex_);

    NeighborsMap::iterator iter = neighbors_.find(address);
    if (iter != neighbors_.end()) {
        nbr = *iter;
        return true;
    }
    return false;
}

void MVpnManager::UpdateNeighbor(MVpnRoute *route) {
    tbb:scoped_lock(neighbors_mutex_);

    bool found = findNeighbor(route->address(), &old_neighbor);

    if (!route->IsValid()) {
        neighbors_.erase(route->address());
        if (found)
            NotifyAllRoutes();
        return;
    }

    MVpnNeighbor neighbor(route->address(), route->vn_id, route->external);
    neighbors_.insert(make_pair(route->address(), neighbor);

    if (found && old_neighbor != neighbor)
        NotifyAllRoutes();
}

void MVpnManager::NotifyAllRoutes() {
    table_->NotifyAllEntries();
}

void MVpnManagerPartition::ProcessSPMSIRoute(MVpnRoute *spmsi_rt) {
    if (manager_->IsMaster())
        return;

    SG sg = SG(spmsi_rt->source(), spmsi_rt->group());
    MVpnManagerPartition *project_manager_partition =
        GetProjectManagerPartition(sg);
    MVpnState *mvpn_sg_state =
        project_manager_partition->findMVpnState(sg);

    // Retrieve any state associcated with this S-PMSI route.
    MVpnDBState *mvpn_dbstate = dynamic_cast<MVpnDBState *>(
        spmsi_rt->GetState(table_, listener_id_));

    MVpnRoute *leaf_ad_rt = NULL;
    if (!spmsi_rt->Isvalid()) {
        if (!mvpn_dbstate)
            return;

        // Delete any Type 4 LeafAD Route originated route.
        if (!mvpn_dbstate->path)
            return;
        assert(mvpn_dbstate->route);
        table_->DeletePath(mvpn_dbstate->route, mvpn_dbstate->path);
        if (mvpn_sg_state) {
            if (mvpn_sg_state->leaf_ad_routes_originated_->erase(
                    vpn_dbstate->route)) {
                project_manager_partition->DeleteState(mvpn_sg_state);
            }
        }
    } else {
        if (!mvpn_sg_state)
            mvpn_sg_state = CreateState(sg);

        if (!mvpn_dbstate) {
            mvpn_dbstate = new MVpnDBState(mvpn_sg_state);
            spmsi_rt->SetState(table_, listener_id_, mvpn_dbstate);
        } else {
            leaf_ad_rt = mvpn_dbstate->route;
        }

        if (!leaf_ad_rt) {
            leaf_ad_rt = CreateLeafADRoute(spmsi_rt);
            assert(mvpn_sg_state->leaf_ad_routes_originated_.insert(
                leaf_ad_rt));
            mvpn_sg_state->refcount__++;
        }
    }

    if (leaf_ad_rt)
        leaf_ad_rt->Notify();
}

BgpRoute *MVpnManagerPartition::ReplicateType12AD(MVpnTable *src_table,
    MVpnTable *src_table, MVpnRoute *source_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {
}

// At the moment, we only do resolution for C-<S,G> Type-7 routes.
BgpRoute *MVpnManagerPartition::ReplicateType7SourceTreeJoin(
    MVpnTable *src_table, MVpnRoute *source_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {

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

    // Find if resolved path points to an active MVpn neighbor.
    MVpnNeighbor neighbor;
    if (!findNeighbor(path->nexthop(path->nexthop(), &neighbor)))
        return NULL;

    const BgpAttr *attr = p->GetAttr();
    const ExtCommunity *ext_community = attr->ext_community();
    if (!ext_community)
        return NULL;

    bool rt_import_found = false;
    // Use rt-import from the resolved path as export route-target.
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                  ext_community->communities()) {
        if (ExtCommunity::is_route_import_target(comm)) {
            rt_import_found = true;
            break NULL;
        }
    }

    if (!rt_import_found)
        return NULL;

    // Replicate path using rd of the resolved path as part of the prefix
    // and append ASN and C-S,G also to the prefix.
    RouteDistinguisher rd = path->GetRouteDistinguisher();
    return replicated_rt;
}

// No special check is necessary. Type-3 S-PMSI routes can be replicated readily
// into secondary tables. When this route is replicated, a new Type-4 route is
// originated by the MVpnManager.
BgpRoute *MVpnManagerPartition::ReplicateType3SPMSIPath(MVpnTable *src_table,
    MVpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
}

// Check if GlobalErmVpnTreeRoute is present. If so, only then can we replicate
// this path for advertisement to ingress routers with associated PMSI tunnel
// information.
void MVpnManagerPartition::ReplicateType4LeafAD(MVpnTable *src_table,
    MVpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    if (!manager_->IsMaster())
        return;

    SG sg = SG(spmsi_rt->source(), spmsi_rt->group());
    MVpnManagerPartition *project_manager_partition =
        GetProjectManagerPartition(sg);
    MVpnState *mvpn_sg_state =
        project_manager_partition->findMVpnState(sg);
    assert(mvpn_sg_state);

    PMSITunnelInfo *pmsi = project_manager_partition->GetLeafAdTunnelInfo(
        mvpn_sg_state->global_ermvpn_tree_rt_);
    if (!pmsi)
        return NULL;

    // Replicate the LeafAD path with appropriate PMSI tunnel info as part of
    // the path attributes.
}

BgpRoute *MVpnManager::RouteReplicate(BgpServer *server, BgpTable *src_table,
    BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    CHECK_CONCURRENCY("db::DBTable");
    MVpnRoute *source_mvpn_rt = dynamic_cast<MVpnRoute *>(source_rt);

    // Phase 1 support for only senders outside the cluster and receivers inside
    // the cluster. Hence don't replicate to a VRF from other VRF tables.
    if (!IsMaster()) {
        MVpnTable *src_mvpn_table = dynamic_cast<MVpnTable *>(src_table);
        if (!src_mvpn_table->IsMaster())
            return NULL;
    }

    if (source_rt->IsType1() || source_rt->IsType2()) {
        return ReplicateType12AD(dynamic_cast <MVpnTable *>(src_table),
            dynamic_cast<MVpnRoute *>(source_rt), src_path, community);
    }

    MVpnManagerPartition *mvpn_manager_partition =
        GetPartitionManager(source_rt->index());

    if (source_rt->IsType7()) {
        return mvpn_manager_partition->ReplicateType7SourceTreeJoin(
            dynamic_cast <MVpnTable *>(src_table),
            dynamic_cast<MVpnRoute *>(source_rt), src_path, community);
    }

    if (source_rt->IsType3()) {
        return mvpn_manager_partition->ReplicateType3SPMSIPath(
            dynamic_cast <MVpnTable *>(src_table),
            dynamic_cast<MVpnRoute *>(source_rt), src_path, community);
    }

    if (source_rt->IsType4()) {
        return mvpn_manager_partition->ReplicateType4LeafAd(
            dynamic_cast <MVpnTable *>(src_table),
            dynamic_cast<MVpnRoute *>(source_rt), src_path, community);
    }
}

void MVpnManager::ResolvePath(BgpRoute *rt, BgpPath *path) {
    MVpnRoute *mvpn_route = static_cast<MVpnRoute *>(rt);
    BgpTable *table = rtinstance_->GetTable(Address::INET);
    table->path_resolver()->StartPathResolution(
        rt->get_table_partition()->index(), path, rt, table, mvpn_rt->source());
}
