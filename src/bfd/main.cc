/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/asio.hpp>

#include "bfd/bfd_server.h"
#include "bfd/bfd_session.h"

using namespace BFD;

class Communicator : public Connection {
 public:
    Communicator() {
    virtual void SendPacket(const boost::asio::ip::address &dstAddr,
                            const ControlPacket *packet) {
    }
    virtual ~TestCommunicator() { }
 private:

};


int main(int argc, char *argv[]) {
    LoggingInit();

    EventManager evm;
    boost::scoped_ptr<Connection> communicator1(new Communicator());
    Server server1(&evm, communicator1.get());

    // Event loop.
    evm.Run();
    return 0;
}
