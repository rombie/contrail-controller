/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_BGP_MVPN_H_
#define SRC_BGP_BGP_MVPN_H_

#include <tbb/reader_writer_lock.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <boost/scoped_ptr.hpp>

#include "base/lifetime.h"
#include "bgp/bgp_attr.h"
#include "db/db_entry.h"
#include "net/address.h"

class BgpPath;
class BgpRoute;
class BgpServer;
class BgpTable;
class ErmVpnRoute;
class ErmVpnTable;
class MvpnManager;
class MvpnManagerPartition;
class MvpnPrefix;
class MvpnProjectManager;
class MvpnProjectManagerPartition;
class MvpnRoute;
class MvpnState;
class MvpnTable;
class PathResolver;
class RoutingInstance;
class UpdateInfo;

typedef boost::intrusive_ptr<MvpnState> MvpnStatePtr;

// This struct represents a MVPN Neighbor discovered using BGP.
//
// Each received Type1 Intra AS Auto-Discovery ad Type2 Inter AS Auto Discovery
// routes from MVPN BGP Peers is maintained as MVPN neighbors inside map in each
// MvpnManager object.
struct MvpnNeighbor {
public:
    MvpnNeighbor();
    MvpnNeighbor(const IpAddress &address, uint32_t asn, uint16_t vrf_id,
                 bool external);
    std::string ToString() const;
    const IpAddress &address() const;
    uint16_t vrf_id() const;
    uint32_t asn() const;
    bool operator==(const MvpnNeighbor &rhs) const;

private:
    friend class MvpnManagerPartition;

    IpAddress address_;
    uint32_t asn_;
    uint16_t vrf_id_;
    bool external_;
    std::string name_;
};

// This class manages Mvpn routes with in a partition of an MvpnTable.
//
// It holds a back pointer to the parent MvpnManager class along with the
// partition id this object belongs to.
//
// Upon route change notification, based on the route-type for which a route
// notification has been received, different set of actions are undertaken in
// this class. This class only handles routes in which customer 'Group' info is
// encoded, such as Type3 S-PMSI routes.
//
// Notification for a Type3 S-PMSI route add/change/delete
//     When sender indicates that it has a sender for a particular <S,G> entry,
//     (With Leaf-Information required in the PMSI tunnel attribute), then this
//     class originates/updates a new Type4 LeafAD route into vrf.mvpn.0 table.
//     LeafAD route however is originated only if a usuable GlobalErmVpnRoute
//     is avaiable for stitching. On the other hand, if such a route was already
//     originated before and now no longer feasible, it is deleted instead.
//
// Notification for a Type7 SourceTreeJoinRoute route add/change/delete
//     When this route is successfully imported into a vrf, new Type3 S-PMSI
//     route is originated/updated into that vrf.mvpn.0 if there is at least
//     one associated Join route from the agent with flags marked as "Sender"
//     or "SenderAndReciver".
class MvpnManagerPartition {
public:
    MvpnManagerPartition(MvpnManager *manager, int part_id);
    virtual ~MvpnManagerPartition();
    MvpnProjectManagerPartition *GetProjectManagerPartition();
    const MvpnProjectManagerPartition *GetProjectManagerPartition() const;

private:
    friend class MvpnManager;

    MvpnTable *table();
    const MvpnTable *table() const;
    int listener_id() const;

    bool ProcessType7SourceTreeJoinRoute(MvpnRoute *join_rt);
    void ProcessType3SPMSIRoute(MvpnRoute *spmsi_rt);
    void ProcessType4LeafADRoute(MvpnRoute *leaf_ad);

    MvpnStatePtr GetState(MvpnRoute *route);
    MvpnStatePtr GetState(MvpnRoute *route) const;
    MvpnStatePtr GetState(ErmVpnRoute *route) const;
    MvpnStatePtr GetState(ErmVpnRoute *route);
    MvpnStatePtr LocateState(MvpnRoute *route);
    void DeleteState(MvpnStatePtr state);
    void NotifyForestNode(const Ip4Address &source, const Ip4Address &group);
    bool GetForestNodePMSI(ErmVpnRoute *rt, uint32_t *label,
                           Ip4Address *address,
                           std::vector<std::string> *encap) const;

    MvpnManager *manager_;
    int part_id_;

    DISALLOW_COPY_AND_ASSIGN(MvpnManagerPartition);
};

