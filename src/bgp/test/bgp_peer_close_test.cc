/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <boost/program_options.hpp>
#include <list>

#include "base/test/addr_test_util.h"

#include "bgp/bgp_factory.h"
#include "bgp/bgp_peer_membership.h"
#include "bgp/bgp_session_manager.h"
#include "bgp/bgp_xmpp_channel.h"
#include "bgp/inet/inet_table.h"
#include "bgp/l3vpn/inetvpn_table.h"
#include "bgp/test/bgp_server_test_util.h"
#include "bgp/tunnel_encap/tunnel_encap.h"
#include "bgp/xmpp_message_builder.h"

#include "control-node/control_node.h"
#include "control-node/test/network_agent_mock.h"
#include "io/test/event_manager_test.h"

#define XMPP_CONTROL_SERV   "bgp.contrail.com"
#define PUBSUB_NODE_ADDR "bgp-node.contrail.com"

using namespace std;
using namespace boost;
using namespace boost::asio;
using namespace boost::assign;
using namespace boost::program_options;
using   boost::any_cast;
using ::testing::TestWithParam;
using ::testing::Bool;
using ::testing::ValuesIn;
using ::testing::Combine;

static vector<int>  n_instances = boost::assign::list_of(3);
static vector<int>  n_routes    = boost::assign::list_of(3);
static vector<int>  n_peers     = boost::assign::list_of(0);
static vector<int>  n_agents    = boost::assign::list_of(3);
static vector<int>  n_targets   = boost::assign::list_of(1);
static vector<bool> xmpp_close_from_control_node =
                                  boost::assign::list_of(false);
static char **gargv;
static int    gargc;
static int    n_db_walker_wait_usecs = 0;
static int    wait_for_idle = 30; // Seconds

static void process_command_line_args(int argc, char **argv) {
    static bool cmd_line_processed;

    if (cmd_line_processed) return;
    cmd_line_processed = true;

    int instances = 1, routes = 1, peers = 1, agents = 1, targets = 1;
    bool close_from_control_node = false;
    bool cmd_line_arg_set = false;

    // Declare the supported options.
    options_description desc("Allowed options");
    desc.add_options()
        ("help", "produce help message")
        ("nroutes", value<int>(), "set number of routes")
        ("npeers", value<int>(), "set number of bgp peers")
        ("nagents", value<int>(), "set number of xmpp agents")
        ("ninstances", value<int>(), "set number of routing instances")
        ("ntargets", value<int>(), "set number of route targets")
        ("db-walker-wait-usecs", value<int>(), "set usecs delay in walker cb")
        ("wait-for-idle-time", value<int>(),
             "task_util::WaitForIdle() wait time, 0 to disable")
        ("close-from-control-node", bool_switch(&close_from_control_node),
             "Initiate xmpp session close from control-node")
        ;

    variables_map vm;
    store(parse_command_line(argc, argv, desc), vm);
    notify(vm);

    if (vm.count("help")) {
        cout << desc << "\n";
        exit(1);
    }

    if (close_from_control_node) {
        cmd_line_arg_set = true;
    }

    if (vm.count("ninstances")) {
        instances = vm["ninstances"].as<int>();
        cmd_line_arg_set = true;
    }
    if (vm.count("nroutes")) {
        routes = vm["nroutes"].as<int>();
        cmd_line_arg_set = true;
    }
    if (vm.count("npeers")) {
        peers = vm["npeers"].as<int>();
        cmd_line_arg_set = true;
    }
    if (vm.count("nagents")) {
        agents = vm["nagents"].as<int>();
        cmd_line_arg_set = true;
    }
    if (vm.count("ntargets")) {
        targets = vm["ntargets"].as<int>();
        cmd_line_arg_set = true;
    }
    if (vm.count("db-walker-wait-usecs")) {
        n_db_walker_wait_usecs = vm["db-walker-wait-usecs"].as<int>();
        cmd_line_arg_set = true;
    }
    if (vm.count("wait-for-idle-time")) {
        wait_for_idle = vm["wait-for-idle-time"].as<int>();
        cmd_line_arg_set = true;
    }

    if (cmd_line_arg_set) {
        n_instances.clear();
        n_instances.push_back(instances);

        n_routes.clear();
        n_routes.push_back(routes);

        n_peers.clear();
        n_peers.push_back(peers);

        n_targets.clear();
        n_targets.push_back(targets);

        n_agents.clear();
        n_agents.push_back(agents);

        xmpp_close_from_control_node.clear();
        xmpp_close_from_control_node.push_back(close_from_control_node);
    }
}

static vector<int> GetInstanceParameters() {
    process_command_line_args(gargc, gargv);
    return n_instances;
}

static vector<int> GetAgentParameters() {
    process_command_line_args(gargc, gargv);
    return n_agents;
}

static vector<int> GetRouteParameters() {
    process_command_line_args(gargc, gargv);
    return n_routes;
}

static vector<int> GetPeerParameters() {
    process_command_line_args(gargc, gargv);
    return n_peers;
}

static vector<int> GetTargetParameters() {
    process_command_line_args(gargc, gargv);
    return n_targets;
}

static void WaitForIdle() {
    if (wait_for_idle) {
        usleep(10);
        task_util::WaitForIdle(wait_for_idle);
    }
}

class PeerCloseManagerTest : public PeerCloseManager {
public:
    explicit PeerCloseManagerTest(IPeer *peer) : PeerCloseManager(peer) { }
    ~PeerCloseManagerTest() { }

