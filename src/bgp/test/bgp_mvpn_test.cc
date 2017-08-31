/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "base/task_annotations.h"
#include "base/test/task_test_util.h"
#include "bgp/bgp_attr.h"
#include "bgp/bgp_config.h"
#include "bgp/ipeer.h"
#include "bgp/bgp_mvpn.h"
#include "bgp/bgp_server.h"
#include "bgp/mvpn/mvpn_table.h"
#include "bgp/routing-instance/rtarget_group_mgr.h"
#include "bgp/test/bgp_server_test_util.h"
#include "control-node/control_node.h"

using boost::scoped_ptr;
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
    BgpMvpnTest() : server_(&evm_) {
    }

    virtual void SetUp() {
        ConcurrencyScope scope("bgp::Config");
        boost::system::error_code err;
        server_.set_bgp_identifier(Ip4Address::from_string("127.0.0.1", err));
        master_cfg_.reset(BgpTestUtil::CreateBgpInstanceConfig(
            BgpConfigManager::kMasterInstance));
        red_cfg_.reset(BgpTestUtil::CreateBgpInstanceConfig("red",
                "target:127.0.0.1:1", "target:127.0.0.1:1"));
        blue_cfg_.reset(BgpTestUtil::CreateBgpInstanceConfig("blue",
                "target:127.0.0.1:2", "target:127.0.0.1:2"));

        // Green imports routes from both red and blue RIs.
        green_cfg_.reset(BgpTestUtil::CreateBgpInstanceConfig("green",
                "target:127.0.0.1:3,target:127.0.0.1:1,target:127.0.0.1:2",
                "target:127.0.0.1:3"));

        TaskScheduler *scheduler = TaskScheduler::GetInstance();
        scheduler->Stop();
        server_.routing_instance_mgr()->CreateRoutingInstance(
                master_cfg_.get());
        server_.rtarget_group_mgr()->Initialize();
        server_.routing_instance_mgr()->CreateRoutingInstance(red_cfg_.get());
        server_.routing_instance_mgr()->CreateRoutingInstance(blue_cfg_.get());
        server_.routing_instance_mgr()->CreateRoutingInstance(green_cfg_.get());
        scheduler->Start();

        master_ = static_cast<BgpTable *>(
            server_.database()->FindTable("bgp.mvpn.0"));
        red_ = static_cast<MvpnTable *>(
            server_.database()->FindTable("red.mvpn.0"));
        blue_ = static_cast<MvpnTable *>(
            server_.database()->FindTable("blue.mvpn.0"));
        green_ = static_cast<MvpnTable *>(
            server_.database()->FindTable("green.mvpn.0"));
    }

    void TearDown() {
        server_.Shutdown();
        task_util::WaitForIdle();
        evm_.Shutdown();
        task_util::WaitForIdle();
    }

    void DeleteMvpnRoute(BgpTable *table, const string &prefix_str);
    void AddMvpnRoute(BgpTable *table, const string &prefix_str,
                      const string &target);

    EventManager evm_;
    BgpServer server_;
    DB db_;
    BgpTable *master_;
    MvpnTable *red_;
    MvpnTable *blue_;
    MvpnTable *green_;
    scoped_ptr<BgpInstanceConfig> red_cfg_;
    scoped_ptr<BgpInstanceConfig> blue_cfg_;
    scoped_ptr<BgpInstanceConfig> green_cfg_;
    scoped_ptr<BgpInstanceConfig> master_cfg_;
};

