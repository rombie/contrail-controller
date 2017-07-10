# 1. Introduction
Provide BGP NGEN MVpn support to contrail software

# 2. Problem statement
Currently, multicast is supported using ErmVpn (Edge replicated multicast).
This solution is limited to a single virtual-network. i.e., senders and
receivers cannot span across different virtual-networks. Also, this solution
is not inter-operable (yet) with any of the known bgp implementations.

# 3. Proposed solution
Use NGEN-MVpn design to solve intra-vn and inter-vn multicast capabilities

## 3.1 Alternatives considered
None

## 3.2 API schema changes
Do we need to support for a new set of import and export route-targets for
mvpn which overrides those configured for unicast (?) e.g.
set routing-instances v protocols mvpn route-target import-target|export-target

```
diff --git a/src/schema/bgp_schema.xsd b/src/schema/bgp_schema.xsd
--- a/src/schema/bgp_schema.xsd
+++ b/src/schema/bgp_schema.xsd
@@ -289,6 +289,8 @@
         <xsd:enumeration value="inet-vpn"/>
         <xsd:enumeration value="e-vpn"/>
         <xsd:enumeration value="erm-vpn"/>
+        <xsd:enumeration value="inet-mvpn"/>
         <xsd:enumeration value="route-target"/>
         <xsd:enumeration value="inet6"/>
         <xsd:enumeration value="inet6-vpn"/>
diff --git a/src/schema/vnc_cfg.xsd b/src/schema/vnc_cfg.xsd
index 2804d8b..732282b 100644
--- a/src/schema/vnc_cfg.xsd
+++ b/src/schema/vnc_cfg.xsd
@@ -1363,6 +1363,8 @@ targetNamespace="http://www.contrailsystems.com/2012/VNC-CONFIG/0">
          <!-- Enable or disable Mirroring for virtual-network -->
          <xsd:element name='mirror-destination' type="xsd:boolean" default="false" required='optional'
              description='Flag to mark the virtual network as mirror destination network'/>
+         <!-- Enable or disable ipv4-multicast for virtual-network -->
+         <xsd:element name='ipv4-multicast' type="xsd:boolean" default="false" required='optional' description='Flag to enable ipv4 multicast service'/>
     </xsd:all>
 </xsd:complexType>

```

## 3.3 User workflow impact
####Describe how users will use the feature.

## 3.4 UI changes
UI shall provide a way to configure MVpn for bgp and virtual-networks.

## 3.5 Notification impact
####Describe any log, UVE, alarm changes

# 4. Implementation

## 4.1 Capability negotiation
When mvpn AFI is configured, BGP shall exchange Capability with MCAST_VPN NLRI
for IPv4 multicast vn routes. AFI(1)/SAFI(5). This is not enabled by default.

## 4.2 MVpnManager

```
class MVpnManager {
public:
    struct RouteState {
    };

    // Locally originated Type-4 Leaf AD Route state
    struct LeafAdRouteStateType4 : State {
        uint32_t label;
        TunnelEncapType tunnel_encap_type;
        ErmVpnPrefix global_tree_route_prefix; // To update Input Tunnel Attr
    };

    // IGMP Join routes state sent by agents over XMPP
    struct CustomerRouteStateType7 : State {
        RouteDistinguisher tree_root_rd;
        RouteTarget import_route_target;
    };

private:
    typedef std::map<MvpnPrefix, State> RouteState; // key as MvpnRoute * ?
    RouteState route_state_;
};
```

1. There shall be one instance of MVpnManager per vrf.mvpn.0 table
2. Maintains list of auto-discovered mvpn [bgp] neighbors
    Each neighbor info contains
        router-id
        rt-import target (retrieved from unicast route towards source)
3. Manages locally originated mvpn routes (source: local) such as
   type-1 (AD), type-7 C-<S, G> and type-3(Leaf AD with PMSI attr)
4. Map of C-<S,G> => ErmVpnGlobalTreeRoute
5. Map of C-<S,G> => S-PMSI (Local receivers)
    When there is no receiver on the local site, then control-node should not
    join to the tunnel. After first receiver comes in (ErmVpnGlobalTreeRoute is
    generated ?), Leaf AD route can be sent to ingress PE showing interest in
    receiving the multicast traffic from the associated source
6. Map of <S, C-<S,G> >
    This used to manage requests with unicast nexthop resolver. When the map
    becomes empty, resolution request can be deleted. Whenever the map size
    reaches 1, new request to resolver is initiated in order to get notified
    when the Source becomes (or is) reachable