    //
    // Do not start the timer in test, as we right away call it in line from
    // within the tests
    //
    void StartStaleTimer() { }
};

class BgpNullPeer {
public:
    BgpNullPeer(BgpServerTest *server, const BgpInstanceConfig *instance_config,
                string name, RoutingInstance *rtinstance, int peer_id)
            : name_(name), peer_id_(peer_id), config_(new BgpNeighborConfig) {
        for (int i = 0; i < 20; i++) {
            ribout_creation_complete_.push_back(false);
        }

        config_->set_name(name);
        config_->set_instance_name(rtinstance->name());
        config_->set_peer_address(IpAddress::from_string("127.0.0.1"));
        config_->set_peer_as(65412);
        config_->set_port(peer_id);

        peer_ = static_cast<BgpPeerTest *>
            (rtinstance->peer_manager()->PeerLocate(server, config_.get()));
        WaitForIdle();
    }
    BgpPeerTest *peer() { return peer_; }
    int peer_id() { return peer_id_; }
    bool ribout_creation_complete(Address::Family family) {
        return ribout_creation_complete_[family];
    }
    void ribout_creation_complete(Address::Family family, bool complete) {
        ribout_creation_complete_[family] = complete;
    }

    string name_;
    int peer_id_;
    BgpPeerTest *peer_;

private:
    auto_ptr<BgpNeighborConfig> config_;
    std::vector<bool> ribout_creation_complete_;
};

class BgpXmppChannelManagerMock : public BgpXmppChannelManager {
public:
    BgpXmppChannelManagerMock(XmppServer *x, BgpServer *b) :
        BgpXmppChannelManager(x, b), channel_(NULL) { }

    virtual BgpXmppChannel *CreateChannel(XmppChannel *channel) {
        channel_ = new BgpXmppChannel(channel, bgp_server_, this);
        return channel_;
    }

    BgpXmppChannel *channel_;
};

typedef std::tr1::tuple<int, int, int, int, int, bool> TestParams;

class BgpPeerCloseTest : public ::testing::TestWithParam<TestParams> {

public:
    void CreateRibsDone(IPeer *ipeer, BgpTable *table, BgpNullPeer *npeer) {
        npeer->ribout_creation_complete(table->family(), true);
    }

    void DeleteRoutingInstance(RoutingInstance *rtinstance);
    bool IsPeerCloseGraceful(bool graceful) { return graceful; }
    void SetPeerCloseGraceful(bool graceful) {
        server_->GetIsPeerCloseGraceful_fnc_ =
                    boost::bind(&BgpPeerCloseTest::IsPeerCloseGraceful, this,
                                graceful);
        xmpp_server_->GetIsPeerCloseGraceful_fnc_ =
                    boost::bind(&BgpPeerCloseTest::IsPeerCloseGraceful, this,
                                graceful);
    }

protected:
    BgpPeerCloseTest() : thread_(&evm_) { }

    virtual void SetUp() {
        server_.reset(new BgpServerTest(&evm_, "A"));
        xmpp_server_ = new XmppServerTest(&evm_, XMPP_CONTROL_SERV);

        channel_manager_.reset(new BgpXmppChannelManagerMock(
                                       xmpp_server_, server_.get()));
        master_cfg_.reset(BgpTestUtil::CreateBgpInstanceConfig(
            BgpConfigManager::kMasterInstance, "", ""));
        master_instance_ = static_cast<RoutingInstance *>(
            server_->routing_instance_mgr()->GetRoutingInstance(
                BgpConfigManager::kMasterInstance));
        n_families_ = 2;
        familes_.push_back(Address::INET);
        familes_.push_back(Address::INETVPN);

        server_->session_manager()->Initialize(0);
        xmpp_server_->Initialize(0, false);
        thread_.Start();
    }