// Ensure that Type1 and Type2 AD routes are created inside the mvpn table.
TEST_F(BgpMvpnTest, Type1_Type2ADLocal) {
    TASK_UTIL_EXPECT_EQ(3, master_->Size());
    TASK_UTIL_EXPECT_EQ(1, red_->Size());
    TASK_UTIL_EXPECT_NE(static_cast<MvpnRoute *>(NULL),
                        red_->FindType1ADRoute());

    TASK_UTIL_EXPECT_EQ(1, blue_->Size());
    TASK_UTIL_EXPECT_NE(static_cast<MvpnRoute *>(NULL),
                        blue_->FindType1ADRoute());

    TASK_UTIL_EXPECT_EQ(3, green_->Size()); // 1 green + 1 red + 1 blue
    TASK_UTIL_EXPECT_NE(static_cast<MvpnRoute *>(NULL),
                        green_->FindType1ADRoute());

    // Verify that only green has discovered a neighbor from red.
    TASK_UTIL_EXPECT_EQ(0, red_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(0, blue_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(2, green_->manager()->neighbors().size());

    MvpnNeighbor neighbor;
    boost::system::error_code err;
    EXPECT_TRUE(green_->manager()->FindNeighbor(&neighbor,
        IpAddress::from_string("127.0.0.1", err),
        red_->routing_instance()->index(), true));
    EXPECT_EQ("127.0.0.1", neighbor.address().to_string());
    EXPECT_EQ(0, neighbor.asn());
    EXPECT_EQ(red_->routing_instance()->index(), neighbor.vrf_id());
    EXPECT_EQ(false, neighbor.external());

    EXPECT_TRUE(green_->manager()->FindNeighbor(&neighbor,
        IpAddress::from_string("127.0.0.1", err),
        blue_->routing_instance()->index(), true));
    EXPECT_EQ("127.0.0.1", neighbor.address().to_string());
    EXPECT_EQ(0, neighbor.asn());
    EXPECT_EQ(blue_->routing_instance()->index(), neighbor.vrf_id());
    EXPECT_EQ(false, neighbor.external());

    // Verify that non-exact also works just based on the ip address.
    EXPECT_TRUE(green_->manager()->FindNeighbor(&neighbor,
        IpAddress::from_string("127.0.0.1", err), 0, false));
    EXPECT_EQ("127.0.0.1", neighbor.address().to_string());
    EXPECT_EQ(0, neighbor.asn());
    if (red_->routing_instance()->index() < blue_->routing_instance()->index())
        EXPECT_EQ(red_->routing_instance()->index(), neighbor.vrf_id());
    else
        EXPECT_EQ(blue_->routing_instance()->index(), neighbor.vrf_id());
    EXPECT_EQ(false, neighbor.external());
}

void BgpMvpnTest::AddMvpnRoute(BgpTable *table, const string &prefix_str,
                               const string &target) {
    MvpnPrefix prefix(MvpnPrefix::FromString(prefix_str));
    DBRequest add_req;
    add_req.key.reset(new MvpnTable::RequestKey(prefix, NULL));

    BgpAttrSpec attr_spec;
    ExtCommunitySpec *commspec(new ExtCommunitySpec());
    RouteTarget tgt = RouteTarget::FromString(target);
    commspec->communities.push_back(tgt.GetExtCommunityValue());
    attr_spec.push_back(commspec);

    BgpAttrPtr attr = server_.attr_db()->Locate(attr_spec);
    STLDeleteValues(&attr_spec);
    add_req.data.reset(new MvpnTable::RequestData(attr, 0, 20));
    add_req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
    table->Enqueue(&add_req);
    task_util::WaitForIdle();
}

void BgpMvpnTest::DeleteMvpnRoute(BgpTable *table, const string &prefix_str) {
    DBRequest delete_req;
    MvpnPrefix prefix(MvpnPrefix::FromString(prefix_str));
    delete_req.key.reset(new MvpnTable::RequestKey(prefix, NULL));
    delete_req.oper = DBRequest::DB_ENTRY_DELETE;
    table->Enqueue(&delete_req);
}

// Add Type1AD route from a mock bgp peer into bgp.mvpn.0 table.
TEST_F(BgpMvpnTest, Type1AD_Remote) {
    // Verify that only green has discovered a neighbor from red.
    TASK_UTIL_EXPECT_EQ(0, red_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(0, blue_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(2, green_->manager()->neighbors().size());

    // Inject a Type1 route from a mock peer into bgp.mvpn.0 table with red
    // route-target.
    string prefix = "1-10.1.1.1:65535,9.8.7.6";
    AddMvpnRoute(master_, prefix, "target:127.0.0.1:1");

    TASK_UTIL_EXPECT_EQ(4, master_->Size()); // 3 local + 1 remote
    TASK_UTIL_EXPECT_EQ(2, red_->Size()); // 1 local + 1 remote(red)
    TASK_UTIL_EXPECT_EQ(1, blue_->Size()); // 1 local
    TASK_UTIL_EXPECT_EQ(4, green_->Size()); // 1 local + 1 remote(red)

    // Verify that neighbor is detected.
    TASK_UTIL_EXPECT_EQ(1, red_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(0, blue_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(3, green_->manager()->neighbors().size());

    MvpnNeighbor neighbor;
    boost::system::error_code err;

    EXPECT_TRUE(red_->manager()->FindNeighbor(&neighbor,
                IpAddress::from_string("10.1.1.1", err), 0, false));
    EXPECT_EQ("10.1.1.1", neighbor.address().to_string());
    EXPECT_EQ(0, neighbor.asn());
    EXPECT_EQ(65535, neighbor.vrf_id());
    EXPECT_EQ(false, neighbor.external());

    EXPECT_TRUE(green_->manager()->FindNeighbor(&neighbor,
                IpAddress::from_string("10.1.1.1", err), 0, false));
    EXPECT_EQ("10.1.1.1", neighbor.address().to_string());
    EXPECT_EQ(0, neighbor.asn());
    EXPECT_EQ(65535, neighbor.vrf_id());
    EXPECT_EQ(false, neighbor.external());

    // Do an exact match as well, as we know the vrf-id passed in the route.
    EXPECT_TRUE(red_->manager()->FindNeighbor(&neighbor,
                IpAddress::from_string("10.1.1.1", err), 0, false));
    EXPECT_EQ("10.1.1.1", neighbor.address().to_string());
    EXPECT_EQ(0, neighbor.asn());
    EXPECT_EQ(65535, neighbor.vrf_id());
    EXPECT_EQ(false, neighbor.external());

    EXPECT_TRUE(green_->manager()->FindNeighbor(&neighbor,
                IpAddress::from_string("10.1.1.1", err), 0, false));
    EXPECT_EQ("10.1.1.1", neighbor.address().to_string());
    EXPECT_EQ(0, neighbor.asn());
    EXPECT_EQ(65535, neighbor.vrf_id());
    EXPECT_EQ(false, neighbor.external());

    DeleteMvpnRoute(master_, prefix);

    // Verify that neighbor is deleted.
    TASK_UTIL_EXPECT_EQ(3, master_->Size()); // 3 local
    TASK_UTIL_EXPECT_EQ(1, red_->Size()); // 1 local
    TASK_UTIL_EXPECT_EQ(1, blue_->Size()); // 1 local
    TASK_UTIL_EXPECT_EQ(3, green_->Size()); // 1 local + 1 red + 1 blue
    TASK_UTIL_EXPECT_EQ(0, red_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(0, blue_->manager()->neighbors().size());
    TASK_UTIL_EXPECT_EQ(2, green_->manager()->neighbors().size());
}

// Add Type7 Join route and verify that it gets sent out to the remote PE with
// the right set of path attributes.
TEST_F(BgpMvpnTest, Type7_Join) {
}

// Add Type3 S-PMSI route and verify that Type4 Leaf-AD gets originated woth the
// right set of path attributes.
TEST_F(BgpMvpnTest, Type3_SPMSI) {
    // Inject Type3 route from a mock peer into bgp.mvpn.0 table with red route
    // target. This route should go into red and green table.
    string prefix = "3-10.1.1.1:65535,9.8.7.6,224.1.2.3,192.168.1.1";
    AddMvpnRoute(master_, prefix, "target:127.0.0.1:1");
    TASK_UTIL_EXPECT_EQ(4, master_->Size()); // 3 local + 1 remote
    TASK_UTIL_EXPECT_EQ(2, red_->Size()); // 1 local + 1 remote(red)
    TASK_UTIL_EXPECT_EQ(1, blue_->Size()); // 1 local

    // 1 local + 2 remote(red) + 1 remote(green)
    TASK_UTIL_EXPECT_EQ(4, green_->Size());

    DeleteMvpnRoute(master_, prefix);
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
