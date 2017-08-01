/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_MvPN_MvPN_TABLE_H_
#define SRC_BGP_MvPN_MvPN_TABLE_H_

#include <string>

#include "bgp/bgp_attr.h"
#include "bgp/bgp_table.h"
#include "bgp/mvpn/mvpn_route.h"

class BgpPath
class BgpRoute;
class BgpServer;
class MvpnManager;

class MvpnTable : public BgpTable {
public:
    static const int kPartitionCount = 1;

    struct RequestKey : BgpTable::RequestKey {
        RequestKey(const MvpnPrefix &prefix, const IPeer *ipeer)
            : prefix(prefix), peer(ipeer) {
        }
        MvpnPrefix prefix;
        const IPeer *peer;
        virtual const IPeer *GetPeer() const { return peer; }
    };

    MvpnTable(DB *db, const std::string &name);

    virtual std::auto_ptr<DBEntry> AllocEntry(const DBRequestKey *key) const;
    virtual std::auto_ptr<DBEntry> AllocEntryStr(const std::string &key) const;
    void ResolvePath(BgpRoute *rt, BgpPath *path);

    virtual Address::Family family() const { return Address::MvPN; }
    bool IsMaster() const;
    virtual bool IsVpnTable() const { return IsMaster(); }

    virtual size_t Hash(const DBEntry *entry) const;
    virtual size_t Hash(const DBRequestKey *key) const;
    virtual int PartitionCount() const { return kPartitionCount; }

    virtual BgpRoute *RouteReplicate(BgpServer *server, BgpTable *src_table,
                                     BgpRoute *src_rt, const BgpPath *path,
                                     ExtCommunityPtr ptr);

    virtual bool Export(RibOut *ribout, Route *route,
                        const RibPeerSet &peerset,
                        UpdateInfoSList &info_slist);
    static DBTableBase *CreateTable(DB *db, const std::string &name);
    size_t HashFunction(const MvpnPrefix &prefix) const;

    void CreateMvpnManager();
    void DestroyMvpnManager();
    MvpnManager *GetMvpnManager();
    const MvpnManager *GetMvpnManager() const;
    virtual void set_routing_instance(RoutingInstance *rtinstance);
    bool RouteNotify(BgpServer *server, DBTablePartBase *root,
                     DBEntryBase *entry);

private:
    friend class BgpMulticastTest;

    virtual BgpRoute *TableFind(DBTablePartition *rtp,
                                const DBRequestKey *prefix);
    MvpnManager *mvpn_manager_;

    DISALLOW_COPY_AND_ASSIGN(MvpnTable);
};

#endif  // SRC_BGP_MvPN_MvPN_TABLE_H_
