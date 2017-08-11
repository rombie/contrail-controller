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

// This struct represents a MVPN Neighbor discovered using BGP.
//
// Each received Type1 Intra AS Auto-Discovery ad Type2 Inter AS Auto Discovery
// routes from MVPN BGP Peers is maintained as MVPN neighbors inside map in each
// MvpnManager object.
struct MvpnNeighbor {
public:
    MvpnNeighbor();
    MvpnNeighbor(const IpAddress &address, uint32_t asn, uint16_t vn_id,
                 bool external);
    std::string ToString() const;
    bool operator==(const MvpnNeighbor &rhs) const;

private:
    friend class MvpnManagerPartition;

    IpAddress address;
    uint32_t asn;
    uint16_t vn_id;
    bool external;
    std::string name;
};

// This class manages Mvpn routes with in a partition of an MvpnTable.
//
// It holds a back pointer to the parent MvpnManager class along with the
// partition id this object belongs to.
//
// Upon route change notification, based on the route-type for which a route
// notification has been received, different set of actions are undertaken in
// this class. This class only handles routes in which customer 'Group' info is
// encode, such as Type3 S-PMSI routes.
//
// Notification for a Type3 S-PMSI route add/change/delete
//     When sender indicates that it has a sender for a particular <S,G> entry,
//     (With Leaf-Information required in the PMSI tunnel attribute), then this
//     class originates/updates a new Type4 LeafAD route into vrf.mvpn.0 table
//
// Notification for a Type7 SourceTreeJoinRoute route add/change/delete
//     When this route is successfully imported into a vrf, new Type3 S-PMSI
//     route is originated/updated into that vrf.mvpn.0. (Typically, only if a
//     sender PMSI tunnel is configured for that <S,G>)
//
// This class also implements all the logic necessary when various MVPN routes
// are replicated from the primary table into secondary tables.
//
// Based on the types of the replicated paths, different set of actions are
// undertaken.
//
// Replication of Type7 SourceTreeJoin Route
//     If <Source> route is resolvable over a Mvpn neighbor and if the resolved
//     route has rt-import route target associated with it, only then this path
//     is replicated. In all other cases, if any replicated if already present
//     is actually deleted instead.
//
//     Route Target of the route is replaced entirely with just the value as
//     encoded inside the route-import extended community of the path which
//     resolves the Source address (of the <S,G>).
//
// Replication of Type4 LeafAD Route
//     This path is replicated only if PMSI tunnel information can be gathered
//     by successfully identifying the forest node of the local tree associated
//     with GlobalErmVpn route. If replicated, GlobalErmVpnRoute is notified so
//     that local route associated with the forest node can be correctly updated
//     with Input tunnel attribute. On the other hand, if the route is not
//     replicated, then the GlobalErmVpnRoute must be notified so that local
//     route associated with the forest node of the local tree is updated with
//     the deletion of any input tunnel attribute previously encoded.
//
//     Please refer to MvpnProjectManagerPartition documentation for further
//     details in this regard.
//
// Replication of Type3 S-PMSI route
//
// Replication of Type1/Type2 AD route
//
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
    bool IsMaster();
    bool IsMaster() const;
    int listener_id() const;

    bool ProcessType7SourceTreeJoinRoute(MvpnRoute *join_rt);
    void ProcessType3SPMSIRoute(MvpnRoute *spmsi_rt);
    void ProcessType4LeafADRoute(MvpnRoute *leaf_ad);

    MvpnState *GetState(MvpnRoute *route);
    const MvpnState *GetState(MvpnRoute *route) const;
    const MvpnState *GetState(ErmVpnRoute *route) const;
    MvpnState *GetState(ErmVpnRoute *route);
    MvpnState *LocateState(MvpnRoute *route);
    void DeleteState(MvpnState *state);

    BgpRoute *ReplicateType7SourceTreeJoin(BgpServer *server,
        MvpnTable *src_table, MvpnRoute *source_rt, const BgpPath *src_path,
        ExtCommunityPtr comm);
    BgpRoute *ReplicateType4LeafAD(BgpServer *server, MvpnTable *src_table,
        MvpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr comm);
    BgpRoute *ReplicatePath(BgpServer *server,
        const MvpnPrefix &rt_key, MvpnTable *src_table, MvpnRoute *src_rt,
        const BgpPath *src_path, ExtCommunityPtr community,
        BgpAttrPtr new_attr = NULL, bool *replicated = NULL);

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
// This class also holds a PathResolver object. This is used to resolve "Source"
// when Type7 <C-S,G> join routes are received over xmpp/igmp. Resolver, when
// ever it finds change to resolution status of a route, notifies the route (for
// which resolution was requested). Inside route notification, which runs off
// DB table tasks, resolver data structures can be examined at it is always
// ensured by scheduling policy that resolver and db tasks do not run together.
//
// This class also provides DeleteActor and maintains a LifetimeRef to parent
// MvpnTable object in order to ensure orderly cleanup during table deletion.
//
class MvpnManager {
public:
    typedef std::vector<MvpnManagerPartition *> PartitionList;
    typedef PartitionList::const_iterator const_iterator;
    typedef std::map<IpAddress, MvpnNeighbor> NeighborsMap;

    explicit MvpnManager(MvpnTable *table);
    virtual ~MvpnManager();
    bool findNeighbor(const IpAddress &address, MvpnNeighbor *nbr) const;
    MvpnProjectManager *GetProjectManager();
    const MvpnProjectManager *GetProjectManager() const;
    void ManagedDelete();
    BgpRoute *RouteReplicate(BgpServer *server, BgpTable *src_table,
        BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr comm);
    void ResolvePath(RoutingInstance *rtinstance, BgpRoute *rt, BgpPath *path);
    bool IsMaster() const;
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
    void Terminate();

private:
    friend class MvpnManagerPartition;
    class DeleteActor;

