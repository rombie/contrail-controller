/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/l3vpn/inetvpn_table.h"

#include <boost/foreach.hpp>

#include "bgp/ipeer.h"
#include "bgp/bgp_server.h"
#include "bgp/bgp_update.h"
#include "bgp/inet/inet_table.h"
#include "bgp/routing-instance/routing_instance.h"
#include "bgp/extended-community/source_as.h"
#include "bgp/extended-community/vrf_route_import.h"

using std::auto_ptr;
using std::string;

InetVpnTable::InetVpnTable(DB *db, const string &name)
        : BgpTable(db, name) {
}

auto_ptr<DBEntry> InetVpnTable::AllocEntry(const DBRequestKey *key) const {
    const RequestKey *pfxkey = static_cast<const RequestKey *>(key);
    return auto_ptr<DBEntry> (new InetVpnRoute(pfxkey->prefix));
}


auto_ptr<DBEntry> InetVpnTable::AllocEntryStr(const string &key_str) const {
    InetVpnPrefix prefix = InetVpnPrefix::FromString(key_str);
    return auto_ptr<DBEntry> (new InetVpnRoute(prefix));
}

size_t InetVpnTable::Hash(const DBEntry *entry) const {
    const InetVpnRoute *rt_entry = static_cast<const InetVpnRoute *>(entry);
    const InetVpnPrefix &inetvpnprefix = rt_entry->GetPrefix();
    Ip4Prefix prefix(inetvpnprefix.addr(), inetvpnprefix.prefixlen());
    size_t value = InetTable::HashFunction(prefix);
    return value % DB::PartitionCount();
}

size_t InetVpnTable::Hash(const DBRequestKey *key) const {
    const RequestKey *rkey = static_cast<const RequestKey *>(key);
    Ip4Prefix prefix(rkey->prefix.addr(), rkey->prefix.prefixlen());
    size_t value = InetTable::HashFunction(prefix);
    return value % DB::PartitionCount();
}

BgpRoute *InetVpnTable::TableFind(DBTablePartition *rtp,
                                  const DBRequestKey *prefix) {
    const RequestKey *pfxkey = static_cast<const RequestKey *>(prefix);
    InetVpnRoute rt_key(pfxkey->prefix);
    return static_cast<BgpRoute *>(rtp->Find(&rt_key));
}

DBTableBase *InetVpnTable::CreateTable(DB *db, const string &name) {
    InetVpnTable *table = new InetVpnTable(db, name);
    table->Init();
    return table;
}

static RouteDistinguisher GenerateDistinguisher(
        const BgpTable *src_table, const BgpPath *src_path) {
    const RouteDistinguisher &source_rd = src_path->GetAttr()->source_rd();
    if (!source_rd.IsZero())
        return source_rd;

    assert(!src_path->GetPeer() || !src_path->GetPeer()->IsXmppPeer());
    const RoutingInstance *src_instance = src_table->routing_instance();
    return *src_instance->GetRD();
}

// Insert origin-vn extended-community if primary table is different
// from the targeted table. (e.g. fabric routes). Form RD using primary
// table index and associated next-hop and look up in bgp.l3vpn.0
// master table for this peer. If a path is found and is associated
// with OriginVn community, attach the same to this route as well.
BgpAttrPtr InetVpnTable::GetInetAttributes(BgpRoute *route,
                                           BgpAttrPtr inet_attrp,
                                           const IPeer *peer) {
    InetRoute *inet_route = dynamic_cast<InetRoute *>(route);
    assert(inet_route);
    if (!inet_attrp || inet_attrp->source_rd().IsZero())
        return inet_attrp;

    const Ip4Prefix &inet_prefix = inet_route->GetPrefix();
    InetTable::RequestKey inet_rt_key(inet_prefix, NULL);
    DBTablePartition *inet_partition =
        static_cast<DBTablePartition *>(GetTablePartition(&inet_rt_key));

    InetVpnPrefix inetvpn_prefix(inet_attrp->source_rd(),
                                 inet_prefix.ip4_addr(),
                                 inet_prefix.prefixlen());
    RequestKey inetvpn_rt_key(inetvpn_prefix, NULL);
    DBTablePartition *inetvpn_partition =
        static_cast<DBTablePartition *>(GetTablePartition(&inetvpn_rt_key));
    assert(inet_partition == inetvpn_partition);
    InetVpnRoute *inetvpn_route = dynamic_cast<InetVpnRoute *>(
        TableFind(inetvpn_partition, &inetvpn_rt_key));
    if (!inetvpn_route)
        return inet_attrp;
    BgpPath *inetvpn_path = inetvpn_route->FindPath(peer);
    if (!inetvpn_path)
        return inet_attrp;
    return UpdateInetAttributes(inetvpn_path->GetAttr(), inet_attrp);
}