## 4.3 Events

Type-1 AD Send
Type-1 AD Receive
Type-2 AD Send
Type-2 AD Receive

Xmpp Type-7 Create C-<S, G>
Mvpn Type-7 Create in bgp.mvpn.0 (via replicate())
Mvpn Type-3 Receive in foo.mvpn.0 (via replicate())

Mvpn Type-4 Create (Locally originated by MVpn Manager)

Source resolvable (Resolver notification)
Source not resolvable (Resolver notification)

ErmVpn GlobalTreeRoute Create/Update (ErmVpnTable Listener)
ErmVpn GlobalTreeRoute Delete (ErmVpnTable Listener)

MVpn work flow us essentially handled complete based on route change
notification. (MVpn Manager is listener to multiple tables)

## 4.3 Auto Discovery

MVpnManager generates Type 1 A-D Route in each of the vrf.mvpn.0 (When ever
mvpn is configured/enabled in the VN) (Note: There is no PMSI info encoded)
Originator control-node IP address, router-id ans asn are used where ever
originator info is encoded.


```
1:RD:OriginatorIpAddr  (RD in asn:vn-id or router-id:vn-id format)
  1:self-control-node-router-id:vn-id:originator-control-node-ip-address OR
  1:asn:vn-id:originator-control-node-ip-address
```
  export route target is export route target of the of the routing-instance.
  These routes would get imported to all mvpn tables whose import route-targets
  list contains this exported route-target (Similar to how vpn-unicast routes
  get imported) (aka JUNOS auto-export)

```
1:RD:SourceAs  (RD in asn:vn-id or router-id:vn-id format)
  1:self-control-node-router-id:vn-id:source-as OR
  1:asn:vn-id:source-as
```

These routes should get replicated to bgp.mvpn.0 and then shall be advertised to
all BGP neighbors with whom mvpn AFI is exchanged as part of the initial
capability negotiation. This is bgp based mvpn-site auto discovery.

Note: Intra-AS route is only advertised to IBGP neighbors and Inter-AS route
is advertised to only e-bgp neighbors. Since, those neighbors are already part
of distinct group on the outbound side, simple hard-coded filtering can be
applied to get this functionality.

MPVN Manager originates Type-1 and Type-2 Inter-AS A-D route (with no PMSI
tunnel information) in each vrf.mvpn.0 table.

## 4.4 C-<S,G> Routes learning via Agents/IGMP

Agent sends over XMPP, IGMP joins over VMI as C-<S,G> routes and keeps track of
map of all S-G routes => List of VMIs (and VRFs) (for mapping to tree-id). These
C-<S,G> routes are added to vrf.mvpn.0 table with source protocol XMPP as Type7
route in vrf.mvpn.0. This shall have zero-rd as the source-root-rd and 0 as the
root-as asn (since these values are unknown/NA for this particular route)


Format of Type 7 <C-S, G> route added to vrf.mvpn.0 with protocol local/MVpn
```
  7:<zero-router-id>:<vn-id>:<zero-as>:<C, G>
```

/32 Source address is registered for resolution via resolver.

When ever this address is resolvable (or otherwise), notification is expected
to be called back into MVpnManager, under the resolved (or not) unicast route
db task context.

o If the route is resolvable (over BGP), next-hop address and rt-import
  extended rtarget community associated with the route is retrieved and stored
  inside MVpnManager DB State state associated with the Type-7 C-<S,G> route.
  Unicast Resolver can provide the handle directly which can be this DB state
  to be updated (?). mvpn prefix is now notified, as further processing is
  required

o If the route is not resolvable any more, then the stored address and
  rt-import community shall be cleared from the MVpn Type-7 route db-state and
  the mvpn route is notified

  Q: (How to find if and when the route-import target value changes)

## 4.5 Type-7 Route replication into bgp.mvpn.0

When Type-7 <C-<S,G> route gets notified, its replicate() method is called. In
here, if the next-hop (root rd) and rt-import community values are available,
new type-7 secondary route is created inside bgp.mvpn.0 table (or existing one
may need to be deleted and added again, if root node changes from one ingress
PE to another). 

Format of Type 7 C-<S, G> route replicated to bgp.mvpn.0 with protocol MVpn
```
  7:<source-root-rd>:<root-as>:<C, G>
  7:source-root-router-id:vn-id:<root-as>:<C, G>
  export route-target should be rt-import route-target of route towards source
  (as advertised by ingress PE)
```

