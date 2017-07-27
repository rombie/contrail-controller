/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_BGP_MMvPN_H_
#define SRC_BGP_BGP_MMvPN_H_

class MVpnManagerPartition;

class BgpPath;
class MVpnRoute;
class MVpnState;

struct MVpnNeighbor {
public:
    MVpnNeighbor(const IpAddress &address, uint32_t asn, uint32_t vn_id,
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

struct MVpnDBState : DBState {
    explicit MVpnDBState(MVpnState *state) : state(state) {
        if (state)
            state->refcount_++;
    }

    MVpnState *state;
    MVpnRoute *route;
    BgpPath *path;

    DISALLOW_COPY_AND_ASSIGN(MVpnDBState);
};

class MVpnState {
public:
    typedef std::set<MVpnRoute *> RoutesSet;
    struct SG {
        IpAddress source;
        IpAddress group;
    };

    MVpnState(const SG &sg) :
            sg_(sg), global_ermvpn_tree_rt_(NULL), refcount_(0) { }
    const SG &sg() const { return sg_; }
    const RoutesSet &cjoin_routes() const { return cjoin_routes_received_; }
    const RoutesSet &leaf_ad_routes() const {
        return leaf_ad_routes_originated_;
    }

private:
    friend class MVpnDBState;
    friend class MVpnManagerPartition;

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

    DISALLOW_COPY_AND_ASSIGN(MVpnState);
};

class MVpnManagerPartition {
public:
    typedef std::map<MVpnState::SG, MVpnState *> StateMap;

    MVpnManagerpartition(MVpnManagerpartition *manager, size_t part_id);
    virtual ~MVpnManagerPartition();

private:
    MVpnManager *manager_;
    size_t part_id_;
    StateMap states_;;

    DISALLOW_COPY_AND_ASSIGN(MVpnManagerPartition);
};

class MVpnManager {
public:
    typedef std::vector<MVpnManagerPartition *> PartitionList;
    typedef PartitionList::const_iterator const_iterator;
    typedef std::map<IpAddress, MVpnNeighbor> NeighborsMap;

    explicit MVpnManager(MVpnTable *table);
    virtual ~MVpnManager();

    bool findNeighbor(const IpAddress &address, MVpnNeighbor &nbr) const;

private:
    void Initialize();
    void AllocPartitions();
    void UpdateNeighbor(MVpnRoute *route);

    MVpnTable *table_;
    int listener_id_;
    PathResolver *resolver_;
    PartitionList partitions_;

    NeighborsMap neighbors_;
    mutable tbb:mutex neighbors_mutex_;

    boost::scoped_ptr<DeleteActor> deleter_;
    LifetimeRef<MVpnManager> table_delete_ref_;

    DISALLOW_COPY_AND_ASSIGN(MVpnManager);
};

#endif  // SRC_BGP_BGP_MvPN_H_
