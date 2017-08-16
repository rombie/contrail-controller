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
    : BgpTable(db, name), manager_(NULL) {
}

PathResolver *MvpnTable::CreatePathResolver() {
    if (routing_instance()->IsMasterRoutingInstance())
        return NULL;
    return (new PathResolver(this));
}

void MvpnTable::ResolvePath(BgpRoute *rt, BgpPath *path) {
    if (manager_)
        manager_->ResolvePath(routing_instance(), rt, path);
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

    // in phase 1 source is outside so no need to send anything
    // to agent
    if (!ribout->IsEncodingBgp())
        return false;

    MvpnRoute *mvpn_route = dynamic_cast<MvpnRoute *>(route);
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

////////////////////////////////////////////////////////////////////////////////
///                     Route Replication Functions                          ///
////////////////////////////////////////////////////////////////////////////////

// Get MvpnProjectManager object for this Mvpn. Each MVPN network is associated
// with a parent project maanger network via configuration. MvpnProjectManager
// is retrieved from this parent network RoutingInstance object.
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

MvpnProjectManager *MvpnTable::GetProjectManager() {
    return const_cast<MvpnProjectManager *>(
        static_cast<const MvpnTable *>(this)->GetProjectManager());
}

const MvpnProjectManagerPartition *MvpnTable::GetProjectManagerPartition(
        BgpRoute *route) const {
    const MvpnProjectManager *manager = GetProjectManager();
    if (!manager)
        return NULL;
    int part_id = route->get_table_partition()->index();
    return manager->GetPartition(part_id);
}

MvpnProjectManagerPartition *MvpnTable::GetProjectManagerPartition(
        BgpRoute *rt) {
    return const_cast<MvpnProjectManagerPartition *>(
        static_cast<const MvpnTable *>(this)->GetProjectManagerPartition(rt));
}

void MvpnTable::UpdateSecondaryTablesForReplication(BgpRoute *rt,
        TableSet *secondary_tables) {
    if (IsMaster())
        return;
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(rt);
    assert(mvpn_rt);
    if (mvpn_rt->GetPrefix().type() != MvpnPrefix::LeafADRoute)
        return;

    const MvpnProjectManagerPartition *partition =
        GetProjectManagerPartition(rt);
    if (!partition)
        return;

    MvpnState::SG sg(mvpn_rt);
    const MvpnState *state = partition->GetState(sg);
    if (!state || !state->spmsi_rt())
        return;
    BgpTable *table = dynamic_cast<BgpTable *>(
        state->spmsi_rt()->get_table_partition()->parent());
    assert(table);
    secondary_tables->insert(table);
}

BgpRoute *MvpnTable::RouteReplicate(BgpServer *server, BgpTable *table,
    BgpRoute *rt, const BgpPath *src_path, ExtCommunityPtr community) {
    CHECK_CONCURRENCY("db::DBTable");
    MvpnRoute *src_rt = dynamic_cast<MvpnRoute *>(rt);
    MvpnTable *src_table = dynamic_cast<MvpnTable *>(table);

    if (src_rt->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute) {
        return ReplicateType7SourceTreeJoin(server, src_table, src_rt,
                                            src_path, community);
    }

    if (src_rt->GetPrefix().type() == MvpnPrefix::LeafADRoute) {
        return ReplicateType4LeafAD(server, src_table, src_rt, src_path,
                                    community);
    }

    return ReplicatePath(server, src_rt->GetPrefix(), src_table, src_rt,
                         src_path, community);
}

BgpRoute *MvpnTable::ReplicateType7SourceTreeJoin(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {

    if (src_table->IsMaster()) {
        return ReplicatePath(server, src_rt->GetPrefix(), src_table, src_rt,
                             src_path, community);
    }

    const BgpAttr *attr = src_path->GetAttr();
    if (!attr)
        return NULL;

    // If source is resolved, only then replicate the path, not otherwise.
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

    ExtCommunityPtr comm =
        server->extcomm_db()->RemoveSourceASAndLocate(attr->ext_community());
    BgpAttrPtr new_attr = server->attr_db()->ReplaceExtCommunityAndLocate(
        attr, comm);

    // Replicate path using <C-S,G>, source_rd and mvpn neighbror ASN as part
    // if the Type-7 prefix.
    MvpnPrefix prefix(MvpnPrefix::SourceTreeJoinRoute, attr->source_rd(),
                      source_as.GetAsn(), src_rt->GetPrefix().group(),
                      src_rt->GetPrefix().source());
    return ReplicatePath(server, prefix, src_table, src_rt, src_path,
                         community, new_attr);
}

// Check if GlobalErmVpnTreeRoute is present. If so, only then can we replicate
// this path for advertisement to ingress routers with associated PMSI tunnel
// information.
BgpRoute *MvpnTable::ReplicateType4LeafAD(BgpServer *server,
    MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
    ExtCommunityPtr community) {
    const BgpAttr *attr = src_path->GetAttr();

    // Do not replicate into non-master tables if the [only] route-target of the
    // route does not match the auto-created vrf-import route target of 'this'.
    if (!attr || !attr->ext_community())
        return NULL;

    if (!IsMaster()) {
        // Make sure that there is an associated Type3 S-PMSI route.
        MvpnRoute *spmsi_rt = FindSPMSIRoute(src_rt);
        if (!spmsi_rt)
            return NULL;

        if (src_table->IsMaster()) {
            return ReplicatePath(server, src_rt->GetPrefix(), src_table,
                                 src_rt, src_path, community);
        }
    }

    MvpnProjectManagerPartition *partition = GetProjectManagerPartition(src_rt);
    if (!partition)
        return NULL;
    MvpnState *mvpn_state = partition->GetState(src_rt);
    assert(mvpn_state);

    // Do not replicate if there is no receiver interested for this <S,G>.
    if (mvpn_state->cjoin_routes_received()->empty())
        return NULL;

    uint32_t label;
    Ip4Address address;
    if (!partition->GetLeafAdTunnelInfo(mvpn_state->global_ermvpn_tree_rt(),
                                        &label, &address)) {
        // TODO(Ananth) old forest node must be updated to reset input tunnel
        // attribute, if encoded.
        return NULL;
    }
    PmsiTunnelSpec *pmsi_spec = new PmsiTunnelSpec();
    pmsi_spec->tunnel_flags = 0;
    pmsi_spec->tunnel_type = PmsiTunnelSpec::IngressReplication;
    pmsi_spec->SetLabel(label);
    pmsi_spec->SetIdentifier(address);

    // Replicate the LeafAD path with appropriate PMSI tunnel info as part of
    // the path attributes. Community should be route-target with root PE
    // router-id + 0 (Page 254).
    BgpAttrPtr new_attr = server->attr_db()->ReplacePmsiTunnelAndLocate(
        src_path->GetAttr(), pmsi_spec);
    bool replicated;

    BgpRoute *replicated_path = ReplicatePath(server, src_rt->GetPrefix(),
            src_table, src_rt, src_path, community, new_attr, &replicated);

    if (replicated) {
        // Notify GlobalErmVpnTreeRoute forest node so that its input tunnel
        // attributes can be updated with the MvpnNeighbor information of the
        // S-PMSI route associated with this LeadAD route.
        mvpn_state->global_ermvpn_tree_rt()->Notify();
    }

    return replicated_path;
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

const IpAddress MvpnTable::GetAddressToResolve(BgpRoute *route,
        const BgpPath *path) const {
    MvpnRoute *mvpn_rt = dynamic_cast<MvpnRoute *>(route);
    assert(mvpn_rt->GetPrefix().type() == MvpnPrefix::SourceTreeJoinRoute);
    return mvpn_rt->GetPrefix().sourceIpAddress();
}

static void RegisterFactory() {
    DB::RegisterFactory("mvpn.0", &MvpnTable::CreateTable);
}

MODULE_INITIALIZER(RegisterFactory);
