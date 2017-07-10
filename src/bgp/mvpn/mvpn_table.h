/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_MVPN_TABLE_H_
#define SRC_BGP_MVPN_TABLE_H_

class MvpnTable : DBTable {
public:
    MvpnTable(DB *db, const std::string &name);
    BgpRoute *ReplicateType7SourceTreeJoin(MvpnTable *src_table,
        MvpnRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr comm);
    BgpRoute *ReplicateType3SPmsi(MvpnTable *src_table, MvpnRoute *source_rt,
        const BgpPath *src_path, ExtCommunityPtr community);
    void ReplicateType4LeafAD(MvpnTable *src_table, MvpnRoute *source_rt,
        const BgpPath *src_path, ExtCommunityPtr community);
    BgpRoute *RouteReplicate(BgpServer *server, BgpTable *src_table,
        BgpRoute *source_rt, const BgpPath *src_path, ExtCommunityPtr comm);

private:
};

#endif  // SRC_BGP_MVPN_TABLE_H_