// This class manages MVPN routes for a given vrf.mvpn.0 table.
//
// In each *.mvpn.0 table, an instance of this class is created when ever the
// mvpn table itself is created (which happens when its parent routing-instance
// gets created.
//
// This class allocated one instance of MvpnManagerPartition for each DB
// partition. While all <S,G> specific operations are essentially manages inside
// MvpnManagerPartition object (in order to get necessary protection from
// concurrency across different db partitions), all other operations which are
// <S,G> agnostic are mainly handled in this class.
//
// Specifically, all MVPN BGP neighbors are maintained in std::map NeighborsMap.
// Neighbors are created or updated when Type1/Type2 paths are received and are
// deleted when those routes are deleted. All access to this map is protected
// by a mutex because even though the map itself may be created, updated, or
// deleted always serially from with in the same db task, map will get accessed
// (read) concurrently from task running of different DB partitions.
//
// This class also provides DeleteActor and maintains a LifetimeRef to parent
// MvpnTable object in order to ensure orderly cleanup during table deletion.
class MvpnManager {
public:
    typedef std::vector<MvpnManagerPartition *> PartitionList;
    typedef PartitionList::const_iterator const_iterator;
    typedef std::map<IpAddress, MvpnNeighbor> NeighborsMap;

    explicit MvpnManager(MvpnTable *table);
    virtual ~MvpnManager();
    bool FindNeighbor(const IpAddress &address, MvpnNeighbor *nbr) const;
    MvpnProjectManager *GetProjectManager();
    const MvpnProjectManager *GetProjectManager() const;
    void ManagedDelete();
    BgpRoute *RouteReplicate(BgpServer *server, BgpTable *src_table,
        BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr comm);
    void ResolvePath(RoutingInstance *rtinstance, BgpRoute *rt, BgpPath *path);
    MvpnManagerPartition *GetPartition(int part_id);
    const MvpnManagerPartition *GetPartition(int part_id) const;
    MvpnTable *table();
    const MvpnTable *table() const;
    int listener_id() const;
    PathResolver *path_resolver();
    PathResolver *path_resolver() const;
    LifetimeActor *deleter();
    const LifetimeActor *deleter() const;
    bool deleted() const;
    virtual void Terminate();
    RouteDistinguisher GetSourceRouteDistinguisher(const BgpPath *path) const;
    virtual void Initialize();

private:
    friend class MvpnManagerPartition;
    class DeleteActor;

    void AllocPartitions();
    void FreePartitions();
    void UpdateNeighbor(MvpnRoute *route);
    void RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry);
    void NotifyAllRoutes();
    bool FindResolvedNeighbor(const BgpPath *path,
            MvpnNeighbor *neighbor) const;

    MvpnTable *table_;
    int listener_id_;
    PartitionList partitions_;

    NeighborsMap neighbors_;
    mutable tbb::reader_writer_lock neighbors_mutex_;

    boost::scoped_ptr<DeleteActor> deleter_;
    LifetimeRef<MvpnManager> table_delete_ref_;

    DISALLOW_COPY_AND_ASSIGN(MvpnManager);
};

// This class holds Mvpn state for a particular <S,G> at any given time.
//
// In MVPN state machinery, different types of routes are sent and received at
// different phases of processing. This class holds all relevant information
// associated with an <S,G>.
//
// This is a refcounted class which is referred by DB States of different
// routes. When the refcount reaches 0, (last referring db state is deleted),
// this object is destroyed.
// TODO(Ananth) Use MvpnStatePtr intrusive pointer to manage this.
//
// global_ermvpn_tree_rt_
//     This is a reference to GlobalErmVpnRoute associated with the ErmVpnTree
//     used in the data plane for this <S,G>. This route is created/updated
//     when ErmVpn notifies changes to ermvpn routes.
//
// spmsi_rt_
//     This is the 'only' Type3 SPMSI sender route originated for this S,G.
//     When an agent indicates that it has an active sender for a particular
//     <S,G> via Join route, then this route is originated (if there is atleast
//     one active reeiver)
//
// spmsi_routes_received_
//     This is a set of all Type3 spmsi routes received for this <S-G>. It is
//     possible that when these routes are received, then there is no ermvpn
//     tree route to use for forwarding in the data plane. In such a case, later
//     when global_ermvpn_tree_rt_ does get updated, all leaf ad routes in this
//     set are notified and re-evaluated.
//
// cjoin_routes_received_
//     This is a set of all Type7 cjoin routes originated (via xmpp) for this
//     <C-S,G>. These routes are replicated and send towards the source only
//     after the source can be resolved and MVPN neighbor is detected for the
//     resolved nexthop address. If MVPN neighbor is discovered or gets deleted,
//     all type-7 join routes must be re-evaluated so that the routes can now be
//     replicated (or perhaps deleted). Path resolver provides necessary hooks
//     for this source path resolution.
class MvpnState {
public:
    typedef std::set<MvpnRoute *> RoutesSet;
    typedef std::map<MvpnRoute *, BgpAttrPtr> RoutesMap;

