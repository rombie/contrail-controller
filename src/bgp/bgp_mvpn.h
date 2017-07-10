/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_BGP_MMVPN_H_
#define SRC_BGP_BGP_MMVPN_H_

class MvpnManagerPartition;

class MvpnDBState : DBState {
public:
    MvpnDBState() : refcount_(0) { }
    virtual ~MvpnDBState() { }

    typedef std::map<MvpnRoute *, MvpnRoute *> RoutesMap;
    struct SG {
        IpAddress source_;
        IpAddress group_;
    };

private:
    friend class MvpnManagerPartition;

    // For Phase 1.
    ErmVpnRoute *global_ermvpn_tree_rt_;
    // t3_received_spmsi_to_t4_originated_leaf_ad_rts_;
    RoutesMap spmsi_to_leafad_rts_;
    std::set<MvpnRoute *> t7_join_originated_routes_;

    // For future phases.
    /*
    std::map<MvpnRoute *, MvpnRoute *>
        t7_received_cjoin_to_t3_originated_spmsi_rts_;
    std::set<MvpnRoute *> t4_leaf_ad_received_rt_;
    std::set<MvpnRoute *> t5_source_active_rt_received_;
    std::set<MvpnRoute *> t5_source_active_rt_originated_;
    */
    
    int refcount_;

    DISALLOW_COPY_AND_ASSIGN(MvpnDBState);
};

class MvpnManagerPartition {
public:
    typedef std::map<MvpnDBState::SG, MvpnDBState *> RoutesStateMap;

    explicit MvpnManagerpartition(MVpnManagerpartition *manager);
    virtual ~MvpnManagerPartition();

private:
    MvpnManager *manager_;
    size_t part_id_;
    RoutesStateMap routes_state_;

    DISALLOW_COPY_AND_ASSIGN(MvpnManagerPartition);
};

class MvpnManager {
public:
    typedef std::vector<MvpnManagerPartition *> PartitionList;
    typedef PartitionList::const_iterator const_iterator;

    explicit MvpnManager(MvpnTable *table);
    virtual ~MvpnManager();

private:
    void Initialize();
    void AllocPartitions();

    MvpnTable *table_;
    int listener_id_;

    // These are used only in __<project>__.mvpn.0 table's manager.
    int ermvpn_listener_id_;
    PartitionList partitions_;

    boost::scoped_ptr<DeleteActor> deleter_;
    LifetimeRef<MvpnManager> table_delete_ref_;

    DISALLOW_COPY_AND_ASSIGN(MvpnManager);
};

#endif  // SRC_BGP_BGP_MVPN_H_
