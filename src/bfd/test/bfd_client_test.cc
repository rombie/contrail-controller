/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>
#include <testing/gunit.h>

#include "bfd/bfd_client.h"
#include "bfd/bfd_control_packet.h"
#include "bfd/bfd_server.h"
#include "bfd/bfd_session.h"
#include "bfd/test/bfd_test_utils.h"

#include "io/test/event_manager_test.h"
#include "base/test/task_test_util.h"

using namespace BFD;
using namespace std;

using std::pair;
using std::size_t;

class Communicator : public Connection {
public:
    typedef map<boost::asio::ip::address,
            pair<Connection *, boost::asio::ip::address> > Links;

    Communicator() : notified_up_(0), notified_down_(0) { }
    virtual ~Communicator() { }

    virtual void SendPacket(const boost::asio::ip::address &dstAddr,
                            const boost::asio::mutable_buffer &buffer,
                            int pktSize) {
        // Find other end-point from the links map.
        Links::const_iterator it = links_.find(dstAddr);
        if (it != links_.end()) {
            boost::system::error_code error;
            it->second.first->HandleReceive(buffer,
                boost::asio::ip::udp::endpoint(it->second.second, 1234),
                pktSize, error);
        }
        delete[] boost::asio::buffer_cast<const uint8_t *>(buffer);
    }
#if 0
    virtual void HandleReceive(const boost::asio::const_buffer &recv_buffer,
                               boost::asio::ip::udp::endpoint remote_endpoint,
                               size_t bytes_transferred,
                               const boost::system::error_code& error) {
        Connection::HandleReceive(recv_buffer, remote_endpoint,
                                  bytes_transferred, error);
    }
#endif
    virtual void NotifyStateChange(const boost::asio::ip::address &remoteHost,
                                   const bool &up) {
        if (up)
            notified_up_++;
        else
            notified_down_++;
    }
    virtual Server *GetServer() const { return server_; }
    virtual void SetServer(Server *server) { server_ = server; }
    Links *links() { return &links_; }
    int notified_up() const { return notified_up_; }
    int notified_down() const { return notified_down_; }

private:
    Server *server_;
    Links links_;
    int notified_up_;
    int notified_down_;
};

class ClientTest : public ::testing::Test {
 protected:
    ClientTest() :
        server_(&evm_, &cm_), client_(&cm_),
        server_test_(&evm_, &cm_test_), client_test_(&cm_test_) {
    }

    virtual void SetUp() {
        thread_.reset(new ServerThread(&evm_));
        thread_->Start();
    }

    virtual void TearDown() {
        evm_.Shutdown();
        thread_->Join();
    }

    void ConfigureSessions();
    EventManager evm_;
    auto_ptr<ServerThread> thread_;
    Communicator cm_;
    Server server_;
    Client client_;

    // Test BFD end-points
    Communicator cm_test_;
    Server server_test_;
    Client client_test_;
    boost::asio::ip::address client_address_;
    boost::asio::ip::address client_test_address_;
};


void ClientTest::ConfigureSessions() {
    client_address_ = boost::asio::ip::address::from_string("10.10.10.1");
    client_test_address_ = boost::asio::ip::address::from_string("192.168.0.1");

    SessionConfig sc;
    sc.desiredMinTxInterval = boost::posix_time::milliseconds(30);
    sc.requiredMinRxInterval = boost::posix_time::milliseconds(50);
    sc.detectionTimeMultiplier = 3;
    EXPECT_EQ(kResultCode_Ok, client_.AddConnection(client_address_, sc));

    SessionConfig sc_t;
    sc_t.desiredMinTxInterval = boost::posix_time::milliseconds(30);
    sc_t.requiredMinRxInterval = boost::posix_time::milliseconds(50);
    sc_t.detectionTimeMultiplier = 3;
    EXPECT_EQ(kResultCode_Ok, client_test_.AddConnection(client_test_address_,
                                                         sc_t));
    TASK_UTIL_EXPECT_FALSE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_EQ(1, cm_.notified_down());
    TASK_UTIL_EXPECT_EQ(0, cm_.notified_up());

    TASK_UTIL_EXPECT_FALSE(client_test_.Up(client_test_address_));
    TASK_UTIL_EXPECT_EQ(1, cm_test_.notified_down());
    TASK_UTIL_EXPECT_EQ(0, cm_test_.notified_up());
}