    void AgentCleanup() {
        BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
            agent->Delete();
        }
    }

    void Cleanup() {
        BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
            delete npeer;
        }
        WaitForIdle();
    }

    virtual void TearDown() {
        WaitForIdle();
        SetPeerCloseGraceful(false);
        XmppPeerClose();
        VerifyRoutes(0);
        VerifyReceivedXmppRoutes(0);

        xmpp_server_->Shutdown();
        WaitForIdle();
        if (n_agents_) {
            TASK_UTIL_EXPECT_EQ(0, xmpp_server_->ConnectionCount());
        }
        AgentCleanup();
        channel_manager_.reset();
        WaitForIdle();

        TcpServerManager::DeleteServer(xmpp_server_);
        xmpp_server_ = NULL;
        server_->Shutdown();
        WaitForIdle();
        Cleanup();
        evm_.Shutdown();
        thread_.Join();
        task_util::WaitForIdle();

        BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
            delete agent;
        }
    }

    void Configure() {
        server_->Configure(GetConfig().c_str());
        WaitForIdle();
        VerifyRoutingInstances();
    }

    XmppChannelConfig *CreateXmppChannelCfg(const char *address, int port,
                                            const string &from,
                                            const string &to,
                                            bool isClient) {
        XmppChannelConfig *cfg = new XmppChannelConfig(isClient);
        boost::system::error_code ec;
        cfg->endpoint.address(ip::address::from_string(address, ec));
        cfg->endpoint.port(port);
        cfg->ToAddr = to;
        cfg->FromAddr = from;
        if (!isClient) cfg->NodeAddr = PUBSUB_NODE_ADDR;
        return cfg;
    }

    void GracefulRestartTestStart();
    void GracefulRestartTestRun();

    string GetConfig();
    BgpAttr *CreatePathAttr();
    void AddRoutes(BgpTable *table, BgpNullPeer *npeer);
    ExtCommunitySpec *CreateRouteTargets();
    void AddAllRoutes();
    void AddPeersWithRoutes(const BgpInstanceConfig *instance_config);
    void AddXmppPeersWithRoutes();
    void CreateAgents();
    void Subscribe();
    void UnSubscribe();
    void AddOrDeleteXmppRoutes(bool add, int nroutes = -1,
                               int down_agents = -1);
    void VerifyReceivedXmppRoutes(int routes);
    void DeleteAllRoutingInstances();
    void VerifyRoutingInstances();
    void XmppPeerClose(int npeers = -1);
    void CallStaleTimer(bool);
    void InitParams();
    void VerifyPeer(BgpServerTest *server, RoutingInstance *rtinstance,
                    BgpNullPeer *npeer, BgpPeerTest *peer);
    void VerifyPeers();
    void VerifyNoPeers();
    void VerifyRoutes(int count);
    void VerifyRibOutCreationCompletion();
    bool SendUpdate(BgpPeerTest *peer, const uint8_t *msg, size_t msgsize);
    bool IsReady(bool ready);
    bool MpNlriAllowed(BgpPeerTest *peer, uint16_t afi, uint8_t safi);

    EventManager evm_;
    ServerThread thread_;
    boost::scoped_ptr<BgpServerTest> server_;
    XmppServerTest *xmpp_server_;
    boost::scoped_ptr<BgpXmppChannelManagerMock> channel_manager_;
    scoped_ptr<BgpInstanceConfigTest> master_cfg_;
    RoutingInstance *master_instance_;
    std::vector<BgpNullPeer *> peers_;
    std::vector<test::NetworkAgentMock *> xmpp_agents_;
    std::vector<BgpXmppChannel *> xmpp_peers_;
    int n_families_;
    std::vector<Address::Family> familes_;
    int n_instances_;
    int n_peers_;
    int n_routes_;
    int n_agents_;
    int n_targets_;
    bool xmpp_close_from_control_node_;

    struct AgentTestParams {
        AgentTestParams(test::NetworkAgentMock *agent, vector<int> instance_ids,
                        vector<int> nroutes) {
            initialize(agent, instance_ids, nroutes);
        }

        AgentTestParams(test::NetworkAgentMock *agent) {
            initialize(agent, vector<int>(), vector<int>());
        }

        void initialize(test::NetworkAgentMock *agent,
                        vector<int> instance_ids, vector<int> nroutes) {
            this->agent = agent;
            this->instance_ids = instance_ids;
            this->nroutes = nroutes;
        }

        test::NetworkAgentMock *agent;
        vector<int> instance_ids;
        vector<int> nroutes;
    };
    std::vector<AgentTestParams> n_flip_from_agents_;
    std::vector<test::NetworkAgentMock *> n_down_from_agents_;
};

void BgpPeerCloseTest::VerifyPeer(BgpServerTest *server,
                                  RoutingInstance *rtinstance,
                                  BgpNullPeer *npeer, BgpPeerTest *peer) {
    EXPECT_EQ((server)->FindPeer(rtinstance->name().c_str(), npeer->name_),
              peer);
}

void BgpPeerCloseTest::VerifyPeers() {
    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        VerifyPeer(server_.get(), master_instance_, npeer, npeer->peer());
    }
}

void BgpPeerCloseTest::VerifyNoPeers() {
    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        VerifyPeer(server_.get(), master_instance_, npeer,
                   static_cast<BgpPeerTest *>(NULL));
    }
}

void BgpPeerCloseTest::VerifyRoutes(int count) {
    // if (!n_peers_) return;

    for (int i = 0; i < n_families_; i++) {
        BgpTable *tb = master_instance_->GetTable(familes_[i]);
        if (count && n_agents_ && n_peers_ &&
                familes_[i] == Address::INETVPN) {
            BGP_VERIFY_ROUTE_COUNT(tb,
                    n_agents_ * n_instances_ * count + // XMPP Routes
                    n_peers_ * count); // BGP Routes
        } else {
            // BGP_VERIFY_ROUTE_COUNT(tb, count);
        }
    }
}

void BgpPeerCloseTest::VerifyRibOutCreationCompletion() {
    WaitForIdle();

    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        for (int i = 0; i < n_families_; i++) {
            EXPECT_TRUE(npeer->ribout_creation_complete(familes_[i]));
        }
    }
}

string BgpPeerCloseTest::GetConfig() {
    ostringstream out;

    out <<
    "<config>\
        <bgp-router name=\'A\'>\
            <identifier>192.168.0.1</identifier>\
            <address>127.0.0.1</address>\
            <port>" << server_->session_manager()->GetPort() << "</port>\
            <session to=\'B\'>\
                <address-families>\
                <family>inet-vpn</family>\
                <family>e-vpn</family>\
                <family>erm-vpn</family>\
                <family>route-target</family>\
                </address-families>\
            </session>\
    </bgp-router>\
    ";

    for (int i = 1; i <= n_instances_; i++) {
        out << "<routing-instance name='instance" << i << "'>\n";
        for (int j = 1; j <= n_targets_; j++) {
            out << "    <vrf-target>target:1:" << j << "</vrf-target>\n";
        }
        out << "</routing-instance>\n";
    }

    out << "</config>";

    BGP_DEBUG_UT("Applying config" << out.str());

    return out.str();
}