BgpAttrPtr InetVpnTable::UpdateInetAttributes(const BgpAttrPtr inetvpn_attrp,
                                              const BgpAttrPtr inet_attrp) {
    BgpServer *server = routing_instance()->server();

    // Check if origin-vn path attribute in inet.0 table path is identical to
    // what is in inetvpn table path.
    ExtCommunity::ExtCommunityValue const *inetvpn_rt_origin_vn = NULL;
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                  inetvpn_attrp->ext_community()->communities()) {
        if (!ExtCommunity::is_origin_vn(comm))
            continue;
        inetvpn_rt_origin_vn = &comm;
        break;
    }

    ExtCommunity::ExtCommunityValue const *inet_rt_origin_vn = NULL;
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                  inet_attrp->ext_community()->communities()) {
        if (!ExtCommunity::is_origin_vn(comm))
            continue;
        inet_rt_origin_vn = &comm;
        break;
    }

    // Ignore if there is no change.
    if (inetvpn_rt_origin_vn == inet_rt_origin_vn)
        return inet_attrp;

    // Update/Delete inet route attributes with updated OriginVn community.
    ExtCommunityPtr new_ext_community;
    if (!inetvpn_rt_origin_vn) {
        new_ext_community = server->extcomm_db()->RemoveOriginVnAndLocate(
            inet_attrp->ext_community());
    } else {
        new_ext_community = server->extcomm_db()->ReplaceOriginVnAndLocate(
            inet_attrp->ext_community(), *inetvpn_rt_origin_vn);
    }

    return server->attr_db()->ReplaceExtCommunityAndLocate(inet_attrp.get(),
                                                           new_ext_community);
}

void InetVpnTable::UpdateInetRoute(BgpServer *server,
                                   InetVpnRoute *inetvpn_route,
                                   const BgpPath *inetvpn_path,
                                   BgpAttrPtr new_inetvpn_attr) {
    assert(routing_instance()->IsMasterRoutingInstance());

    // Check if a route is present in inet.0 table for this prefix.
    InetTable *inet_table =
        dynamic_cast<InetTable *>(routing_instance()->GetTable(Address::INET));
    Ip4Prefix inet_prefix(inetvpn_route->GetPrefix().addr(),
                          inetvpn_route->GetPrefix().prefixlen());
    InetTable::RequestKey inet_rt_key(inet_prefix, NULL);
    DBTablePartition *inet_partition =
        static_cast<DBTablePartition *>(GetTablePartition(&inet_rt_key));

    InetVpnRoute inetvpn_rt_key(inetvpn_route->GetPrefix());
    DBTablePartition *inetvpn_partition =
        static_cast<DBTablePartition *>(GetTablePartition(&inetvpn_rt_key));
    assert(inet_partition == inetvpn_partition);

    InetRoute *inet_route = dynamic_cast<InetRoute *>(
        inet_table->TableFind(inet_partition, &inet_rt_key));
    if (!inet_route)
        return;
    BgpPath *inet_path = inet_route->FindPath(inetvpn_path->GetPeer());
    if (!inet_path)
        return;
    BgpAttrPtr inet_attrp = inet_path->GetAttr();
    BgpAttrPtr new_inet_attrp = UpdateInetAttributes(new_inetvpn_attr,
                                                     inet_attrp);
    if (new_inet_attrp != inet_attrp) {
        inet_path->SetAttr(new_inet_attrp, inet_path->GetOriginalAttr());
        inet_route->Notify();
    }
}

