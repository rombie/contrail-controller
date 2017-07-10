/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/mvpn/mvpn_table.h"

using std::string;

MvpnTable::MvpnTable(DB *db, const string &name) :
    BgpTable(db, name), resolver_(new PathResolver(this, true)) {
}

bool MvpnTable::RouteNotify(BgpServer *server, DBTablePartBase *root,
                            DBEntryBase *entry) {
    BgpTable *table = static_cast<BgpTable *>(root->parent());
    BgpRoute *route = static_cast<BgpRoute *> (entry);

    // Trigger RPF check if necessary.
    BgpPath *path = FindType7Path(); // Can we have exthop as C-S ??
    BgpTable *inet_table = table->instance()->GetTable(Address::Inet);
    resolver_->StartPathResolution(root->index(), path, route, inet_table);
}

// At the moment, we only do resolution for C-<S,G> Type-7 routes.
BgpRoute *MvpnTable::ReplicateType7SourceTreeJoin(MvpnTable *src_table,
    MvpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {

    // In case of bgp.mvpn.0 table, if src_rt is type-7 <C-S,G> route and it is
    // now resolvable (or otherwise), replicate or delete C-S,G route to
    // advertise/withdraw from the ingress root PE node.
    const BgpRoute *resolved_rt = NULL;
    if (src_table->resolver()->rnexthop())
        resolved_rt = src_table->resolver()->rnexthop()->route();

    if (!resolved_rt)
        return NULL;

    const BgpPath *path =
        src_table->resolver()->FindResolvedPath(source_rt, src_path);

    if (!path)
        return;

    const BgpAttr *attr = p->GetAttr();
    const ExtCommunity *ext_community = attr->ext_community();
    if (!ext_community)
        return NULL;

    bool rt_import_found = false;
    // Use rt-import from the resolved path as export route-target.
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                  ext_community->communities()) {
        if (ExtCommunity::is_route_import_target(comm)) {
            rt_import_found = true;
            break NULL;
        }
    }

    if (!rt_import_found)
        return NULL;

    // Replicate path using rd of the resolved path as part of the prefix
    // and append ASN and C-S,G also to the prefix.
    RouteDistinguisher rd = resolved_rt->GetRouteDistinguisher();
    return replicated_rt;
}

BgpRoute *MvpnTable::RouteReplicate(BgpServer *server, BgpTable *src_table,
    BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    MvpnRoute *source_mvpn_rt = dynamic_cast<MvpnRoute *>(source_rt);

    // Phase 1 support for only senders outside the cluster and receivers inside
    // the cluster. Hence don't replicate to a VRF from other VRF tables.
    if (!IsMaster()) {
        ErmVpnTable *src_ermvpn_table = dynamic_cast<ErmVpnTable *>(src_table);
        if (!src_ermvpn_table->IsMaster())
            return NULL;
    }

    if (source_rt->IsType7()) {
        return ReplicateType7SourceTreeJoin(
            dynamic_cast <MVpnTable *>(src_table),
            dynamic_cast<MvpnRoute *>(source_rt), src_path, community);
    }

    if (source_rt->IsType3()) {
        return mvpn_manager_->ReplicateType3SPmsi(
            dynamic_cast <MVpnTable *>(src_table),
            dynamic_cast<MvpnRoute *>(source_rt), src_path, community);
    }

    if (source_rt->IsType4()) {
        return ReplicateType4LeafAD(dynamic_cast <MVpnTable *>(src_table),
            dynamic_cast<MvpnRoute *>(source_rt), src_path, community);
    }
}
