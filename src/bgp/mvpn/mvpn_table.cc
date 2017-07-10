/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/mvpn/mvpn_table.h"

MvpnTable::MvpnTable(DB *db, const string &name) :
    BgpTable(db, name), resolver_(new PathResolver(this, true)) {
}

bool MvpnTable::BgpRouteNotify(BgpServer *server, DBTablePartBase *root,
                               DBEntryBase *entry) {
    BgpTable *table = static_cast<BgpTable *>(root->parent());
    BgpRoute *route = static_cast<BgpRoute *> (entry);

    // Trigger RPF check if necessary.
    BgpPath *path = FindType7Path(); // Can we have exthop as C-S ??
    BgpTable *inet_table = table->instance()->GetTable(Address::Inet);
    resolver_->StartPathResolution(root->index(), path, route, inet_table);
}

void MvpnTable::ProcessResolvedRoutes(BgpTable *src_table, BgpRoute *source_rt,
        const BgpPath *src_path, ExtCommunityPtr community) {
    // In case of bgp.mvpn.0 table, is src_rt is type-7 <C-S,G> route and
    // route is now resolvable (or otherwise), replicate or delete C-S,G route
    // to advertise/withdraw from the ingress root PE node.
    const BgpRoute *resolved_rt = NULL;
    if (src_table->resolver()->rnexthop())
        resolved_rt = src_table->resolver()->rnexthop()->route();
    if (!resolved_rt)
        return;
    const BgpPath *path = src_table->resolver()->FindResolvedPath(source_rt,
                                                                  src_path);
    if (!path)
        return;
    const BgpAttr *attr = path->GetAttr();
    const ExtCommunity *ext_community = attr->ext_community();
    if (!ext_community)
        return;

    bool rt_import_found = false;
    // Use rt-import from the resolved path as export route-target.
    BOOST_FOREACH(const ExtCommunity::ExtCommunityValue &comm,
                  ext_community->communities()) {
        if (ExtCommunity::is_route_import_target(comm)) {
            rt_import_found = true;
            break;
        }
    }

    if (!rt_import_found)
        return;

    // Replicate path using rd of the resolved path as part of the prefix
    // and append ASN and C-S,G also to the prefix.
    RouteDistinguisher rd = resolved_rt->GetRouteDistinguisher();
}

BgpRoute *MvpnTable::RouteReplicate(BgpServer *server, BgpTable *src_table,
    BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    ProcessResolvedRoutes(src_table, source_rt, src_path, community);
}
