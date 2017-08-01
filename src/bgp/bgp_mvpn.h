/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_BGP_MMvPN_H_
#define SRC_BGP_BGP_MMvPN_H_

class MvpnManagerPartition;

class BgpPath;
class MvpnRoute;
class MvpnState;

struct MvpnNeighbor {
public:
    MvpnNeighbor(const IpAddress &address, uint32_t asn, uint32_t vn_id,
                 bool external) :
            address(address), asn(asn), vn_id(vn_id), external(external) {
        std::ostringstream os;
        os << address << ":" << asn << ":" << vn_id << ":" << external;
        name = os.str();
    }

    std::string toString() const { return name; }

private:
    IpAddress address;
    uint32_t asn;
    uint32_t vn_id;
    bool external;
    std::string name;
};

struct MvpnDBState : DBState {
    explicit MvpnDBState(MvpnState *state) : state(state) {
        if (state)
            state->refcount_++;
    }

    MvpnState *state;
    MvpnRoute *route;
    BgpPath *path;

    DISALLOW_COPY_AND_ASSIGN(MvpnDBState);
};

class MvpnManagerPartition {
public:
    MvpnManagerpartition(MvpnManagerpartition *manager, size_t part_id);
    virtual ~MvpnManagerPartition();

private:
    MvpnManager *manager_;
    size_t part_id_;

    DISALLOW_COPY_AND_ASSIGN(MvpnManagerPartition);
};

class MvpnManager {
public:
    typedef std::vector<MvpnManagerPartition *> PartitionList;
    typedef PartitionList::const_iterator const_iterator;
    typedef std::map<IpAddress, MvpnNeighbor> NeighborsMap;

    explicit MvpnManager(MvpnTable *table);
    virtual ~MvpnManager();

    bool findNeighbor(const IpAddress &address, MvpnNeighbor &nbr) const;

private:
    void Initialize();
    void AllocPartitions();
    void UpdateNeighbor(MvpnRoute *route);

    MvpnTable *table_;
    int listener_id_;
    PathResolver *resolver_;
    PartitionList partitions_;

    NeighborsMap neighbors_;
    mutable tbb:mutex neighbors_mutex_;

    boost::scoped_ptr<DeleteActor> deleter_;
    LifetimeRef<MvpnManager> table_delete_ref_;

    DISALLOW_COPY_AND_ASSIGN(MvpnManager);
};

class MvpnState {
public:
    typedef std::set<MvpnRoute *> RoutesSet;
    struct SG {
        IpAddress source;
        IpAddress group;
    };

    MvpnState(const SG &sg) :
            sg_(sg), global_ermvpn_tree_rt_(NULL), refcount_(0) { }
    const SG &sg() const { return sg_; }
    const RoutesSet &cjoin_routes() const { return cjoin_routes_received_; }
    const ErmVpnRoute *global_ermvpn_tree_rt() const {
        return global_ermvpn_tree_rt_;
    }
    const RoutesSet &leaf_ad_routes() const {
        return leaf_ad_routes_originated_;
    }
    void set_global_ermvpn_tree_rt(ErmVpnRoute *global_ermvpn_tree_rt) {
        global_ermvpn_tree_rt = global_ermvpn_tree_rt_;
    }

private:
    friend class MvpnDBState;
    friend class MvpnProjectManagerPartition;

    SG sg_;
    ErmVpnRoute *global_ermvpn_tree_rt_;
    RoutesSet leaf_ad_routes_originated_;
    RoutesSet cjoin_routes_received_;

#if 0 // For future phases.
    RoutesSet t4_leaf_ad_received_rt_;
    RoutesSet t5_source_active_rt_received_;
    RoutesSet t5_source_active_rt_originated_;
#endif
    
    int refcount_;

    DISALLOW_COPY_AND_ASSIGN(MvpnState);
};

class MvpnProjectManagerPartition {
public:
    typedef std::map<MvpnState::SG, MvpnState *> StateMap;

    MvpnProjectManagerpartition(MvpnProjectManagerpartition *manager,
                              size_t part_id);
    virtual ~MvpnProjectManagerPartition();

private:
    MvpnProjectManager *manager_;
    size_t part_id_;
    StateMap states_;;

    DISALLOW_COPY_AND_ASSIGN(MvpnProjectManagerPartition);
};

class MvpnProjectManager {
public:
    typedef std::vector<MvpnProjectManagerPartition *> PartitionList;
    typedef PartitionList::const_iterator const_iterator;

    explicit MvpnProjectManager(MvpnTable *table);
    virtual ~MvpnProjectManager();

private:
    void Initialize();
    void AllocPartitions();

    MvpnTable *table_;
    int listener_id_;
    PartitionList partitions_;

    boost::scoped_ptr<DeleteActor> deleter_;
    LifetimeRef<MvpnProjectManager> table_delete_ref_;

    DISALLOW_COPY_AND_ASSIGN(MvpnProjectManager);
};

#endif  // SRC_BGP_BGP_MvPN_H_