bool BgpPeerCloseTest::SendUpdate(BgpPeerTest *peer, const uint8_t *msg,
                              size_t msgsize) {
    return true;
}

bool BgpPeerCloseTest::IsReady(bool ready) {
    return ready;
}

bool BgpPeerCloseTest::MpNlriAllowed(BgpPeerTest *peer, uint16_t afi,
                                 uint8_t safi) {
    if (afi ==  BgpAf::IPv4 && safi == BgpAf::Unicast) {
        return true;
    }

    if (afi ==  BgpAf::IPv4 && safi == BgpAf::Vpn) {
        return true;
    }

    if (afi ==  BgpAf::L2Vpn && safi == BgpAf::EVpn) {
        return true;
    }

    return false;
}

BgpAttr *BgpPeerCloseTest::CreatePathAttr() {
    BgpAttrSpec attr_spec;
    BgpAttrDB *db = server_->attr_db();
    BgpAttr *attr = new BgpAttr(db, attr_spec);

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

    return attr;
}

ExtCommunitySpec *BgpPeerCloseTest::CreateRouteTargets() {
    auto_ptr<ExtCommunitySpec> commspec(new ExtCommunitySpec());

    for (int i = 1; i <= n_targets_; i++) {
        RouteTarget tgt = RouteTarget::FromString(
                "target:1:" + boost::lexical_cast<string>(i));
        const ExtCommunity::ExtCommunityValue &extcomm =
            tgt.GetExtCommunity();
        uint64_t value = get_value(extcomm.data(), extcomm.size());
        commspec->communities.push_back(value);
    }

    if (commspec->communities.empty()) return NULL;
    return commspec.release();
}

void BgpPeerCloseTest::AddRoutes(BgpTable *table, BgpNullPeer *npeer) {
    DBRequest req;
    boost::scoped_ptr<ExtCommunitySpec> commspec;
    boost::scoped_ptr<BgpAttrLocalPref> local_pref;
    IPeer *peer = npeer->peer();

#if 0
    InetVpnPrefix vpn_prefix(InetVpnPrefix::FromString(
                                                "123:456:192.168.255.0/32"));
#endif

    Ip4Prefix prefix(Ip4Prefix::FromString("192.168.255.0/24"));
    InetVpnPrefix vpn_prefix(InetVpnPrefix::FromString("123:456:20." +
                boost::lexical_cast<string>(npeer->peer_id()) + ".1.1/32"));

    for (int rt = 0; rt < n_routes_; rt++,
        prefix = task_util::Ip4PrefixIncrement(prefix),
        vpn_prefix = task_util::InetVpnPrefixIncrement(vpn_prefix)) {
        req.oper = DBRequest::DB_ENTRY_ADD_CHANGE;

        int localpref = 1 + (std::rand() % (n_peers_ + n_agents_));
        localpref = 100; // Default preference
        BgpAttrSpec attr_spec;
        local_pref.reset(new BgpAttrLocalPref(localpref));
        attr_spec.push_back(local_pref.get());

        BgpAttrNextHop nexthop(0x7f010000 + npeer->peer_id());
        attr_spec.push_back(&nexthop);
        TunnelEncap tun_encap(std::string("gre"));

        switch (table->family()) {
            case Address::INET:
                commspec.reset(new ExtCommunitySpec());
                commspec->communities.push_back(get_value(tun_encap.GetExtCommunity().begin(), 8));
                attr_spec.push_back(commspec.get());
                req.key.reset(new InetTable::RequestKey(prefix, peer));
                break;
            case Address::INETVPN:
                req.key.reset(new InetVpnTable::RequestKey(vpn_prefix, peer));
                commspec.reset(CreateRouteTargets());
                if (!commspec.get()) {
                    commspec.reset(new ExtCommunitySpec());
                }
                commspec->communities.push_back(get_value(tun_encap.GetExtCommunity().begin(), 8));
                attr_spec.push_back(commspec.get());
                break;
            default:
                assert(0);
                break;
        }

        uint32_t label = 20000 + rt;
        BgpAttrPtr attr = server_->attr_db()->Locate(attr_spec);
        req.data.reset(new InetTable::RequestData(attr, 0, label));
        table->Enqueue(&req);
    }
}

void BgpPeerCloseTest::AddAllRoutes() {
    RibExportPolicy policy(BgpProto::IBGP, RibExportPolicy::BGP, 1, 0);

    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        for (int i = 0; i < n_families_; i++) {
            BgpTable *table = master_instance_->GetTable(familes_[i]);

            server_->membership_mgr()->Register(npeer->peer(), table, policy,
                    -1, boost::bind(&BgpPeerCloseTest::CreateRibsDone, this, _1,
                                    _2, npeer));

            // Add routes to RibIn
            AddRoutes(table, npeer);
        }
        npeer->peer()->set_vpn_tables_registered(true);
    }

    WaitForIdle();
}

void BgpPeerCloseTest::AddXmppPeersWithRoutes() {
    if (!n_agents_) return;

    CreateAgents();

    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        TASK_UTIL_EXPECT_EQ(true, agent->IsEstablished());
    }

    WaitForIdle();
    Subscribe();
    VerifyReceivedXmppRoutes(0);
    AddOrDeleteXmppRoutes(true);
    WaitForIdle();
    VerifyReceivedXmppRoutes(n_instances_ * n_agents_ * n_routes_);
}