    struct SG {
        SG(const Ip4Address &source, const Ip4Address &group);
        SG(const IpAddress &source, const IpAddress &group);
        SG(const ErmVpnRoute *route);
        SG(const MvpnRoute *route);
        bool operator<(const SG &other) const;

        IpAddress source;
        IpAddress group;
    };

    typedef std::map<SG, MvpnStatePtr> StatesMap;
    MvpnState(const SG &sg, StatesMap *states = NULL);
    virtual ~MvpnState();
    const SG &sg() const;
    ErmVpnRoute *global_ermvpn_tree_rt();
    const ErmVpnRoute *global_ermvpn_tree_rt() const;
    MvpnRoute *spmsi_rt();
    const MvpnRoute *spmsi_rt() const;
    void set_global_ermvpn_tree_rt(ErmVpnRoute *global_ermvpn_tree_rt);
    void set_spmsi_rt(MvpnRoute *spmsi_rt);
    const RoutesSet &spmsi_routes_received() const;
    RoutesSet &spmsi_routes_received();
    const RoutesSet &cjoin_routes_received() const;
    RoutesSet &cjoin_routes_received();
    const RoutesMap &leafad_routes_received() const;
    RoutesMap &leafad_routes_received();
    const StatesMap *states() const { return states_; }
    StatesMap *states() { return states_; }

private:
    friend class MvpnDBState;
    friend class MvpnManagerPartition;
    friend class MvpnProjectManagerPartition;
    friend void intrusive_ptr_add_ref(MvpnState *mvpn_state);
    friend void intrusive_ptr_release(MvpnState *mvpn_state);

    SG sg_;
    ErmVpnRoute *global_ermvpn_tree_rt_;
    MvpnRoute *spmsi_rt_;
    RoutesSet spmsi_routes_received_;
    RoutesSet cjoin_routes_received_;
    RoutesMap leafad_routes_received_;
    StatesMap *states_;

#if 0  // In future phases.
    RoutesSet t4_leaf_ad_received_rt_;
    RoutesSet t5_source_active_rt_received_;
    RoutesSet t5_source_active_rt_originated_;
#endif

    tbb::atomic<int> refcount_;

    DISALLOW_COPY_AND_ASSIGN(MvpnState);
};

inline void intrusive_ptr_add_ref(MvpnState *mvpn_state) {
    mvpn_state->refcount_.fetch_and_increment();
}

inline void intrusive_ptr_release(MvpnState *mvpn_state) {
    int prev = mvpn_state->refcount_.fetch_and_decrement();
    if (prev > 1)
        return;
    if (mvpn_state->states()) {
        MvpnState::StatesMap::iterator iter =
            mvpn_state->states()->find(mvpn_state->sg());
        if (iter != mvpn_state->states()->end()) {
            assert(iter->second == mvpn_state);
            mvpn_state->states()->erase(mvpn_state->sg());
        }
    }
    delete mvpn_state;
}

// This class holds a reference to MvpnState along with associated with route
// and path pointers. This is stored as DBState inside the table along with the
// associated route.
//
// Note: Routes are never deleted until the DB state is deleted. MvpnState which
// is refcounted is also never deleted until there is no MvpnDBState that refers
// to it.
struct MvpnDBState : public DBState {
    MvpnDBState();
    MvpnDBState(MvpnStatePtr state, MvpnRoute *route);
    ~MvpnDBState();
    explicit MvpnDBState(MvpnStatePtr state);
    explicit MvpnDBState(MvpnRoute *route);

    MvpnStatePtr state;
    MvpnRoute *route;

