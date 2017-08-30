/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include "base/task_annotations.h"
#include "base/test/task_test_util.h"
#include "bgp/bgp_config.h"
#include "bgp/ipeer.h"
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
        master_cfg_.reset(BgpTestUtil::CreateBgpInstanceConfig(
            BgpConfigManager::kMasterInstance));
        red_cfg_.reset(BgpTestUtil::CreateBgpInstanceConfig("red",
                "target:1:2", "target:1:2"));

        TaskScheduler *scheduler = TaskScheduler::GetInstance();
        scheduler->Stop();
        server_.routing_instance_mgr()->CreateRoutingInstance(
                master_cfg_.get());
        server_.rtarget_group_mgr()->Initialize();
        server_.routing_instance_mgr()->CreateRoutingInstance(red_cfg_.get());
        scheduler->Start();

        vpn_ = static_cast<BgpTable *>(
            server_.database()->FindTable("bgp.l3vpn.0"));
        red_ = static_cast<MvpnTable *>(
            server_.database()->FindTable("red.mvpn.0"));
    }

    void TearDown() {
        server_.Shutdown();
        task_util::WaitForIdle();
        evm_.Shutdown();
        task_util::WaitForIdle();
    }

    EventManager evm_;
    BgpServer server_;
    DB db_;
    BgpTable *vpn_;
    MvpnTable *red_;
    scoped_ptr<BgpInstanceConfig> red_cfg_;
    scoped_ptr<BgpInstanceConfig> master_cfg_;
};

TEST_F(BgpMvpnTest, Type1ADBasic) {
    // Ensure that Type-1 and Type-2 AD routes are always created inside the
    // mvpn table.
    TASK_UTIL_EXPECT_EQ(2, red_->Size());
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
