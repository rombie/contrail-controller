/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_MVPN_MVPN_TABLE_H_
#define SRC_BGP_MVPN_MVPN_TABLE_H_

#include <string>

#include "bgp/bgp_attr.h"
#include "bgp/bgp_table.h"
#include "bgp/mvpn/mvpn_route.h"

class BgpPath;
class BgpRoute;
class BgpServer;
class MvpnManager;
class MvpnProjectManager;
class MvpnProjectManagerPartition;

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
    void CreateManager();
    void DestroyManager();
    virtual Address::Family family() const { return Address::MVPN; }
    bool IsMaster() const;
    virtual bool IsVpnTable() const { return IsMaster(); }

    virtual size_t Hash(const DBEntry *entry) const;
    virtual size_t Hash(const DBRequestKey *key) const;
    virtual int PartitionCount() const { return kPartitionCount; }
    MvpnRoute *FindSPMSIRoute(MvpnRoute *leaf_ad_rt) { return NULL; }
    const ExtCommunity::ExtCommunityValue GetAutoVrfImportRouteTarget() const {
        return ExtCommunity::ExtCommunityValue();
    }

    virtual BgpRoute *RouteReplicate(BgpServer *server, BgpTable *src_table,
                                     BgpRoute *src_rt, const BgpPath *path,
                                     ExtCommunityPtr ptr);
    virtual bool Export(RibOut *ribout, Route *route,
                        const RibPeerSet &peerset,
                        UpdateInfoSList &info_slist);
    static DBTableBase *CreateTable(DB *db, const std::string &name);
    size_t HashFunction(const MvpnPrefix &prefix) const;
    PathResolver *CreatePathResolver();
    const MvpnManager *manager() const { return manager_; }
    MvpnManager *manager() { return manager_; }

    virtual void set_routing_instance(RoutingInstance *rtinstance);
    bool RouteNotify(BgpServer *server, DBTablePartBase *root, DBEntryBase *e);
    RouteDistinguisher GetSourceRouteDistinguisher(const BgpPath *path) const;
    const MvpnProjectManager *GetProjectManager() const;
    MvpnProjectManager *GetProjectManager();
    const MvpnProjectManagerPartition *GetProjectManagerPartition(
            BgpRoute *route) const;
    MvpnProjectManagerPartition *GetProjectManagerPartition(BgpRoute *rt);
    void UpdateSecondaryTablesForReplication(BgpRoute *rt,
        TableSet *secondary_tables);
    const IpAddress GetAddressToResolve(BgpRoute *route, const BgpPath *path)
            const;
    const RouteTarget::List &GetExportList(BgpRoute *rt) const;
    MvpnPrefix CreateType4LeafADRoutePrefix(const MvpnRoute *type3_rt);
    MvpnPrefix CreateType3SPMSIRoutePrefix(MvpnRoute *type7_rt);
    MvpnPrefix CreateType2ADRoutePrefix();
    MvpnPrefix CreateType1ADRoutePrefix();
    MvpnRoute *LocateType1ADRoute();
    MvpnRoute *LocateType2ADRoute();
    MvpnRoute *LocateType3SPMSIRoute(MvpnRoute *type7_join_rt);
    MvpnRoute *LocateType4LeafADRoute(const MvpnRoute *type3_spmsi_rt);

private:
    friend class BgpMulticastTest;

    virtual BgpRoute *TableFind(DBTablePartition *rtp,
                                const DBRequestKey *prefix);
    MvpnRoute *LocateRoute(MvpnPrefix &prefix);

    BgpRoute *ReplicateType7SourceTreeJoin(BgpServer *server,
        MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
        ExtCommunityPtr comm);
    BgpRoute *ReplicateType4LeafAD(BgpServer *server, MvpnTable *src_table,
                                   MvpnRoute *src_rt, const BgpPath *src_path,
                                   ExtCommunityPtr community);
    BgpRoute *ReplicatePath(BgpServer *server, const MvpnPrefix &prefix,
        MvpnTable *src_table, MvpnRoute *src_rt, const BgpPath *src_path,
        ExtCommunityPtr comm, BgpAttrPtr new_attr = NULL,
        bool *replicated = NULL);
    UpdateInfo *GetMvpnUpdateInfo(RibOut *ribout, MvpnRoute *route,
                                  const RibPeerSet &peerset);

    MvpnManager *manager_;

    DISALLOW_COPY_AND_ASSIGN(MvpnTable);
};

#endif  // SRC_BGP_MVPN_MVPN_TABLE_H_