void BgpPeerCloseTest::CreateAgents() {
    Ip4Prefix prefix(Ip4Prefix::FromString("127.0.0.1/32"));

    for (int i = 0; i < n_agents_; i++) {

        // create an XMPP client in server A
        test::NetworkAgentMock *agent = new test::NetworkAgentMock(&evm_,
            "agent" + boost::lexical_cast<string>(i) +
                "@vnsw.contrailsystems.com",
            xmpp_server_->GetPort(),
            prefix.ip4_addr().to_string());
        agent->set_id(i);
        xmpp_agents_.push_back(agent);
        WaitForIdle();

        TASK_UTIL_EXPECT_NE_MSG(static_cast<BgpXmppChannel *>(NULL),
                          channel_manager_->channel_,
                          "Waiting for channel_manager_->channel_ to be set");
        xmpp_peers_.push_back(channel_manager_->channel_);
        channel_manager_->channel_ = NULL;

        prefix = task_util::Ip4PrefixIncrement(prefix);
    }
}

void BgpPeerCloseTest::Subscribe() {

    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        agent->Subscribe(BgpConfigManager::kMasterInstance, -1);
        for (int i = 1; i <= n_instances_; i++) {
            string instance_name = "instance" + boost::lexical_cast<string>(i);
            agent->Subscribe(instance_name, i);
        }
    }
    WaitForIdle();
}

void BgpPeerCloseTest::UnSubscribe() {

    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        agent->Unsubscribe(BgpConfigManager::kMasterInstance);
        for (int i = 1; i <= n_instances_; i++) {
            string instance_name = "instance" + boost::lexical_cast<string>(i);
            agent->Unsubscribe(instance_name);
        }
    }
    VerifyReceivedXmppRoutes(0);
    WaitForIdle();
}

void BgpPeerCloseTest::AddOrDeleteXmppRoutes(bool add, int n_routes,
                                             int down_agents) {
    if (n_routes ==-1)
        n_routes = n_routes_;

    if (down_agents == -1)
        down_agents = n_agents_;

    int agent_id = 1;
    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        if (down_agents-- < 1)
            continue;

        for (int i = 1; i <= n_instances_; i++) {
            string instance_name = "instance" + boost::lexical_cast<string>(i);

            Ip4Prefix prefix(Ip4Prefix::FromString(
                "10." + boost::lexical_cast<string>(i) +
                boost::lexical_cast<string>(agent_id) + ".1/32"));
            for (int rt = 0; rt < n_routes; rt++,
                prefix = task_util::Ip4PrefixIncrement(prefix)) {
                if (add)
                    agent->AddRoute(instance_name, prefix.ToString(),
                                    "100.100.100." +
                                    boost::lexical_cast<string>(agent_id));
                else
                    agent->DeleteRoute(instance_name, prefix.ToString());
            }
        }
        agent_id++;
    }
    WaitForIdle();
    // if (!add) VerifyReceivedXmppRoutes(0);
}

void BgpPeerCloseTest::VerifyReceivedXmppRoutes(int routes) {
    if (!n_agents_) return;

    int agent_id = 0;
    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        agent_id++;
        if (routes > 0 && !agent->IsEstablished())
            continue;
        for (int i = 1; i <= n_instances_; i++) {
            string instance_name = "instance" + boost::lexical_cast<string>(i);
            if (!agent->HasSubscribed(instance_name))
                continue;
            TASK_UTIL_EXPECT_EQ_MSG(routes, agent->RouteCount(instance_name),
                                    "Wait for routes in " + instance_name);
            ASSERT_TRUE(agent->RouteCount(instance_name) == routes);
        }
    }
    WaitForIdle();
}

void BgpPeerCloseTest::DeleteAllRoutingInstances() {
    ostringstream out;
    out << "<delete>";
    for (int i = 1; i <= n_instances_; i++) {
        out << "<routing-instance name='instance" << i << "'>\n";
        for (int j = 1; j <= n_targets_; j++) {
            out << "    <vrf-target>target:1:" << j << "</vrf-target>\n";
        }
        out << "</routing-instance>\n";
    }

    out << "</delete>";


    server_->Configure(out.str().c_str());
    WaitForIdle();
}

void BgpPeerCloseTest::VerifyRoutingInstances() {
    for (int i = 1; i <= n_instances_; i++) {
        string instance_name = "instance" + boost::lexical_cast<string>(i);
        TASK_UTIL_EXPECT_NE(static_cast<RoutingInstance *>(NULL),
                            server_->routing_instance_mgr()->\
                                GetRoutingInstance(instance_name));
    }

    //
    // Verify 'default' master routing-instance
    //
    TASK_UTIL_EXPECT_NE(static_cast<RoutingInstance *>(NULL),
                        server_->routing_instance_mgr()->GetRoutingInstance(
                               BgpConfigManager::kMasterInstance));
}

void BgpPeerCloseTest::AddPeersWithRoutes(
        const BgpInstanceConfig *instance_config) {

    Configure();
    SetPeerCloseGraceful(false);

    // Add XmppPeers with routes as well
    AddXmppPeersWithRoutes();

    for (int p = 1; p <= n_peers_; p++) {
        ostringstream oss;

        oss << "NullPeer" << p;
        BgpNullPeer *npeer =
            new BgpNullPeer(server_.get(), instance_config, oss.str(),
                            master_instance_, p);
        VerifyPeer(server_.get(), master_instance_, npeer, npeer->peer());

        // Override certain default routines to customize behavior that we want
        // in this test.
        npeer->peer()->SendUpdate_fnc_ =
            boost::bind(&BgpPeerCloseTest::SendUpdate, this, npeer->peer(),
                        _1, _2);
        npeer->peer()->MpNlriAllowed_fnc_ =
            boost::bind(&BgpPeerCloseTest::MpNlriAllowed, this, npeer->peer(),
                        _1, _2);
        npeer->peer()->IsReady_fnc_ =
            boost::bind(&BgpPeerCloseTest::IsReady, this, true);
        peers_.push_back(npeer);
    }

    AddAllRoutes();
}

