/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/mvpn/mvpn_table.h"

#include <boost/foreach.hpp>

#include "base/task_annotations.h"
#include "bgp/ermvpn/ermvpn_table.h"
#include "bgp/extended-community/source_as.h"
#include "bgp/ipeer.h"
#include "bgp/bgp_factory.h"
#include "bgp/bgp_multicast.h"
#include "bgp/bgp_mvpn.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_update.h"
#include "bgp/inet/inet_table.h"
#include "bgp/origin-vn/origin_vn.h"
#include "bgp/routing-instance/path_resolver.h"
#include "bgp/routing-instance/routing_instance.h"

using std::auto_ptr;
using std::string;

size_t MvpnTable::HashFunction(const MvpnPrefix &prefix) const {
    if ((prefix.type() == MvpnPrefix::IntraASPMSIADRoute) ||
           (prefix.type() == MvpnPrefix::LeafADRoute)) {
        uint32_t data = prefix.originator().to_ulong();
        return boost::hash_value(data);
    }
    if (prefix.type() == MvpnPrefix::InterASPMSIADRoute) {
        uint32_t data = prefix.asn();
        return boost::hash_value(data);
    }
    return boost::hash_value(prefix.group().to_ulong());
}

MvpnTable::MvpnTable(DB *db, const string &name)
    : BgpTable(db, name), manager_(NULL), force_replication_(false) {
}

PathResolver *MvpnTable::CreatePathResolver() {
    if (routing_instance()->IsMasterRoutingInstance())
        return NULL;
    return (new PathResolver(this));
}

auto_ptr<DBEntry> MvpnTable::AllocEntry(
    const DBRequestKey *key) const {
    const RequestKey *pfxkey = static_cast<const RequestKey *>(key);
    return auto_ptr<DBEntry> (new MvpnRoute(pfxkey->prefix));
}

auto_ptr<DBEntry> MvpnTable::AllocEntryStr(
    const string &key_str) const {
    MvpnPrefix prefix = MvpnPrefix::FromString(key_str);
    return auto_ptr<DBEntry> (new MvpnRoute(prefix));
}

size_t MvpnTable::Hash(const DBEntry *entry) const {
    const MvpnRoute *rt_entry = static_cast<const MvpnRoute *>(entry);
    const MvpnPrefix &mvpnprefix = rt_entry->GetPrefix();
    size_t value = MvpnTable::HashFunction(mvpnprefix);
    return value % kPartitionCount;
}

size_t MvpnTable::Hash(const DBRequestKey *key) const {
    const RequestKey *rkey = static_cast<const RequestKey *>(key);
    Ip4Prefix prefix(rkey->prefix.group(), 32);
    size_t value = InetTable::HashFunction(prefix);
    return value % kPartitionCount;
}

BgpRoute *MvpnTable::TableFind(DBTablePartition *rtp,
    const DBRequestKey *prefix) {
    const RequestKey *pfxkey = static_cast<const RequestKey *>(prefix);
    MvpnRoute rt_key(pfxkey->prefix);
    return static_cast<BgpRoute *>(rtp->Find(&rt_key));
}

DBTableBase *MvpnTable::CreateTable(DB *db, const string &name) {
    MvpnTable *table = new MvpnTable(db, name);
    table->Init();
    return table;
}

bool MvpnTable::Export(RibOut *ribout, Route *route,
    const RibPeerSet &peerset, UpdateInfoSList &uinfo_slist) {
    MvpnRoute *mvpn_route = dynamic_cast<MvpnRoute *>(route);

    if (ribout->IsEncodingXmpp()) {
        UpdateInfo *uinfo = GetMvpnUpdateInfo(ribout, mvpn_route, peerset);
        if (!uinfo)
            return false;
        uinfo_slist->push_front(*uinfo);
        return true;
    }

    uint8_t rt_type = mvpn_route->GetPrefix().type();
    if (ribout->peer_type() == BgpProto::EBGP &&
                rt_type == MvpnPrefix::IntraASPMSIADRoute) {
        return false;
    }
    if (ribout->peer_type() == BgpProto::IBGP &&
                rt_type == MvpnPrefix::InterASPMSIADRoute) {
        return false;
    }
    BgpRoute *bgp_route = static_cast<BgpRoute *> (route);
    UpdateInfo *uinfo = GetUpdateInfo(ribout, bgp_route, peerset);
    if (!uinfo)
        return false;
    uinfo_slist->push_front(*uinfo);
    return true;
}

void MvpnTable::CreateManager() {
    // Don't create the MvpnManager for the VPN table.
    if (IsMaster())
        return;
    assert(!manager_);
    manager_ = BgpObjectFactory::Create<MvpnManager>(this);
    manager_->Initialize();
}

void MvpnTable::DestroyManager() {
    assert(manager_);
    manager_->Terminate();
    delete manager_;
    manager_ = NULL;
}