This should should get replicated to bgp.mvpn.0, and then shall be advertised
to all other mvpn neighbors. (Route Target Filtering will ensure that it is
only sent to the ingress PE)

Any change to readability to Source shall be dealt as delete of old type-7
route and add of new type-7 route

Note: This requires advertising IGMP Routes as XMPP routes into different table
vrf.mvpn.0 (instead of vrf.ermvpn.0). Hence requires changes to agent code.

## 4.5 C-<*, G> Routes learning via Agents/IGMP

For C-<*, G> routes, Source address is retrieved from Type-5 Source Active (SA)
routes. This is an additional asynchronous dependency that creeps in when we
add support for ASM mode. Code must handle both cases where in SA route is
available when route is received from XMPP, and when it becomes available later
on. MVpn Manager can maintain a map of <G, S> built from received Source-Active
routes. This is not targeted for Phase 1. Also, origination of Source Active
routes (after learning about a specific source with in the vpn site) is also not
targeted for Phase 1. When it is supported, MVpn Manager shall originate SA
route as appropriate. This route will also get replicated to bgp.mvpn.0 and gets
imported into local vrfs as well as remote vrfs based on export and import route
targets.

## 4.5 Nexthop resolver for source
As mentioned in previous section, when C-<S, G> route is received and installed
in vrf.mvpn.0 table (protocol: XMPP), MVpnManager would get notified. One of the
actions to take for this particular event is to monitor for route resolution
towards the source. Similar to path-resolver, a nexthop resolver needs to be
implemented to achieve this functionality. This would be a subclass of class
ConditionMatch and monitor for any viable path to reach the source.

Phase 1: Senders are _always_ outside the cluster and receivers are always
inside the cluster.  In this phase, source is expected to resolve always over a
BGP path, with nexthop pointing to one of the SDN gateways.