// Invoke stale timer callbacks as evm is not running in this unit test
void BgpPeerCloseTest::CallStaleTimer(bool bgp_peers_ready) {
    BOOST_FOREACH(BgpNullPeer *peer, peers_) {
        peer->peer()->IsReady_fnc_ =
            boost::bind(&BgpPeerCloseTest::IsReady, this, bgp_peers_ready);
        peer->peer()->peer_close()->close_manager()->StaleTimerCallback();
    }

    BOOST_FOREACH(BgpXmppChannel *peer, xmpp_peers_) {
        peer->Peer()->peer_close()->close_manager()->StaleTimerCallback();
    }

    WaitForIdle();
}

void BgpPeerCloseTest::XmppPeerClose(int npeers) {
    if (npeers < 1)
        npeers = xmpp_agents_.size();

    int down_count = npeers;
    if (xmpp_close_from_control_node_) {
        BOOST_FOREACH(BgpXmppChannel *peer, xmpp_peers_) {
            peer->Peer()->Close();
            if (!--down_count)
                break;
        }
    } else {
        BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
            agent->SessionDown();
            if (!--down_count)
                break;
        }
    }

    down_count = npeers;
    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        TASK_UTIL_EXPECT_EQ(down_count < 1, agent->IsEstablished());
        down_count--;
    }
}

void BgpPeerCloseTest::InitParams() {
    n_instances_ = ::std::tr1::get<0>(GetParam());
    n_routes_ = ::std::tr1::get<1>(GetParam());
    n_peers_ = ::std::tr1::get<2>(GetParam());
    n_agents_ = ::std::tr1::get<3>(GetParam());
    n_targets_ = ::std::tr1::get<4>(GetParam());
    xmpp_close_from_control_node_ = ::std::tr1::get<5>(GetParam());
}

// Peer flaps
//
// Run all tests with  n peers, n routes, and address-families combined
//
// 1. Close with RibIn delete
// 2. Close with RibIn Stale, followed by RibIn Delete
// 3. Close with RibIn Stale, followed by RibIn Sweep
//
// Config deletions
//
// 1. Peer(s) deletion from config
// 2. Routing Instance(s) deletion from config
// 3. Delete entire bgp routing config
//
//
// Config modifications
//
// 1. Modify router-id
// 2. Toggle graceful restart options for address families selectively
// 3. Modify graceful restart timer values for address families selectively
// 4. Neighbor inbound policy channge (In future..)
//
// 1. Repeated close in each of the above case before the close is complete
// 2. Repeated close in each of the above case after the close is complete (?)

/*
TEST_P(BgpPeerCloseTest, ClosePeers) {
    SCOPED_TRACE(__FUNCTION__);
    InitParams();

    AddPeersWithRoutes(master_cfg_.get());
    VerifyPeers();
    VerifyRoutes(n_routes_);
    VerifyRibOutCreationCompletion();

    // BgpShow::Instance(server_.get());

    // Trigger ribin deletes
    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        npeer->peer()->SetAdminState(true);
    }

    XmppPeerClose();

    // Assert that all ribins for all families have been deleted correctly
    VerifyPeers();
    VerifyRoutes(0);
}

TEST_P(BgpPeerCloseTest, DeletePeers) {
    SCOPED_TRACE(__FUNCTION__);
    InitParams();
    AddPeersWithRoutes(master_cfg_.get());
    WaitForIdle();
    VerifyPeers();
    VerifyRoutes(n_routes_);
    VerifyRibOutCreationCompletion();

    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        npeer->peer()->ManagedDelete();
    }

    XmppPeerClose();

    // Assert that all ribins have been deleted correctly
    VerifyNoPeers();
    VerifyRoutes(0);
}

TEST_P(BgpPeerCloseTest, DeleteXmppRoutes) {
    SCOPED_TRACE(__FUNCTION__);
    InitParams();
    AddPeersWithRoutes(master_cfg_.get());
    WaitForIdle();
    VerifyPeers();
    VerifyRoutes(n_routes_);
    VerifyRibOutCreationCompletion();

    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        npeer->peer()->ManagedDelete();
    }

    AddOrDeleteXmppRoutes(false);

    // Assert that all ribins have been deleted correctly
    VerifyNoPeers();
    VerifyRoutes(0);
}

TEST_P(BgpPeerCloseTest, Unsubscribe) {
    SCOPED_TRACE(__FUNCTION__);
    InitParams();
    AddPeersWithRoutes(master_cfg_.get());
    WaitForIdle();
    VerifyPeers();
    VerifyRoutes(n_routes_);
    VerifyRibOutCreationCompletion();

    BOOST_FOREACH(BgpNullPeer *npeer, peers_) {
        npeer->peer()->ManagedDelete();
    }

    UnSubscribe();

    // Assert that all ribins have been deleted correctly
    VerifyNoPeers();
    VerifyRoutes(0);
}

TEST_P(BgpPeerCloseTest, DeleteRoutingInstances) {
    SCOPED_TRACE(__FUNCTION__);
    InitParams();
    AddPeersWithRoutes(master_cfg_.get());
    WaitForIdle();
    VerifyPeers();
    VerifyRoutes(n_routes_);
    VerifyRibOutCreationCompletion();

    // Delete the routing instance
    DeleteAllRoutingInstances();
    UnSubscribe();
    WaitForIdle();

    TASK_UTIL_EXPECT_EQ_MSG(1, server_->routing_instance_mgr()->count(),
        "Waiting for the completion of routing-instances' deletion");
}
*/