    void Initialize();
    void AllocPartitions();
    void FreePartitions();
    void UpdateNeighbor(MvpnRoute *route);
    void RouteListener(DBTablePartBase *tpart, DBEntryBase *db_entry);
    void NotifyAllRoutes();
    bool FindResolvedNeighbor(MvpnRoute *src_rt, const BgpPath *src_path,
            MvpnNeighbor *neighbor,
            ExtCommunity::ExtCommunityValue *rt_import = NULL) const;

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
//
// global_ermvpn_tree_rt_
//     This is a reference to GlobalErmVpnRoute associated with the ErmVpnTree
//     used in the data plane for this <S,G>. This route is created/updated
//     when ErmVpn notifies changes to ermvpn routes.
//
// leaf_ad_routes_originated_
//     This is a set of all type-4 leaf ad routes originated for this <S-G>.
//     It is possible that when leaf ad routes are originated, there is no
//     ermvpn tree available for forwarding in the data plane. In such a case,
//     later when global_ermvpn_tree_rt_ does get updated, all leaf ad routes
//     in this set are notified and re-evaluated for route replication.
//
// cjoin_routes_received_
//     This is a set of all Type7 cjoin routes originated (via xmpp) for this
//     <C-S,G>. These routes are replicated and send towards the source only
//     after the source can be resolved and MVPN neighbor is detected for the
//     resolved nexthop address. If MVPN neighbor is discovered or gets deleted,
//     all type-7 join routes must be re-evaluated so that the routes can now be
//     replicated (or perhaps deleted)
class MvpnState {
public:
    typedef std::set<MvpnRoute *> RoutesSet;
    struct SG {
        SG(const Ip4Address &source, const Ip4Address &group);
        SG(const IpAddress &source, const IpAddress &group);
        bool operator<(const SG &other) const;

        IpAddress source;
        IpAddress group;
    };

    explicit MvpnState(const SG &sg);
    virtual ~MvpnState();
    const SG &sg() const;
    ErmVpnRoute *global_ermvpn_tree_rt();
    const ErmVpnRoute *global_ermvpn_tree_rt() const;
    const RoutesSet &leaf_ad_routes() const;
    RoutesSet *leaf_ad_routes();
    void set_global_ermvpn_tree_rt(ErmVpnRoute *global_ermvpn_tree_rt);
    const RoutesSet &cjoin_routes_received() const;
    RoutesSet *cjoin_routes_received();

private:
    friend class MvpnDBState;
    friend class MvpnManagerPartition;
    friend class MvpnProjectManagerPartition;

    SG sg_;
    ErmVpnRoute *global_ermvpn_tree_rt_;
    RoutesSet leaf_ad_routes_originated_;
    RoutesSet cjoin_routes_received_;

#if 0  // In future phases.
    RoutesSet t4_leaf_ad_received_rt_;
    RoutesSet t5_source_active_rt_received_;
    RoutesSet t5_source_active_rt_originated_;
#endif

    int refcount_;

    DISALLOW_COPY_AND_ASSIGN(MvpnState);
};

// This class holds a reference to MvpnState along with associated with route
// and path pointers. This is stored as DBState inside the table along with the
// associated route.
//
// Note: Routes are never deleted until the DB state is deleted. MvpnState which
// is refcounted is also never deleted until there is no MvpnDBState that refers
// to it.
struct MvpnDBState : public DBState {
    MvpnDBState();
    MvpnDBState(MvpnState *state, MvpnRoute *route);
    explicit MvpnDBState(MvpnState *state);
    explicit MvpnDBState(MvpnRoute *route);

    MvpnState *state;
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
// Type4 leafAD MPVN routes are notified for re-evaluation.
//
// StateMap states_
//     A Map of <<S,G>, MvpnState> is maintained to hold MvpnState for all
//     <S,G>s that fall into a specific DB partition.
//
// This provides APIs to create/update/delete MvpnState as required. MvpnState
// is refcounted. When the refcount reaches 0, it is deleted from the StateMap
// and destroyed.
class MvpnProjectManagerPartition {
public:
    typedef std::map<MvpnState::SG, MvpnState *> StateMap;
    typedef MvpnState::SG SG;

    MvpnProjectManagerPartition(MvpnProjectManager*manager, int part_id);
    virtual ~MvpnProjectManagerPartition();
    MvpnState *GetState(const SG &sg);
    const MvpnState *GetState(const SG &sg) const;
    MvpnState *LocateState(const SG &sg);
    MvpnState *CreateState(const SG &sg);
    void DeleteState(MvpnState *mvpn_state);
    bool GetLeafAdTunnelInfo(ErmVpnRoute *global_ermvpn_tree_rt,
                             uint32_t *label, Ip4Address *address) const;

private:
    friend class MvpnProjectManager;
    friend class MvpnManagerPartition;

    void NotifyLeafAdRoutes(ErmVpnRoute *ermvpn_rt);

    // Back pointer to the parent MvpnProjectManager
    MvpnProjectManager *manager_;

    // Partition id of the manged DB partition.
    int part_id_;
    StateMap states_;;

    DISALLOW_COPY_AND_ASSIGN(MvpnProjectManagerPartition);
};

// This class glues mvpn and ermvpn modules
//
// It maintains a list of MvpnProjectManagerPartition objects, one for each
// DB partition.
//
// It listens to changes to ErmVpn table and for any applicable change to
// GlobalErmVpnRoute, it notifies all applicable LeafAdRoutes so that those
// routes can be replicated/deleted based on the current state of the
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
    void Terminate();

private:
    class DeleteActor;

    void Initialize();
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
