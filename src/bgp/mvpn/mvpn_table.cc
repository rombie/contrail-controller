/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/mvpn/mvpn_table.h"

using std::string;

size_t MVpnTable::HashFunction(const MVpnPrefix &prefix) const {
    if ((prefix.type() == MVpnPrefix::IntraASPMSIAutoDiscoveryRoute) ||
           (prefix.type() == MVpnPrefix::LeafAutoDiscoveryRoute)) {
        uint32_t data = prefix.originator().to_ulong();
        return boost::hash_value(data);
    }
    if (prefix.type() == MVpnPrefix::InterASPMSIAutoDiscoveryRoute) {
        uint32_t data = prefix.asn();
        return boost::hash_value(data);
    }
    return boost::hash_value(prefix.group().to_ulong());
}

MVpnTable::MVpnTable(DB *db, const string &name) :
        BgpTable(db, name), mvpn_manager_(NULL) {
}

BgpRoute *MVpnTable::RouteReplicate(BgpServer *server, BgpTable *src_table,
    BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    if (mvpn_manager_) {
        mvpn_manager_->RouteReplicate(server, src_table, source_rt, src_path,
                                      community);
    }
}

void MVpnTable::ResolvePath(BgpRoute *rt, BgpPath *path) {
    if (mvpn_manager_)
        mvpn_manager_->ResolvePath(rt, path);
}

void MVpnTable::CreateMVpnManager() {
    // Don't create the MVpnManager for the VPN table.
    if (IsMaster())
        return;
    assert(!mvpn_manager_);
    mvpn_manager_ = BgpObjectFactory::Create<MVpnManager>(this);
}

void MVpnTable::DestroyMVpnManager() {
    assert(mvpn_manager_);
    delete mvpn_manager_;
    mvpn_manager_ = NULL;
}

MVpnManager *MVpnTable::GetMVpnManager() {
    return mvpn_manager_;
}

const MVpnManager *MVpnTable::GetMVpnManager() const {
    return mvpn_manager_;
}

void MVpnTable::set_routing_instance(RoutingInstance *rtinstance) {
    BgpTable::set_routing_instance(rtinstance);
    CreateMVpnManager();
}

bool MVpnTable::IsMaster() const {
    return routing_instance()->IsMasterRoutingInstance();
}

static void RegisterFactory() {
    DB::RegisterFactory("mvpn.0", &MVpnTable::CreateTable);
}

MODULE_INITIALIZER(RegisterFactory);