// Bring up n_agents_ in n_instances_ and advertise
//     n_routes_ (v4 and v6) in each connection
// Verify that n_agents_ * n_instances_ * n_routes_ routes are received in
//     agent in each instance
// * Subset * picked serially/randomly
// Subset of agents support GR
// Subset of agents go down permanently (Triggered from agents)
// Subset of agents flip (go down and come back up) (Triggered from agents)
// Subset of agents go down permanently (Triggered from control-node)
// Subset of agents flip (Triggered from control-node)
//     Subset of subscriptions after restart
//     Subset of routes are [re]advertised after restart
void BgpPeerCloseTest::GracefulRestartTestStart () {
    InitParams();

    //  Bring up n_agents_ in n_instances_ and advertise n_routes_ per session
    AddPeersWithRoutes(master_cfg_.get());
    VerifyPeers();
    VerifyRoutes(n_routes_);
    VerifyRibOutCreationCompletion();
}

void BgpPeerCloseTest::GracefulRestartTestRun () {
    int total_routes = n_instances_ * n_agents_ * n_routes_;

    //  Verify that n_agents_ * n_instances_ * n_routes_ routes are received in
    //  agent in each instance
    VerifyReceivedXmppRoutes(total_routes);

    // Subset of agents support GR
    // BOOST_FOREACH(test::NetworkAgentMock *agent, n_gr_supported_agents)
        SetPeerCloseGraceful(true);

    //  Subset of agents go down permanently (Triggered from agents)
    BOOST_FOREACH(test::NetworkAgentMock *agent, n_down_from_agents_) {
        TASK_UTIL_EXPECT_EQ(true, agent->IsEstablished());
        agent->SessionDown();
        TASK_UTIL_EXPECT_EQ(false, agent->IsEstablished());
        total_routes -= n_instances_ * n_routes_;
    }

    //  Subset of agents flip (Triggered from agents)
    BOOST_FOREACH(AgentTestParams agent_test_param, n_flip_from_agents_) {
        test::NetworkAgentMock *agent = agent_test_param.agent;
        TASK_UTIL_EXPECT_EQ(true, agent->IsEstablished());
        agent->SessionDown();
        TASK_UTIL_EXPECT_EQ(false, agent->IsEstablished());
        total_routes -= n_instances_ * n_routes_;
    }

    BOOST_FOREACH(AgentTestParams agent_test_param, n_flip_from_agents_) {
        test::NetworkAgentMock *agent = agent_test_param.agent;
        TASK_UTIL_EXPECT_EQ(false, agent->IsEstablished());
        agent->SessionUp();
    }
    WaitForIdle();

    BOOST_FOREACH(AgentTestParams agent_test_param, n_flip_from_agents_) {
        test::NetworkAgentMock *agent = agent_test_param.agent;
        TASK_UTIL_EXPECT_EQ(true, agent->IsEstablished());

        // Subset of subscriptions after restart
        agent->Subscribe(BgpConfigManager::kMasterInstance, -1);
        for (size_t i = 0; i < agent_test_param.instance_ids.size(); i++) {
            int instance_id = agent_test_param.instance_ids[i];
            string instance_name = "instance" +
                boost::lexical_cast<string>(instance_id);
            agent->Subscribe(instance_name, instance_id);
            // Subset of routes are [re]advertised after restart
            Ip4Prefix prefix(Ip4Prefix::FromString(
                "10." + boost::lexical_cast<string>(instance_id) +
                boost::lexical_cast<string>(agent->id()) + ".1/32"));
            int nroutes = agent_test_param.nroutes[i];
            for (int rt = 0; rt < nroutes; rt++,
                prefix = task_util::Ip4PrefixIncrement(prefix)) {
                agent->AddRoute(instance_name, prefix.ToString(),
                    "100.100.100." + boost::lexical_cast<string>(agent->id()));
            }
            total_routes += nroutes;
        }
    }
    WaitForIdle();

    // Directly invoke stale timer callbacks
    CallStaleTimer(true);
    VerifyReceivedXmppRoutes(total_routes);
}

// None of the agents go down permanently
TEST_P(BgpPeerCloseTest, GracefulRestart_Down_1) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();
    GracefulRestartTestRun();
}

// All agents go down permanently
TEST_P(BgpPeerCloseTest, GracefulRestart_Down_2) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    n_down_from_agents_ = xmpp_agents_;
    GracefulRestartTestRun();
}

// Some agents go down permanently
TEST_P(BgpPeerCloseTest, GracefulRestart_Down_3) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    for (size_t i = 0; i < xmpp_agents_.size()/2; i++)
        n_down_from_agents_.push_back(xmpp_agents_[i]);
    GracefulRestartTestRun();
}

// Some agents go down permanently and some flip (which sends no routes)
TEST_P(BgpPeerCloseTest, GracefulRestart_Down_4) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    for (size_t i = 0; i < xmpp_agents_.size(); i++) {
        if (i <= xmpp_agents_.size()/2)
            n_down_from_agents_.push_back(xmpp_agents_[i]);
        else
            n_flip_from_agents_.push_back(AgentTestParams(xmpp_agents_[i]));
    }
    GracefulRestartTestRun();
}

