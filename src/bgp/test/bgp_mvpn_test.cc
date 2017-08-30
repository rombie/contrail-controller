/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "base/test/task_test_util.h"
#include "bgp/bgp_config.h"
#include "bgp/bgp_log.h"
#include "bgp/bgp_server.h"
#include "bgp/extended-community/etree.h"
#include "bgp/extended-community/mac_mobility.h"
#include "bgp/inet/inet_route.h"
#include "bgp/inet/inet_table.h"
#include "bgp/origin-vn/origin_vn.h"
#include "control-node/control_node.h"
#include "net/community_type.h"

using std::string;

class PeerMock : public IPeer {
public:
    PeerMock()
        : peer_type_(BgpProto::IBGP),
          address_(Ip4Address(0)) {
    }
    PeerMock(BgpProto::BgpPeerType peer_type, Ip4Address address)
        : peer_type_(peer_type),
          address_(address),
          address_str_("Peer_" + address.to_string()) {
    }

    virtual const string &ToString() const { return address_str_; }
    virtual const string &ToUVEKey() const { return address_str_; }
    virtual bool SendUpdate(const uint8_t *msg, size_t msgsize) { return true; }
    virtual BgpServer *server() { return NULL; }
    virtual BgpServer *server() const { return NULL; }
    virtual IPeerClose *peer_close() { return NULL; }
    virtual IPeerClose *peer_close() const { return NULL; }
    virtual void UpdateCloseRouteStats(Address::Family family,
        const BgpPath *old_path, uint32_t path_flags) const {
    }
    virtual IPeerDebugStats *peer_stats() { return NULL; }
    virtual const IPeerDebugStats *peer_stats() const { return NULL; }
    virtual bool IsReady() const { return true; }
    virtual bool IsXmppPeer() const { return peer_type_ == BgpProto::XMPP; }
    virtual void Close(bool graceful) { }
    virtual const string GetStateName() const { return "Established"; }
    BgpProto::BgpPeerType PeerType() const { return peer_type_; }
    virtual uint32_t bgp_identifier() const { return address_.to_ulong(); }
    virtual void UpdateTotalPathCount(int count) const { }
    virtual int GetTotalPathCount() const { return 0; }
    virtual void UpdatePrimaryPathCount(int count,
        Address::Family family) const { }
    virtual int GetPrimaryPathCount() const { return 0; }
    virtual bool IsRegistrationRequired() const { return true; }
    virtual void MembershipRequestCallback(BgpTable *table) { }
    virtual bool MembershipPathCallback(DBTablePartBase *tpart,
        BgpRoute *route, BgpPath *path) { return false; }
    virtual bool CanUseMembershipManager() const { return true; }
    virtual bool IsInGRTimerWaitState() const { return false; }

private:
    BgpProto::BgpPeerType peer_type_;
    Ip4Address address_;
    std::string address_str_;
};

class BgpMvpnTest : public ::testing::Test {
protected:
    BgpMvpnTest() : server_(&evm_), table_(&db_, "test.mvpn.0") {
    }

    void TearDown() {
        server_.Shutdown();
        task_util::WaitForIdle();
    }

    EventManager evm_;
    BgpServer server_;
    DB db_;
    MvTable table_;
};

TEST_F(BgpMvpnTest, Paths) {
    BgpAttrSpec spec;
    BgpAttrDB *db = server_.attr_db();
    BgpAttr *attr = new BgpAttr(db, spec);

    attr->set_origin(BgpAttrOrigin::IGP);
    attr->set_med(5);
    attr->set_local_pref(10);

    AsPathSpec as_path;
    AsPathSpec::PathSegment *ps = new AsPathSpec::PathSegment;
    ps->path_segment_type = AsPathSpec::PathSegment::AS_SEQUENCE;
    ps->path_segment.push_back(20);
    ps->path_segment.push_back(30);
    as_path.path_segments.push_back(ps);

    attr->set_as_path(&as_path);


    PeerMock peer;
    BgpPath *path = new BgpPath(&peer, BgpPath::BGP_XMPP, attr, 0, 0);

    Ip4Prefix prefix;
    InetRoute route(prefix);
    route.InsertPath(path);

    BgpAttr *attr2 = new BgpAttr(*attr);
    attr2->set_med(4);
    BgpPath *path2 = new BgpPath(&peer, BgpPath::BGP_XMPP, attr2, 0, 0);
    route.InsertPath(path2);

    EXPECT_EQ(path2, route.FindPath(BgpPath::BGP_XMPP, &peer, 0));

    BgpAttr *attr3 = new BgpAttr(*attr);
    attr3->set_local_pref(20);
    BgpPath *path3 = new BgpPath(&peer, BgpPath::BGP_XMPP, attr3, 0, 0);
    route.InsertPath(path3);

    EXPECT_EQ(path3, route.FindPath(BgpPath::BGP_XMPP, &peer, 0));

    BgpAttr *attr4 = new BgpAttr(*attr);
    attr4->set_origin(BgpAttrOrigin::EGP);
    BgpPath *path4 = new BgpPath(&peer, BgpPath::BGP_XMPP, attr4, 0, 0);
    route.InsertPath(path4);

    EXPECT_EQ(path3, route.FindPath(BgpPath::BGP_XMPP, &peer, 0));

    // Remove all 4 paths that have been added so far
    route.RemovePath(&peer);
    route.RemovePath(&peer);
    route.RemovePath(&peer);
    route.RemovePath(&peer);
}

static void SetUp() {
    bgp_log_test::init();
    ControlNode::SetDefaultSchedulingPolicy();
}

static void TearDown() {
    task_util::WaitForIdle();
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Terminate();
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    SetUp();
    int result = RUN_ALL_TESTS();
    TearDown();
    return result;
}