Note: If sender support within data-center is supported, then code should handle
the case when next-hop is resolvable directly through this vrf or a different
vrf (via XMPP). If it is through bgp, it is also possible that this is still an
xmpp path (in peer control-node). We do not need to join to the peer towards the
source if the source is within the data center (peer is control-node ?, as the
ermvpn tree built is already a self-contained bi-directional tree which already
includes all intended multicast receivers for any <C-S, G>.

```
diff --git a/src/base/set_util.h b/src/base/set_util.h
index 687b734..5cd37ae 100644
--- a/src/base/set_util.h
+++ b/src/base/set_util.h
@@ -15,17 +15,22 @@
 // The add/delete functors are responsible for adding/deleting appropriate
 // elements to/from the 1st set.
 //
+// Returns true if any set is modified, false if they are identical.
+//
 template <typename SetType, typename AddFunctor, typename DelFunctor>
-void set_synchronize(SetType *set1, const SetType *set2,
+bool set_synchronize(SetType *set1, const SetType *set2,
                      AddFunctor add_fn, DelFunctor del_fn) {
     typename SetType::iterator it1 = set1->begin(), next1 = set1->begin();
     typename SetType::const_iterator it2 = set2->begin();
+    bool modified = false;
     while (it1 != set1->end() && it2 != set2->end()) {
         if (*it1 < *it2) {
             ++next1;
+            modified = true;
             del_fn(it1);
             it1 = next1;
         } else if (*it1 > *it2) {
+            modified = true;
             add_fn(it2);
             ++it2;
         } else {
@@ -36,11 +41,14 @@ void set_synchronize(SetType *set1, const SetType *set2,
     }
     for (next1 = it1; it1 != set1->end(); it1 = next1) {
         ++next1;
+        modified = true;
         del_fn(it1);
     }
     for (; it2 != set2->end(); ++it2) {
+        modified = true;
         add_fn(it2);
     }
+    return modified;
 }
 
 #endif  // BASE_SET_UTIL_H
diff --git a/src/bgp/mvpn/mvpn_table.cc b/src/bgp/mvpn/mvpn_table.cc
new file mode 100644
index 0000000..88231b8
--- /dev/null
+++ b/src/bgp/mvpn/mvpn_table.cc
@@ -0,0 +1,62 @@
+/*
+ * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
+ */
+
+#include "bgp/mvpn/mvpn_table.h"
+
+MvpnTable::MvpnTable(DB *db, const string &name) :
+    BgpTable(db, name), resolver_(new PathResolver(this, true)) {
+}
+
+bool MvpnTable::BgpRouteNotify(BgpServer *server, DBTablePartBase *root,
+                               DBEntryBase *entry) {
+    BgpTable *table = static_cast<BgpTable *>(root->parent());
+    BgpRoute *route = static_cast<BgpRoute *> (entry);
+
+    // Trigger RPF check if necessary.
+    BgpPath *path = FindType7Path(); // Can we have exthop as C-S ??
+    BgpTable *inet_table = table->instance()->GetTable(Address::Inet);
+    resolver_->StartPathResolution(root->index(), path, route, inet_table);
+}
+
+void MvpnTable::ProcessResolvedRoutes(BgpTable *src_table, BgpRoute *source_rt,
+        const BgpPath *src_path, ExtCommunityPtr community) {
+    // In case of bgp.mvpn.0 table, is src_rt is type-7 <C-S,G> route and
+    // route is now resolvable (or otherwise), replicate or delete C-S,G route
+    // to advertise/withdraw from the ingress root PE node.
+    const BgpRoute *resolved_rt = NULL;
+    if (src_table->resolver()->rnexthop())
+        resolved_rt = src_table->resolver()->rnexthop()->route();
+    if (!resolved_rt)
+        return;
+    const BgpPath *path = src_table->resolver()->FindResolvedPath(source_rt,
+                                                                  src_path);
+    if (!path)
+        return;
+    const BgpAttr *attr = path->GetAttr();
+    const ExtCommunity *ext_community = attr->ext_community();
+    if (!ext_community)
+        return;
+
+    bool rt_import_found = false;
+    // Use rt-import from the resolved path as export route-target.
+    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
+                  ext_community->communities()) {
+        if (ExtCommunity::is_route_import_target(comm)) {
+            rt_import_found = true;
+            break;
+        }
+    }
+
+    if (!rt_import_found)
+        return;
+
+    // Replicate path using rd of the resolved path as part of the prefix
+    // and append ASN and C-S,G also to the prefix.
+    RouteDistinguisher rd = resolved_rt->GetRouteDistinguisher();
+}
+
+BgpRoute *MvpnTable::RouteReplicate(BgpServer *server, BgpTable *src_table,
+    BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
+    ProcessResolvedRoutes(src_table, source_rt, src_path, community);
+}
diff --git a/src/bgp/routing-instance/path_resolver.cc b/src/bgp/routing-instance/path_resolver.cc
index 9c6ce48..a7e726b 100644
--- a/src/bgp/routing-instance/path_resolver.cc
+++ b/src/bgp/routing-instance/path_resolver.cc
@@ -25,12 +25,14 @@ using std::vector;
 //
 // Return true if the prefix for the BgpRoute is the same as given IpAddress.
 //
-static bool RoutePrefixIsAddress(Address::Family family, const BgpRoute *route,
-    const IpAddress &address) {
+bool PathResolver::RoutePrefixMatch(Address::Family family,
+                                    const BgpRoute *route,
+                                    const IpAddress &address) {
     if (family == Address::INET) {
         const InetRoute *inet_route = static_cast<const InetRoute *>(route);
-        if (inet_route->GetPrefix().addr() == address.to_v4() &&
-            inet_route->GetPrefix().prefixlen() == Address::kMaxV4PrefixLen) {
+        uint32_t mask = ~((1 << (32 - inet_route->GetPrefix().prefixlen()))-1);
+        if ((address.to_v4().to_ulong() & mask) ==
+                inet_route->GetPrefix().addr().to_ulong()) {
             return true;
         }
     } else if (family == Address::INET6) {
@@ -73,11 +75,12 @@ private:
 // The listener_id if used to set state on BgpRoutes for BgpPaths that have
 // requested resolution.
 //
-PathResolver::PathResolver(BgpTable *table)
+PathResolver::PathResolver(BgpTable *table, bool resolution_only)
     : table_(table),
       listener_id_(table->Register(
           boost::bind(&PathResolver::RouteListener, this, _1, _2),
           "PathResolver")),
+      resolution_only_(resolution_only),
       nexthop_reg_unreg_trigger_(new TaskTrigger(
           boost::bind(&PathResolver::ProcessResolverNexthopRegUnregList, this),
           TaskScheduler::GetInstance()->GetTaskId("bgp::Config"),
@@ -214,6 +217,13 @@ ResolverRouteState *PathResolver::FindResolverRouteState(BgpRoute *route) {
     return state;
 }
 
+const BgpPath *PathResolver::FindResolvedPath(const BgpRoute *route,
+                                              const BgpPath *path) {
+    ResolverPath *resolver_path = GetPartition(route->get_table_partition()->
+        index())->FindResolverPath(path);
+    return resolver_path ? resolver_path->FindResolvedPath() : NULL;
+}
+
 //
 // Find or create ResolverRouteState for the given BgpRoute.
 //
@@ -656,8 +666,10 @@ void PathResolverPartition::StartPathResolution(const BgpPath *path,
 
     Address::Family family = table()->family();
     IpAddress address = path->GetAttr()->nexthop();
-    if (table() == nh_table && RoutePrefixIsAddress(family, route, address))
+    if (table() == nh_table &&
+            resolver_->RoutePrefixMatch(family, route, address)) {
         return;
+    }
 
     ResolverNexthop *rnexthop =
         resolver_->LocateResolverNexthop(address, nh_table);
@@ -886,9 +898,10 @@ ResolverPath::~ResolverPath() {
 //
 void ResolverPath::AddResolvedPath(ResolvedPathList::const_iterator it) {
     BgpPath *path = *it;
-    const IPeer *peer = path->GetPeer();
     resolved_path_list_.insert(path);
-    route_->InsertPath(path);
+    if (partition_->resolver()->resolution_only())
+        return;
+    const IPeer *peer = path->GetPeer();
     BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
         "Added resolved path " << route_->ToString() <<
         " peer " << (peer ? peer->ToString() : "None") <<
@@ -896,6 +909,7 @@ void ResolverPath::AddResolvedPath(ResolvedPathList::const_iterator it) {
         " nexthop " << path->GetAttr()->nexthop().to_string() <<
         " label " << path->GetLabel() <<
         " in table " << partition_->table()->name());
+    route_->InsertPath(path);
 }
 
 //
@@ -904,6 +918,9 @@ void ResolverPath::AddResolvedPath(ResolvedPathList::const_iterator it) {
 //
 void ResolverPath::DeleteResolvedPath(ResolvedPathList::const_iterator it) {
     BgpPath *path = *it;
+    resolved_path_list_.erase(it);
+    if (partition_->resolver()->resolution_only())
+        return;
     const IPeer *peer = path->GetPeer();
     BGP_LOG_STR(BgpMessage, SandeshLevel::SYS_DEBUG, BGP_LOG_FLAG_TRACE,
         "Deleted resolved path " << route_->ToString() <<
@@ -913,7 +930,6 @@ void ResolverPath::DeleteResolvedPath(ResolvedPathList::const_iterator it) {
         " label " << path->GetLabel() <<
         " in table " << partition_->table()->name());
     route_->DeletePath(path);
-    resolved_path_list_.erase(it);
 }
 
 //
@@ -1066,15 +1082,19 @@ bool ResolverPath::UpdateResolvedPaths() {
         BgpPath *resolved_path =
             LocateResolvedPath(peer, path_id, attr.get(), nh_path->GetLabel());
         future_resolved_path_list.insert(resolved_path);
+        if (partition_->resolver()->resolution_only())
+            break;
     }
 
     // Reconcile the current and future resolved paths and notify/delete the
     // route as appropriate.
-    set_synchronize(&resolved_path_list_, &future_resolved_path_list,
+    bool modified;
+    modified = set_synchronize(&resolved_path_list_, &future_resolved_path_list,
         boost::bind(&ResolverPath::AddResolvedPath, this, _1),
         boost::bind(&ResolverPath::DeleteResolvedPath, this, _1));
     if (route_->BestPath()) {
-        partition_->table_partition()->Notify(route_);
+        if (modified)
+            partition_->table_partition()->Notify(route_);
     } else {
         partition_->table_partition()->Delete(route_);
     }
@@ -1092,7 +1112,6 @@ ResolverNexthop::ResolverNexthop(PathResolver *resolver, IpAddress address,
       address_(address),
       table_(table),
       registered_(false),
-      route_(NULL),
       rpath_lists_(DB::PartitionCount()),
       table_delete_ref_(this, table->deleter()) {
 }
@@ -1121,7 +1140,7 @@ bool ResolverNexthop::Match(BgpServer *server, BgpTable *table,
     // Ignore if the route doesn't match the address.
     Address::Family family = table->family();
     assert(family == Address::INET || family == Address::INET6);
-    if (!RoutePrefixIsAddress(family, route, address_))
+    if (!resolver_->RoutePrefixMatch(family, route, address_))
         return false;
 
     // Set or remove MatchState as appropriate.
@@ -1130,14 +1149,14 @@ bool ResolverNexthop::Match(BgpServer *server, BgpTable *table,
     bool state_added = condition_listener->CheckMatchState(table, route, this);
     if (deleted) {
         if (state_added) {
-            route_ = NULL;
+            erase(route);
             condition_listener->RemoveMatchState(table, route, this);
         } else {
             return false;
         }
     } else {
         if (!state_added) {
-            route_ = route;
+            insert(route);
             condition_listener->SetMatchState(table, route, this);
         }
     }
@@ -1191,12 +1210,12 @@ void ResolverNexthop::RemoveResolverPath(int part_id, ResolverPath *rpath) {
 }
 
 ResolverRouteState *ResolverNexthop::GetResolverRouteState() {
-    if (!route_)
+    if (!route())
         return NULL;
     PathResolver *nh_resolver = table_->path_resolver();
     if (!nh_resolver)
         return NULL;
-    return nh_resolver->FindResolverRouteState(route_);
+    return nh_resolver->FindResolverRouteState(route());
 }
 
 //
@@ -1228,3 +1247,40 @@ bool ResolverNexthop::empty() const {
     }
     return true;
 }
+
+bool ResolverNexthop::ResolverRouteCompare::operator() (
+    const BgpRoute *l, const BgpRoute *r) const {
+    BgpTable *table = static_cast<BgpTable *>(l->get_table());
+    Address::Family family = table->family();
+    if (family == Address::INET) {
+        const InetRoute *lhs = static_cast<const InetRoute *>(l);
+        const InetRoute *rhs_inet = static_cast<const InetRoute *>(r);
+        return lhs->GetPrefix().prefixlen() > rhs_inet->GetPrefix().prefixlen();
+    }
+
+    if (family == Address::INET6) {
+        const Inet6Route *lhs = static_cast<const Inet6Route *>(l);
+        const Inet6Route *rhs = static_cast<const Inet6Route *>(r);
+        return lhs->GetPrefix().prefixlen() > rhs->GetPrefix().prefixlen();
+    }
+
+    assert(false);
+    return true;
+}
+
+void ResolverNexthop::insert(BgpRoute *route) {
+    tbb::mutex::scoped_lock lock(routes_mutex_);
+    routes_.insert(route);
+}
+void ResolverNexthop::erase(BgpRoute *route) {
+    tbb::mutex::scoped_lock lock(routes_mutex_);
+    routes_.erase(route);
+}
+const BgpRoute *ResolverNexthop::route() const {
+    tbb::mutex::scoped_lock lock(routes_mutex_);
+    return !routes_.empty() ? *(routes_.begin()) : NULL;
+}
+BgpRoute *ResolverNexthop::route() {
+    tbb::mutex::scoped_lock lock(routes_mutex_);
+    return !routes_.empty() ? *(routes_.begin()) : NULL;
+}
diff --git a/src/bgp/routing-instance/path_resolver.h b/src/bgp/routing-instance/path_resolver.h
index 759a7aa..1e8699f 100644
--- a/src/bgp/routing-instance/path_resolver.h
+++ b/src/bgp/routing-instance/path_resolver.h
@@ -91,7 +91,7 @@ class TaskTrigger;
 //
 class PathResolver {
 public:
-    explicit PathResolver(BgpTable *table);
+    PathResolver(BgpTable *table, bool resolution_only = false);
     ~PathResolver();
 
     void StartPathResolution(int part_id, const BgpPath *path, BgpRoute *route,
@@ -107,6 +107,8 @@ public:
     Address::Family family() const;
     DBTableBase::ListenerId listener_id() const { return listener_id_; }
     BgpConditionListener *get_condition_listener(Address::Family family);
+    const BgpPath *FindResolvedPath(const BgpRoute *route, const BgpPath *path);
+    bool resolution_only() const { return resolution_only_; }
 
     bool IsDeleted() const;
     void ManagedDelete();
@@ -114,6 +116,8 @@ public:
     void RetryDelete();
 
     void FillShowInfo(ShowPathResolver *spr, bool summary) const;
+    bool RoutePrefixMatch(Address::Family family, const BgpRoute *route,
+                          const IpAddress &address);
 
 private:
     friend class PathResolverPartition;
@@ -158,6 +162,7 @@ private:
 
     BgpTable *table_;
     DBTableBase::ListenerId listener_id_;
+    bool resolution_only_;
     mutable tbb::mutex mutex_;
     ResolverNexthopMap nexthop_map_;
     ResolverNexthopList nexthop_reg_unreg_list_;
@@ -321,6 +326,9 @@ public:
     BgpRoute *route() const { return route_; }
     const ResolverNexthop *rnexthop() const { return rnexthop_; }
     void clear_path() { path_ = NULL; }
+    const BgpPath *FindResolvedPath() const {
+        return resolved_path_count() ?  *(resolved_path_list_.begin()) : NULL;
+    }
     size_t resolved_path_count() const { return resolved_path_list_.size(); }
 
 private:
@@ -402,20 +410,28 @@ public:
 
     IpAddress address() const { return address_; }
     BgpTable *table() const { return table_; }
-    const BgpRoute *route() const { return route_; }
-    BgpRoute *route() { return route_; }
+    void insert(BgpRoute *route);
+    void erase(BgpRoute *route);
+    const BgpRoute *route() const;
+    BgpRoute *route();
     bool empty() const;
     bool registered() const { return registered_; }
     void set_registered() { registered_ = true; }
 
 private:
+    struct ResolverRouteCompare {
+        bool operator()(const BgpRoute *l, const BgpRoute *r) const;
+    };
+
     typedef std::set<ResolverPath *> ResolverPathList;
+    typedef std::set<BgpRoute *, ResolverRouteCompare> ResolverRouteSet;
 
     PathResolver *resolver_;
     IpAddress address_;
     BgpTable *table_;
     bool registered_;
-    BgpRoute *route_;
+    ResolverRouteSet routes_;
+    mutable tbb::mutex routes_mutex_;
     std::vector<ResolverPathList> rpath_lists_;
     LifetimeRef<ResolverNexthop> table_delete_ref_;
 
diff --git a/src/bgp/test/path_resolver_test.cc b/src/bgp/test/path_resolver_test.cc
index 0921a72..37c3163 100644
--- a/src/bgp/test/path_resolver_test.cc
+++ b/src/bgp/test/path_resolver_test.cc
@@ -853,13 +853,28 @@ TYPED_TEST(PathResolverTest, SinglePrefix1) {
         this->BuildHostAddress(bgp_peer1->ToString()));
 
     this->AddXmppPath(xmpp_peer1, "blue",
-        this->BuildPrefix(bgp_peer1->ToString(), 32),
+        this->BuildPrefix(bgp_peer1->ToString(), 24),
         this->BuildNextHopAddress("172.16.1.1"), 10000);
     this->VerifyPathAttributes("blue", this->BuildPrefix(1), bgp_peer1,
         this->BuildNextHopAddress("172.16.1.1"), 10000);
 
+    // Add more specific match and verify that nexthop resolves to the newer
+    // more specific route.
+    this->AddXmppPath(xmpp_peer1, "blue",
+        this->BuildPrefix(bgp_peer1->ToString(), 28),
+        this->BuildNextHopAddress("172.16.1.1"), 20000);
+    this->VerifyPathAttributes("blue", this->BuildPrefix(1), bgp_peer1,
+        this->BuildNextHopAddress("172.16.1.1"), 20000);
+
+    // Delete more specific route and verify that nexthop resolves older
+    // less specific route.
     this->DeleteXmppPath(xmpp_peer1, "blue",
-        this->BuildPrefix(bgp_peer1->ToString(), 32));
+        this->BuildPrefix(bgp_peer1->ToString(), 28));
+    this->VerifyPathAttributes("blue", this->BuildPrefix(1), bgp_peer1,
+        this->BuildNextHopAddress("172.16.1.1"), 10000);
+
+    this->DeleteXmppPath(xmpp_peer1, "blue",
+        this->BuildPrefix(bgp_peer1->ToString(), 24));
     this->VerifyPathNoExists("blue", this->BuildPrefix(1), bgp_peer1,
         this->BuildNextHopAddress("172.16.1.1"));
 
```

When <S,G> Type-7 route sent by egress PE is successfully imported into
vrf.mvpn.0 (by matching auto-generated rt-import route-target) by the ingress
PE, if provider-tunnel is configured (as shown above), then ingress PE should
generate Type 3 C-<S, G> S-PMSI AD Route into vrf.mvpn.0 table. From here, this
would replicated to bgp.mvpn.0 and is advertised to remote (egress) PEs.

```3:<root-rd>:<C,G>:<Sender-PE-Router-Id> (Page 240)```

Target would be the export target of the vrf (so it would get imported into
vrf.mvpn.0 in the egress pe)

PMSI Flags: Leaf Information Required 1 (So that egress can initiate the join
for the pmsi tunnel) (Similar to RSVP based S-PMSI, Page 251)
TunnelType: Ingress-Replication, Label:0

## 4.7 PMSI Tunnel advertisement from egress (Leaf AD)

When type-3 route is imported into vrf.mvpn.0 table at the egress, (such as
control-node), then if there is an active type-7 route advertised from this
table to the same ingress PE, (which means there are some receivers interested),
then a new type-4 Leaf AD route is added to vrf.inet.0 (Page 254)

```
4:<S-PMSI-Type-3-Route-Prefix>:<Receive-PE-Router-ID>
Route target: <Ingress-PE-Router-ID>:0
PMSI Tunnel: Tunnel type: Ingress-Replication and label:Tree-Label
```
? It goes into correct vrf only because the prefix itself is copied (??)

## 4.8 Source AS extended community
This is mainly applicable for inter-as mvpn stitched over segmented tunnels.
This is not targeted for Phase 1.

## 4.9 Multicast Edge Replicated Tree
Build one tree for each unique combination of C-<S, G> under each tenant.
Note: <S,G> is expected to be unique with a tenant (project) space.

Tree can be built using very similar logic used in ermvpn.

Master control-node which builds level-0 and level-1 tree shall also be solely
responsible for advertising mvpn routes. Today, this is simply decided based on
control-node with lowest router-id.

Agent shall continue to advertise routes related to BUM traffic over xmpp to
vrf.ermvpn.0 table. However, all mvpn routes (which are based on IGMP joins
or leaves) are always advertised over XMPP to <project>.__default__.mvpn.0
table. (i.e, the name of the VN/VRF is an internally defined constant)

Control-node would build an edge replicated multicast tree like how it does so
already. PMSI Tunnel information is gathered from root-node of each of these
trees. (1:1 mapping between root node of a tree and mvpn Type-3 leaf AD route)
based on C-<S, G>.

When advertising Type3 Leaf AD Route (Section 4.7), egress must also advertise
PMSI tunnel information for forwarding to happen. MVpn Manager shall use the
root node ingress label as the label value in PMSI tunnel attribute

When ever tree is recomputed (and only if root node changes), Type3 route is
updated and re-advertised as necessary. If a new node is selected as root,
Type-3 route can be simply updated and re-advertised (implicit withdraw and
new update)

ErmVpn Tree built inside the contrail cluster is fully bi-directional and self
contained. Vrouter would flood the packets within the tree only so long as the
packet was originated from one of the nodes inside the tree (in the oif list)

This is no longer true when a single node of the tree is stitched with the SDN
gateway using ingress replication. The stitched node should be programmed to
accept multicast packets from SDN gateway (over the GRE/UDP tunnel) and then
flood them among all the nodes contained inside the tree. This is done by
encoding a new "Input Tunnel Attribute" to the xmpp route sent to the agent.
This attribute shall contain the IP address of the tunnel end point (SDN GW)
as well as the Tunnel Type (MPLS over GR/MPLS over UDP) as appropriate.

Vrouter should relax its checks and indeed accept received multicast packets
from this SDN gateway even though that incoming interface may not be part of the
oif list.

When ever GlobalTreeRoute is added/modified, MVpn Manager would get notified as
a listener. This callback happens in the context of ErmVpn table. MVpn Manager
should find associated MVpn Route by inspecting the DB State (of the
GlobalTreeRoute) and notify associated MVpn Type-3 (Leaf AD) if any. During
this operation, necessary forwarding information such as Input label, tunnel
type must be found from the GlobalTreeRoute and stored inside the DB State
associated with the 

# 5. Performance and scaling impact

##5.2 Forwarding performance

# 6. Upgrade

# 7. Deprecations
####If this feature deprecates any older feature or API then list it here.

# 8. Dependencies
####Describe dependent features or components.

# 9. Testing
## 9.1 Unit tests

## 9.2 Dev tests
## 9.3 System tests

# 10. Documentation Impact

# 11. References
1. [Multicast in MPLS/BGP IP VPNs](https://tools.ietf.org/html/rfc6513)
2. [BGP Encodings and Procedures for Multicast in MPLS/BGP IP VPNs](https://tools.ietf.org/html/rfc6514)
3. [Ingress Replication Tunnels in Multicast VPN](https://tools.ietf.org/html/rfc7988)
4. [Extranet Multicast in BGP/IP MPLS VPNs](https://tools.ietf.org/html/rfc7900)
5. [BGP/MPLS IP Virtual Private Networks (VPNs)](https://tools.ietf.org/html/rfc4364)