// All agents come back up but do not subscribe to any instance
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_1) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        n_flip_from_agents_.push_back(AgentTestParams(agent));
    }
    GracefulRestartTestRun();
}

// All agents come back up and subscribe to all instances and sends all routes
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_2) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        vector<int> instance_ids = vector<int>();
        vector<int> nroutes = vector<int>();
        for (int i = 1; i <= n_instances_; i++) {
            instance_ids.push_back(i);
            nroutes.push_back(n_routes_);
        }
        n_flip_from_agents_.push_back(AgentTestParams(agent, instance_ids,
                                                      nroutes));
    }
    GracefulRestartTestRun();
}

// All agents come back up and subscribe to all instances but sends no routes
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_3) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        vector<int> instance_ids = vector<int>();
        vector<int> nroutes = vector<int>();
        for (int i = 1; i <= n_instances_; i++) {
            instance_ids.push_back(i);
            nroutes.push_back(0);
        }
        n_flip_from_agents_.push_back(AgentTestParams(agent, instance_ids,
                                                      nroutes));
    }
    GracefulRestartTestRun();
}

// All agents come back up and subscribe to all instances and sends some routes
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_4) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    BOOST_FOREACH(test::NetworkAgentMock *agent, xmpp_agents_) {
        vector<int> instance_ids = vector<int>();
        vector<int> nroutes = vector<int>();
        for (int i = 1; i <= n_instances_; i++) {
            instance_ids.push_back(i);
            nroutes.push_back(n_routes_/2);
        }
        n_flip_from_agents_.push_back(AgentTestParams(agent, instance_ids,
                                                      nroutes));
    }
    GracefulRestartTestRun();
}





// Some agents come back up but do not subscribe to any instance
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_Some_1) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    for (size_t i = 0; i < xmpp_agents_.size()/2; i++) {
        test::NetworkAgentMock *agent = xmpp_agents_[i];
        n_flip_from_agents_.push_back(AgentTestParams(agent));
    }
    GracefulRestartTestRun();
}

// Some agents come back up and subscribe to all instances and sends all routes
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_Some_2) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    for (size_t i = 0; i < xmpp_agents_.size()/2; i++) {
        test::NetworkAgentMock *agent = xmpp_agents_[i];
        vector<int> instance_ids = vector<int>();
        vector<int> nroutes = vector<int>();
        for (int j = 1; j <= n_instances_; j++) {
            instance_ids.push_back(j);
            nroutes.push_back(n_routes_);
        }
        n_flip_from_agents_.push_back(AgentTestParams(agent, instance_ids,
                                                      nroutes));
    }
    GracefulRestartTestRun();
}

// Some agents come back up and subscribe to all instances but sends no routes
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_Some_3) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    for (size_t i = 0; i < xmpp_agents_.size()/2; i++) {
        test::NetworkAgentMock *agent = xmpp_agents_[i];
        vector<int> instance_ids = vector<int>();
        vector<int> nroutes = vector<int>();
        for (int j = 1; j <= n_instances_; j++) {
            instance_ids.push_back(j);
            nroutes.push_back(0);
        }
        n_flip_from_agents_.push_back(AgentTestParams(agent, instance_ids,
                                                      nroutes));
    }
    GracefulRestartTestRun();
}

// Some agents come back up and subscribe to all instances and sends some routes
TEST_P(BgpPeerCloseTest, GracefulRestart_Flap_Some_4) {
    SCOPED_TRACE(__FUNCTION__);
    GracefulRestartTestStart();

    for (size_t i = 0; i < xmpp_agents_.size()/2; i++) {
        test::NetworkAgentMock *agent = xmpp_agents_[i];
        vector<int> instance_ids = vector<int>();
        vector<int> nroutes = vector<int>();
        for (int j = 1; j <= n_instances_; j++) {
            instance_ids.push_back(j);
            nroutes.push_back(n_routes_/2);
        }
        n_flip_from_agents_.push_back(AgentTestParams(agent, instance_ids,
                                                      nroutes));
    }
    GracefulRestartTestRun();
}

#define COMBINE_PARAMS \
    Combine(ValuesIn(GetInstanceParameters()),                      \
            ValuesIn(GetRouteParameters()),                         \
            ValuesIn(GetPeerParameters()),                          \
            ValuesIn(GetAgentParameters()),                         \
            ValuesIn(GetTargetParameters()),                        \
            ValuesIn(xmpp_close_from_control_node))

INSTANTIATE_TEST_CASE_P(BgpPeerCloseTestWithParams, BgpPeerCloseTest,
                        COMBINE_PARAMS);

class TestEnvironment : public ::testing::Environment {
    virtual ~TestEnvironment() { }
};

static void SetUp() {
    ControlNode::SetDefaultSchedulingPolicy();
    BgpServerTest::GlobalSetUp();
    BgpObjectFactory::Register<PeerCloseManager>(
        boost::factory<PeerCloseManagerTest *>());
    BgpObjectFactory::Register<BgpXmppMessageBuilder>(
        boost::factory<BgpXmppMessageBuilder *>());
}

static void TearDown() {
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    scheduler->Terminate();
}

int main(int argc, char **argv) {
    gargc = argc;
    gargv = argv;

    bgp_log_test::init();
    ::testing::InitGoogleTest(&gargc, gargv);
    ::testing::AddGlobalTestEnvironment(new TestEnvironment());
    SetUp();
    int result = RUN_ALL_TESTS();
    TearDown();
    return result;
}