    DISALLOW_COPY_AND_ASSIGN(MvpnDBState);
};

// This class glues mvpn and ermvpn modules, inside a particular DB partition.
//
// Each MVPN is associated with a parent MvpnProjectManager virtual-network via
// configuration. This parent MvpnProjectManager's ermvpn tree is the one used
// for all multicast packets replication in the data plane for the given MVPN.
//
// Inside each RoutingInstance object, name of this parent manager virtual
// network is stored in mvpn_project_manager_network_ string. When ever this
// information is set/modified/cleared in the routing instance, all associated
// Type3 S-PMSI MPVN received routes should be notified for re-evaluation.
//
// MvpnState::StatesMap states_
//     A Map of <<S,G>, MvpnState> is maintained to hold MvpnState for all
//     <S,G>s that fall into a specific DB partition.
//
// This provides APIs to create/update/delete MvpnState as required. MvpnState
// is refcounted. When the refcount reaches 0, it is deleted from the
// MvpnState::StatesMap and destroyed.
class MvpnProjectManagerPartition {
public:
    typedef MvpnState::SG SG;

    MvpnProjectManagerPartition(MvpnProjectManager*manager, int part_id);
    virtual ~MvpnProjectManagerPartition();
    MvpnStatePtr GetState(const SG &sg);
    MvpnStatePtr GetState(const SG &sg) const;
    MvpnStatePtr LocateState(const SG &sg);
    MvpnStatePtr CreateState(const SG &sg);
    void DeleteState(MvpnStatePtr mvpn_state);

private:
    friend class MvpnProjectManager;
    friend class MvpnManagerPartition;

    ErmVpnRoute *GetGlobalTreeRootRoute(ErmVpnRoute *rt) const;
    ErmVpnTable *table();
    const ErmVpnTable *table() const;
    bool IsUsableGlobalTreeRootRoute(ErmVpnRoute *ermvpn_route) const;
    void RouteListener(DBEntryBase *db_entry);
    int listener_id() const;
    void NotifyForestNode(const Ip4Address &source, const Ip4Address &group);
    bool GetForestNodePMSI(ErmVpnRoute *rt, uint32_t *label,
                           Ip4Address *address,
                           std::vector<std::string> *encap) const;

    // Back pointer to the parent MvpnProjectManager
    MvpnProjectManager *manager_;

    // Partition id of the manged DB partition.
    int part_id_;
    MvpnState::StatesMap states_;;

    DISALLOW_COPY_AND_ASSIGN(MvpnProjectManagerPartition);
};

// This class glues mvpn and ermvpn modules
//
// It maintains a list of MvpnProjectManagerPartition objects, one for each DB
// partition.
//
// It listens to changes to ErmVpn table and for any applicable change to
// GlobalErmVpnRoute, it notifies all applicable received SPMSI routes so that
// those routes can be replicated/deleted based on the current state of the
// GlobalErmVpnRoute associated with a given <S,G>.
//
// This class also provides DeleteActor and maintains a LifetimeRef to parent
// MvpnTable object in order to ensure orderly cleanup during table deletion.
class MvpnProjectManager {
public:
    typedef std::vector<MvpnProjectManagerPartition *> PartitionList;
    typedef PartitionList::const_iterator const_iterator;

    explicit MvpnProjectManager(ErmVpnTable *table);
    virtual ~MvpnProjectManager();
    MvpnProjectManagerPartition *GetPartition(int part_id);
    const MvpnProjectManagerPartition *GetPartition(int part_id) const;
    void ManagedDelete();
    virtual void Terminate();
    ErmVpnTable *table();
    const ErmVpnTable *table() const;
    int listener_id() const;
    virtual void Initialize();
    MvpnStatePtr GetState(MvpnRoute *route) const;
    MvpnStatePtr GetState(MvpnRoute *route);
    UpdateInfo *GetUpdateInfo(MvpnRoute *route);

private:
    class DeleteActor;

    void AllocPartitions();
    void FreePartitions();
    void RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry);

    ErmVpnTable *table_;
    int listener_id_;
    PartitionList partitions_;

    boost::scoped_ptr<DeleteActor> deleter_;
    LifetimeRef<MvpnProjectManager> table_delete_ref_;

    DISALLOW_COPY_AND_ASSIGN(MvpnProjectManager);
};

#endif  // SRC_BGP_BGP_MVPN_H_
