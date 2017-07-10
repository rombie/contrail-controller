/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/bgp_mvpn.h"
#include "bgp/mvpn/mvpn_table.h"

MvpnDBState::~MvpnDBState() {
    assert(!global_ermvpn_tree_rt_);
    assert(t3_selective_pmsi_received_rt_.empty());
    assert(t4_leaf_ad_originated_rt_.empty());
    assert(t7_join_originated_routes_.empty());
}

MvpnManager::MvpnManager(MVpnTable *table)
        : table_(table),
          listener_id_(DBTable::kInvalidId),
          ermvpn_listener_id_(DBTable::kInvalidId),
          table_delete_ref_(this, table->deleter()) {
    deleter_.reset(new DeleteActor(this));
    Initialize();
}

MvpnManager::~MvpnManager() {
}

void MvpnManager::Initialize() {
    AllocPartitions();
    ermvpn_listener_id_ = ermvpn_table_->Register(
        boost::bind(&MvpnManager::ErmVpnRouteListener, this, _1, _2),
        "MvpnManager");

    if (table_->instance()->name() == "fabric") {
        mvpn_listener_id_ = table_->Register(
            boost::bind(&MvpnManager::ErmVpnRouteListener, this, _1, _2),
                        "MvpnManager");
    }

    // Originate t-1 Internal Auto-Discovery Route
    // Originate t-2 External Auto-Discovery Route
}

void MvpnManager::ErmVpnRouteListener(DBTablePartBase *tpart,
                                      DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");
    ErmVpnRoute *route = dynamic_cast<ErmVpnRoute *>(db_entry);

    // Notify all t-4 Leaf AD routes already originated for this S,G.
    if (IsGlobalTreeRootRoute(route)) {
        McastManagerPartition *partition = partitions_[tpart->index()];
        partition->NotifyLeafAdRoutes(route);
    }
}

void MvpnManager::RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry) {
    CHECK_CONCURRENCY("db::DBTable");

    McastManagerPartition *partition = partitions_[tpart->index()];
    MVpnRoute *route = dynamic_cast<MVpnRoute *>(db_entry);
    if (route->GetPrefix().type() == MVpnPrefix::Type3SPMSI) {
        partition->ProcessSPMSIRoute();
        return;
    }
}

void McastTreeManager::AllocPartitions() {
    for (int part_id = 0; part_id < table_->PartitionCount(); part_id++) {
        partitions_.push_back(new McastManagerPartition(this, part_id));
    }
}

// Constructor for MvpnManagerPartition.
MvpnManagerPartition::MvpnManagerPartition(MvpnManager *manager, size_t part_id)
    : manager_(manager),
      part_id_(part_id),
      work_queue_(TaskScheduler::GetInstance()->GetTaskId("db::DBTable"),
              part_id_,
              boost::bind(&MvpnManagerPartition::ProcessEntry, this, _1)) {
}

// Destructor for MvpnManagerPartition.
McastManagerPartition::~MvpnManagerPartition() {
    work_queue_.Shutdown();
}

void McastManagerPartition::ProcessSPMSIRoute(MVpnRoute *spmsi_rt) {
    if (IsMasterInstance())
        return;

    SG sg = SG(spmsi_rt->source(), spmsi_rt->group());
    RoutesStateMap::iterator iter = routes_state_.find(sg);
    MvpnDBState *mvpn_dbstate = NULL;
    if (iter != routes_state_.end()) {
        mvpn_dbstate = *iter;
        assert(mvpn_dbstate);
    }

    // Check if there is already a state associcated with this S,G.
    DBState *dbstate = spmsi_rt->GetState(table_, listener_id_);
    assert(!dbstate || dynamic_cast<MvpnDBState *>(dbstate) == mvpn_dbstate);

    MvpnRoute *leaf_ad_rt = NULL;
    if (!mvpn_dbstate) {
        // Ignore if there is no state associated with invalid route.
        if (!ermvpn_rt->Isvalid())
            return;

        // Create a new DB State and set it inside the route.
        mvpn_dbstate = new MvpnDBState();
        mvpn_dbstate->refcount_++;
        ermvpn_route->SetState(table_, listener_id_, mvpn_dbstate);

        // Insert this dbstate into mvpn route map as well.
        assert(routes_state_.insert(make_pair(sg, mvpn_dbstate)).second);
    } else {
        leaf_ad_rt = mvpn_dbstate->spmsi_to_leafad_rts_.find(spmsi_rt);
        if (!spmsi_rt->Isvalid()) {
            mvpn_dbstate->spmsi_to_leafad_rts_.erase(spmsi_rt);
        } else {
        }
    }

    if (spmsi_rt->Isvalid()) {
        // Originate new t4 path if one does not exists already.
        if (!leaf_ad_rt) {
            leaf_ad_rt = CreateLeafADRoute(spmsi_rt);
            mvpn_dbstate->spmsi_to_leafad_rts_.insert(make_pair(spmsi_rt,
                                                                leaf_ad_rt));
        }
    }

    if (leaf_ad_rt)
        leaf_ad_rt->Notify();
}