void MvpnTable::set_routing_instance(RoutingInstance *rtinstance) {
    BgpTable::set_routing_instance(rtinstance);
    CreateManager();
}

RouteDistinguisher MvpnTable::GetSourceRouteDistinguisher(
    const BgpPath *path) const {
    if (!manager_)
        return RouteDistinguisher();
    return manager_->GetSourceRouteDistinguisher(path);
}

bool MvpnTable::IsMaster() const {
    return routing_instance()->IsMasterRoutingInstance();
}

// Call the const version to avoid code duplication.
MvpnProjectManager *MvpnTable::GetProjectManager() {
    return const_cast<MvpnProjectManager *>(
        static_cast<const MvpnTable *>(this)->GetProjectManager());
}

// Call the const version to avoid code duplication.
MvpnProjectManagerPartition *MvpnTable::GetProjectManagerPartition(
        BgpRoute *rt) {
    return const_cast<MvpnProjectManagerPartition *>(
        static_cast<const MvpnTable *>(this)->GetProjectManagerPartition(rt));
}

////////////////////////////////////////////////////////////////////////////////
///                Route Replication and export Functions                    ///
////////////////////////////////////////////////////////////////////////////////

// Export MVPN routes
UpdateInfo *MvpnTable::GetMvpnUpdateInfo(RibOut *ribout, MvpnRoute *route,
    const RibPeerSet &peerset) {
    // TODO(Ananth) if it is not a "Sender" route, ignore.
    if (route->GetPrefix().type() != MvpnPrefix::SourceActiveADRoute)
        return NULL;
    if (!route->IsUsable())
        return NULL;

    MvpnProjectManager *pm = GetProjectManager();
    return pm ? pm->GetUpdateInfo(route) : NULL;
}

// Get MvpnProjectManager object for this Mvpn. Each MVPN network is associated
// with a parent project maanger network via configuration. MvpnProjectManager
// is retrieved from this parent network RoutingInstance's ErmVpnTable.
const MvpnProjectManager *MvpnTable::GetProjectManager() const {
    std::string pm_network =
        routing_instance()->mvpn_project_manager_network();
    if (pm_network.empty())
        return NULL;
    const RoutingInstance *rtinstance =
        routing_instance()->manager()->GetRoutingInstance(pm_network);
    if (!rtinstance || rtinstance->deleted())
        return NULL;
    const ErmVpnTable *table = dynamic_cast<const ErmVpnTable *>(
        rtinstance->GetTable(Address::ERMVPN));
    if (!table || table->IsDeleted())
        return NULL;
    return table->mvpn_project_manager();
}

// Return the MvpnProjectManagerPartition for this route using the same DB
// partition index as of the route.
const MvpnProjectManagerPartition *MvpnTable::GetProjectManagerPartition(
        BgpRoute *route) const {
    const MvpnProjectManager *manager = GetProjectManager();
    if (!manager)
        return NULL;
    int part_id = route->get_table_partition()->index();
    return manager->GetPartition(part_id);
}

// Override virtual method to retrive target table for MVPN routes. For now,
// only Type-4 LeafAD routes require special treatment, as they always come
// with the same route target <router-id>:0. Hence, if normal rtf selection
// mode is used, every table with MVPN enalbled would have to be notified for
// replication. Instead, find the table based on the correspondong S-PMSI route.
// This route can be retrieved from the MVPN state of the <S-G> maintained in
// the MvpnProjectManagerPartition object.
void MvpnTable::UpdateSecondaryTablesForReplication(BgpRoute *rt,
        TableSet *secondary_tables) {
    if (IsMaster())
        return;
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(rt);
    assert(mvpn_rt);

    // Special table lookup is required only for the Type4 LeafAD routes.
    if (mvpn_rt->GetPrefix().type() != MvpnPrefix::LeafADRoute)
        return;

    // Find the right MvpnProjectManagerPartition based on the rt's partition.
    const MvpnProjectManagerPartition *partition =
        GetProjectManagerPartition(rt);
    if (!partition)
        return;

    // Retrieve MVPN state. Ignore if there is no state or if there is no usable
    // Type3 SPMSI route 0associated with it (perhaps it was deleted already).
    MvpnState::SG sg(mvpn_rt);
    MvpnStatePtr state = partition->GetState(sg);
    if (!state || !state->spmsi_rt() || !state->spmsi_rt()->IsUsable())
        return;

    // Matching Type-3 S-PMSI route was found. Return the table that holds this
    // route, if it is usable.
    BgpTable *table = dynamic_cast<BgpTable *>(
        state->spmsi_rt()->get_table_partition()->parent());
    assert(table);

    // Update table list to let replicator invoke RouteReplicate() for this
    // LeafAD route for this table which has the corresponding Type3 SPMSI
    // route. This was originated as the 'Sender' since receiver joined to
    // the <C-S,G> group.
    secondary_tables->insert(table);
}