// Verify that BFD sessions come UP.
TEST_F(ClientTest, Basic1) {
    ConfigureSessions();

    // Connect two bfd links
    cm_.links()->insert(make_pair(client_address_,
                        make_pair(&cm_test_, client_test_address_)));
    cm_test_.links()->insert(make_pair(client_test_address_,
                             make_pair(&cm_, client_address_)));
    TASK_UTIL_EXPECT_TRUE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_EQ(1, cm_.notified_down());
    TASK_UTIL_EXPECT_EQ(1, cm_.notified_up());

    TASK_UTIL_EXPECT_TRUE(client_test_.Up(client_test_address_));
    TASK_UTIL_EXPECT_EQ(1, cm_test_.notified_down());
    TASK_UTIL_EXPECT_EQ(1, cm_test_.notified_up());
}

// Verify that UP BFD sessions go down if connectivity is lost from one
// direction.
TEST_F(ClientTest, Basic2) {
    ConfigureSessions();

    // Connect two bfd links
    cm_.links()->insert(make_pair(client_address_,
                        make_pair(&cm_test_, client_test_address_)));
    cm_test_.links()->insert(make_pair(client_test_address_,
                             make_pair(&cm_, client_address_)));
    TASK_UTIL_EXPECT_TRUE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_EQ(1, cm_.notified_down());
    TASK_UTIL_EXPECT_EQ(1, cm_.notified_up());

    TASK_UTIL_EXPECT_TRUE(client_test_.Up(client_test_address_));
    TASK_UTIL_EXPECT_EQ(1, cm_test_.notified_down());
    TASK_UTIL_EXPECT_EQ(1, cm_test_.notified_up());

    // Bring down connetivity from one side and make sure that state goes down
    // on both.
    cm_.links()->erase(client_address_);

    TASK_UTIL_EXPECT_FALSE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_EQ(2, cm_.notified_down());
    TASK_UTIL_EXPECT_EQ(1, cm_.notified_up());

    TASK_UTIL_EXPECT_FALSE(client_test_.Up(client_test_address_));
    TASK_UTIL_EXPECT_EQ(2, cm_test_.notified_down());
    TASK_UTIL_EXPECT_EQ(1, cm_test_.notified_up());
}

// Verify that UP BFD sessions go down if connectivity is lost from both
// directions.
TEST_F(ClientTest, Basic3) {
    ConfigureSessions();

    // Connect two bfd links
    cm_.links()->insert(make_pair(client_address_,
                        make_pair(&cm_test_, client_test_address_)));
    cm_test_.links()->insert(make_pair(client_test_address_,
                             make_pair(&cm_, client_address_)));
    TASK_UTIL_EXPECT_TRUE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_TRUE(client_test_.Up(client_test_address_));

    // Bring down connetivity from one side and make sure that state goes down
    // on both.
    cm_.links()->erase(client_address_);
    cm_.links()->erase(client_test_address_);

    TASK_UTIL_EXPECT_FALSE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_FALSE(client_test_.Up(client_test_address_));
}

// Verify that DOWN BFD session comes up after link connectivity is restored.
TEST_F(ClientTest, Basic4) {
    ConfigureSessions();

    // Connect one bfd link
    cm_.links()->insert(make_pair(client_address_,
                        make_pair(&cm_test_, client_test_address_)));
    task_util::WaitForIdle();

    // Session should remain down
    TASK_UTIL_EXPECT_FALSE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_FALSE(client_test_.Up(client_test_address_));

    // Bring up other connection to establish bi-directional connectivity.
    cm_test_.links()->insert(make_pair(client_test_address_,
                             make_pair(&cm_, client_address_)));
    TASK_UTIL_EXPECT_TRUE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_TRUE(client_test_.Up(client_test_address_));

    // Bring down connetivity from one side and make sure that state goes down
    // on both.
    cm_.links()->erase(client_address_);

    TASK_UTIL_EXPECT_FALSE(client_.Up(client_address_));
    TASK_UTIL_EXPECT_FALSE(client_test_.Up(client_test_address_));
}

int main(int argc, char **argv) {
    LoggingInit();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