BgpRoute *InetVpnTable::RouteReplicate(BgpServer *server,
        BgpTable *src_table, BgpRoute *src_rt, const BgpPath *src_path,
        ExtCommunityPtr community) {
    assert(src_table->family() == Address::INET);

    InetRoute *inet = dynamic_cast<InetRoute *> (src_rt);
    assert(inet);

    const RouteDistinguisher &rd = GenerateDistinguisher(src_table, src_path);

    InetVpnPrefix vpn(rd, inet->GetPrefix().ip4_addr(),
                      inet->GetPrefix().prefixlen());

    InetVpnRoute rt_key(vpn);

    DBTablePartition *rtp =
        static_cast<DBTablePartition *>(GetTablePartition(&rt_key));
    BgpRoute *dest_route = static_cast<BgpRoute *>(rtp->Find(&rt_key));
    if (dest_route == NULL) {
        dest_route = new InetVpnRoute(vpn);
        rtp->Add(dest_route);
    } else {
        dest_route->ClearDelete();
    }

    if (!src_table->routing_instance()->IsMasterRoutingInstance()) {
        if (server->bgp_identifier() != 0) {
            VrfRouteImport vit(server->bgp_identifier(),
                    src_table->routing_instance()->index());
            community = server->extcomm_db()->ReplaceVrfRouteImportAndLocate(
                    community.get(), vit.GetExtCommunity());
        }
        if (server->autonomous_system() != 0) {
            SourceAs sas(server->autonomous_system(), 0);
            community = server->extcomm_db()->ReplaceSourceASAndLocate(
                    community.get(), sas.GetExtCommunity());
        }
    }

    BgpAttrPtr new_attr =
        server->attr_db()->ReplaceExtCommunityAndLocate(src_path->GetAttr(),
                                                        community);

    // Check whether there's already a path with the given peer and path id.
    BgpPath *dest_path =
        dest_route->FindSecondaryPath(src_rt, src_path->GetSource(),
                                      src_path->GetPeer(),
                                      src_path->GetPathId());
    if (dest_path != NULL) {
        if ((new_attr != dest_path->GetOriginalAttr()) ||
            (src_path->GetFlags() != dest_path->GetFlags()) ||
            (src_path->GetLabel() != dest_path->GetLabel())) {
            // Update Attributes and notify (if needed)
            assert(dest_route->RemoveSecondaryPath(src_rt,
                                src_path->GetSource(), src_path->GetPeer(),
                                src_path->GetPathId()));
        } else {
            return dest_route;
        }
    }

    // Create replicated path and insert it on the route
    BgpSecondaryPath *replicated_path =
        new BgpSecondaryPath(src_path->GetPeer(), src_path->GetPathId(),
                            src_path->GetSource(), new_attr,
                            src_path->GetFlags(), src_path->GetLabel());
    replicated_path->SetReplicateInfo(src_table, src_rt);
    dest_route->InsertPath(replicated_path);

    // Always trigger notification.
    rtp->Notify(dest_route);

    // Update corresponding route's extended communities in inet.0 table.
    UpdateInetRoute(server, dynamic_cast<InetVpnRoute *>(dest_route), src_path,
                    new_attr);

    return dest_route;
}

bool InetVpnTable::Export(RibOut *ribout, Route *route,
        const RibPeerSet &peerset, UpdateInfoSList &uinfo_slist) {
    BgpRoute *bgp_route = static_cast<BgpRoute *> (route);
    UpdateInfo *uinfo = GetUpdateInfo(ribout, bgp_route, peerset);
    if (!uinfo) return false;
    uinfo_slist->push_front(*uinfo);

    return true;
}

static void RegisterFactory() {
    DB::RegisterFactory("bgp.l3vpn.0", &InetVpnTable::CreateTable);
}
MODULE_INITIALIZER(RegisterFactory);