// Find or create the route.
MvpnRoute *MvpnTable::FindRoute(MvpnPrefix &prefix) {
    MvpnRoute rt_key(prefix);
    DBTablePartition *rtp = static_cast<DBTablePartition *>(
        GetTablePartition(&rt_key));
    return static_cast<MvpnRoute *>(rtp->Find(&rt_key));
}

const MvpnRoute *MvpnTable::FindRoute(MvpnPrefix &prefix) const {
    return const_cast<MvpnRoute *>(
        static_cast<const MvpnTable *>(this)->FindRoute(prefix));
}

// Find or create the route.
MvpnRoute *MvpnTable::LocateRoute(MvpnPrefix &prefix) {
    MvpnRoute rt_key(prefix);
    DBTablePartition *rtp = static_cast<DBTablePartition *>(
        GetTablePartition(&rt_key));
    MvpnRoute *dest_route = static_cast<MvpnRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new MvpnRoute(prefix);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }
    return dest_route;
}

MvpnPrefix MvpnTable::CreateType4LeafADRoutePrefix(const MvpnRoute *type3_rt) {
    assert(type3_rt->GetPrefix().type() == MvpnPrefix::SPMSIADRoute);
    const Ip4Address originator_ip(server()->bgp_identifier());
    MvpnPrefix prefix(MvpnPrefix::LeafADRoute, originator_ip);
    prefix.SetRtKeyFromSPMSIADRoute(type3_rt->GetPrefix());
    return prefix;
}

MvpnRoute *MvpnTable::LocateType4LeafADRoute(const MvpnRoute *type3_spmsi_rt) {
    MvpnPrefix prefix = CreateType4LeafADRoutePrefix(type3_spmsi_rt);
    return LocateRoute(prefix);
}

MvpnPrefix MvpnTable::CreateType3SPMSIRoutePrefix(MvpnRoute *type7_rt) {
    assert(type7_rt->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute);
    const RouteDistinguisher *rd = routing_instance()->GetRD();
    Ip4Address source = type7_rt->GetPrefix().source();
    Ip4Address group = type7_rt->GetPrefix().group();
    const Ip4Address originator_ip(server()->bgp_identifier());
    MvpnPrefix prefix(MvpnPrefix::SPMSIADRoute, *rd, originator_ip,
            group, source);
    return prefix;
}

MvpnPrefix MvpnTable::CreateType4SourceActiveRoutePrefix(MvpnRoute *rt) const {
    const RouteDistinguisher *rd = routing_instance()->GetRD();
    Ip4Address source = rt->GetPrefix().source();
    Ip4Address group = rt->GetPrefix().group();
    const Ip4Address originator_ip(server()->bgp_identifier());
    MvpnPrefix prefix(MvpnPrefix::SourceActiveADRoute, *rd, originator_ip,
                      group, source);
    return prefix;
}

MvpnRoute *MvpnTable::LocateType3SPMSIRoute(MvpnRoute *type7_rt) {
    MvpnPrefix prefix = CreateType3SPMSIRoutePrefix(type7_rt);
    return LocateRoute(prefix);
}

MvpnPrefix MvpnTable::CreateType2ADRoutePrefix() {
    const RouteDistinguisher *rd = routing_instance()->GetRD();
    MvpnPrefix prefix(MvpnPrefix::InterASPMSIADRoute, *rd,
            server()->autonomous_system());
    return prefix;
}

MvpnRoute *MvpnTable::LocateType2ADRoute() {
    MvpnPrefix prefix = CreateType2ADRoutePrefix();
    return LocateRoute(prefix);
}

MvpnPrefix MvpnTable::CreateType1ADRoutePrefix() {
    const RouteDistinguisher *rd = routing_instance()->GetRD();
    const Ip4Address originator_ip(server()->bgp_identifier());
    MvpnPrefix prefix(MvpnPrefix::IntraASPMSIADRoute, *rd, originator_ip);
    return prefix;
}

MvpnRoute *MvpnTable::LocateType1ADRoute() {
    MvpnPrefix prefix = CreateType1ADRoutePrefix();
    return LocateRoute(prefix);
}

MvpnRoute *MvpnTable::FindType1ADRoute() {
    MvpnPrefix prefix = CreateType1ADRoutePrefix();
    return FindRoute(prefix);
}

MvpnRoute *MvpnTable::FindType2ADRoute() {
    MvpnPrefix prefix = CreateType2ADRoutePrefix();
    return FindRoute(prefix);
}

MvpnRoute *MvpnTable::FindType4SourceActiveADRoute(MvpnRoute *rt) {
    MvpnPrefix prefix = CreateType4SourceActiveRoutePrefix(rt);
    return FindRoute(prefix);
}

