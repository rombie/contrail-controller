/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/mvpn/mvpn_table.h"

using std::string;

size_t MvpnTable::HashFunction(const MvpnPrefix &prefix) const {
    if ((prefix.type() == MvpnPrefix::IntraASPMSIAutoDiscoveryRoute) ||
           (prefix.type() == MvpnPrefix::LeafAutoDiscoveryRoute)) {
        uint32_t data = prefix.originator().to_ulong();
        return boost::hash_value(data);
    }
    if (prefix.type() == MvpnPrefix::InterASPMSIAutoDiscoveryRoute) {
        uint32_t data = prefix.asn();
        return boost::hash_value(data);
    }
    return boost::hash_value(prefix.group().to_ulong());
}

MvpnTable::MvpnTable(DB *db, const string &name) :
        BgpTable(db, name), mvpn_manager_(NULL) {
}

BgpRoute *MvpnTable::RouteReplicate(BgpServer *server, BgpTable *src_table,
    BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr community) {
    if (mvpn_manager_) {
        mvpn_manager_->RouteReplicate(server, src_table, source_rt, src_path,
                                      community);
    }
}

void MvpnTable::ResolvePath(BgpRoute *rt, BgpPath *path) {
    if (mvpn_manager_)
        mvpn_manager_->ResolvePath(rt, path);
}

void MvpnTable::CreateMvpnManager() {
    // Don't create the MvpnManager for the VPN table.
    if (IsMaster())
        return;
    assert(!mvpn_manager_);
    mvpn_manager_ = BgpObjectFactory::Create<MvpnManager>(this);
}

void MvpnTable::DestroyMvpnManager() {
    assert(mvpn_manager_);
    delete mvpn_manager_;
    mvpn_manager_ = NULL;
}

MvpnManager *MvpnTable::GetMvpnManager() {
    return mvpn_manager_;
}

const MvpnManager *MvpnTable::GetMvpnManager() const {
    return mvpn_manager_;
}

void MvpnTable::set_routing_instance(RoutingInstance *rtinstance) {
    BgpTable::set_routing_instance(rtinstance);
    CreateMvpnManager();
}

bool MvpnTable::IsMaster() const {
    return routing_instance()->IsMasterRoutingInstance();
}

static void RegisterFactory() {
    DB::RegisterFactory("mvpn.0", &MvpnTable::CreateTable);
}

MODULE_INITIALIZER(RegisterFactory);