MvpnRoute *MvpnManager::ReplicateType4LeafAD(MvpnTable *src_table,
    MvpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    MvpnManagerPartition *mvpn_manager_partition =
        GetPartitionManager(source_rt->index());
    if (!mvpn_manager_partition)
        return NULL;
    return mvpn_manager_partition->ReplicateType4LeafAD(src_table,
            source_rt, src_path, community);
}

// Check if GlobalErmVpnTreeRoute is present. If so, only then can we replicate
// this path for advertisement to ingress routers with associated PMSI tunnel
// information.
void MvpnManagerPartition::ReplicateType4LeafAD(MvpnTable *src_table,
    MvpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {

    PMSITunnelInfo *pmsi =
        mvpn_partition_manager->GetLeafAdTunnelInfo(source_rt->group());
    if (!pmsi)
        return NULL;

    SG sg = SG(source_rt->source(), source_rt->group());
    RoutesStateMap::iterator iter = routes_state_.find(sg);
    assert(iter != routes_state_.end());
    MvpnDBState *mvpn_dbstate = *iter;

    if (!mvpndb_state->global_ermvpn_tree_rt_)
        return NULL;

    PMSI *pmsi = ermvpn_table_->GetGlobalErmVpnRoutePMSIInfo(
        mvpndb_state->global_ermvpn_tree_rt_);
    return CreateMvpnType4Route(source_rt, pmsi);
}

void McastManagerPartition::NotifyLeafADRoutes(ErmVpnRoute *ermvpn_rt) {
    SG sg = SG(ermvpn_rt->source(), ermvpn_rt->group());
    RoutesStateMap::iterator iter = routes_state_.find(sg);
    MvpnDBState *mvpn_dbstate = NULL;
    if (iter != routes_state_.end()) {
        mvpn_dbstate = *iter;
        assert(mvpn_dbstate);
    }

    // Check if there is already a state associcated with this S,G.
    DBState *dbstate = ermvpn_rt->GetState(ermvpntable_, ermvpn_listener_id_);
    assert(!dbstate || dynamic_cast<MvpnDBState *>(dbstate) == mvpn_dbstate);

    if (!mvpn_dbstate) {

        // Ignore if there is no state associated with invalid route.
        if (!ermvpn_rt->Isvalid())
            return;

        // Create a new DB State and set it inside the route.
        mvpn_dbstate = new MvpnDBState();
        mvpn_dbstate->global_ermvpn_tree_rt_ = ermvpn_rt;
        mvpn_dbstate->refcount_++;
        ermvpn_route->SetState(ermvpn_table_, ermvpn_listener_id_,
                               mvpn_dbstate);
        // Insert this dbstate into mvpn route map as well.
        assert(routes_state_.insert(make_pair(sg, mvpn_dbstate)).second);
        return;
    }

    if (!ermvpn_rt->Isvalid()) {
        mvpn_dbstate->global_ermvpn_tree_rt_ = NULL;

        // Remove this db-state from the route.
        ermvpn_route->ClearState(ermvpn_table_, ermvpn_listener_id_);
        mvpn_dbstate->refcount_--;
        if (!mvpn_dbstate->refcount_) {
            assert(routes_state_.erase(sg));
            delete mvpn_dbstate;
            return;
        }
    } else {
        mvpn_dbstate->global_ermvpn_tree_rt_ = ermvpn_rt;
    }

    // Notify all originated t-4 routes for PMSI re-computation.
    BOOST_FOREACH(RoutesMap::value_type &i,
        mvpn_dbstate->t3_received_spmsi_to_t4_originated_leaf_ad_rts_) {
        i.second->Notify();
    }
}

void McastManagerPartition::ProcessLeafADRoute(MVpnRoute *spmsi_route) {

    // Originate t-4 route. If one is already present, delete it and add
    // a new one corresponding to this new/updated t-3 route.

    // Retrieve PMSI Tunnel information from ErmVpn global tree route.
    PMSITunnelInfo *tunnel_info = NULL;

    // Find next db entry filling in type, source and group information.
    ErmVpnPrefix ermVpnPrefix(ErmVpnPrefix::GlobalTreeRoute,
            RouteDistinguisher::kZeroRd, spmsi_route->group(),
            smpi_route->source());
    ermvpn_table->get_partition(part_id_)->lower_bound(key);
    ErmVpnRoute *ermvpn_route =
        ermvpn_table_->FindGlobalTreeRoute(spmsi->source(), spmsi->group());
    if (ermvpn_route) {
        tunnel_info = ermvpn_route->GetPMSITunnelInfo();
    }
}

McastManagerPartition::ProcessLeafAdRoute(ErmVpnRoute *ermvpn_route) {
    // Find if there is any locally originated Primary Type-4 path. This can
    // be done by walking the table with lower_bound option. Table key is sorted
    // based on Type, Source, Group, etc.
    MvpnRoute *t4_spmsi_route = table_->FindRoute("Type4",
        ermvpn_route->source(), ermvpn_route->group());
    if (!route)
        return;
}