const MvpnRoute *MvpnTable::FindType4SourceActiveADRoute(MvpnRoute *rt) const {
    MvpnPrefix prefix = CreateType4SourceActiveRoutePrefix(rt);
    return FindRoute(prefix);
}

// RouteReplicate() method called from the RouteReplicator during replication.
//
// Handle MVPN routes appropriately based on the route type. Return replicated
// route, or NULL if not replicated.
BgpRoute *MvpnTable::RouteReplicate(BgpServer *server, BgpTable *table,
    BgpRoute *rt, const BgpPath *src_path, ExtCommunityPtr community) {
    CHECK_CONCURRENCY("db::DBTable");
    MvpnRoute *src_rt = dynamic_cast<MvpnRoute *>(rt);
    MvpnTable *src_table = dynamic_cast<MvpnTable *>(table);

    if (force_replication()) {
        return ReplicatePath(server, src_rt->GetPrefix(), src_table, src_rt,
                             src_path, community);
    }

    // Replicate Type7 C-Join route.
    if (src_rt->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute) {
        return ReplicateType7SourceTreeJoin(server, src_table, src_rt,
                                            src_path, community);
    }

    // Replicate all other types.
    // TODO(Ananth) Should we ignore types we don't support like Source-Active.
    return ReplicatePath(server, src_rt->GetPrefix(), src_table, src_rt,
                         src_path, community);
}

BgpRoute *MvpnTable::ReplicateType7SourceTreeJoin(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {

    // If replicating from Master table, no special checks are required.
    if (src_table->IsMaster()) {
        return ReplicatePath(server, src_rt->GetPrefix(), src_table, src_rt,
                             src_path, community);
    }

    // This is the case when routes are replicated either to Master or to other
    // vrf.mvpn.0 as identified the route targets. In either case, basic idea
    // is to target the replicated path directly to vrf where sender resides.
    //
    // Route-target of the target vrf is derived from the Vrf Import Target of
    // the route the source resolves to. Resolver code would have already
    // computed this and encoded inside source-rd. Also source-as to encode in
    // the RD is also encoded as part of the SourceAS extended community.
    const BgpAttr *attr = src_path->GetAttr();
    if (!attr)
        return NULL;

    // Do not resplicate if the source is not resolvable.
    if (attr->source_rd().IsZero())
        return NULL;

    // Find source-as extended-community. If not present, do not replicate
    bool source_as_found = false;
    SourceAs source_as;
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &value,
                  attr->ext_community()->communities()) {
        if (ExtCommunity::is_source_as(value)) {
            source_as_found = true;
            source_as = SourceAs(value);
            break;
        }
    }

    if (!source_as_found)
        return NULL;

    // No need to send SourceAS with this mvpn route. This is only sent along
    // with the unicast routes.
    community = server->extcomm_db()->RemoveSourceASAndLocate(community.get());

    // Replicate path using source route's<C-S,G>, source_rd and asn as encoded
    // in the source-as attribute.
    MvpnPrefix prefix(MvpnPrefix::SourceTreeJoinRoute, attr->source_rd(),
                      source_as.GetAsn(), src_rt->GetPrefix().group(),
                      src_rt->GetPrefix().source());

    // Replicate the path with the computed prefix and attributes.
    return ReplicatePath(server, prefix, src_table, src_rt, src_path,
                         community);
}

BgpRoute *MvpnTable::ReplicatePath(BgpServer *server, const MvpnPrefix &prefix,
        MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
        ExtCommunityPtr comm, BgpAttrPtr new_attr, bool *replicated) {
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
        GetTablePartition(&rt_key));
    BgpRoute *dest_route = static_cast<BgpRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new MvpnRoute(mvpn_prefix);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }

    new_attr = attr_db->ReplaceExtCommunityAndLocate(new_attr.get(), comm);

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
    if (replicated)
        *replicated = true;

    return dest_route;
}

// Find MVPN Source address from the route which needs to be resolved in order
// to propagate the join towards the sender.
const IpAddress MvpnTable::GetAddressToResolve(BgpRoute *route,
        const BgpPath *path) const {
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(route);
    assert(mvpn_rt->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute);
    return mvpn_rt->GetPrefix().sourceIpAddress();
}

const RouteTarget::List &MvpnTable::GetExportList(BgpRoute *rt) const {
    static RouteTarget::List empty_list;
    MvpnRoute *mvpn_route = dynamic_cast<MvpnRoute *>(rt);
    assert(mvpn_route);
    if (mvpn_route->GetPrefix().type() != MvpnPrefix::SourceTreeJoinRoute)
        return BgpTable::GetExportList(rt);
    return empty_list;
}

static void RegisterFactory() {
    DB::RegisterFactory("mvpn.0", &MvpnTable::CreateTable);
}

MODULE_INITIALIZER(RegisterFactory);
