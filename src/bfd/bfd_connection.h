/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#ifndef SRC_BFD_BFD_CONNECTION_H_
#define SRC_BFD_BFD_CONNECTION_H_

#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>
#include "bfd/bfd_server.h"

namespace BFD {
class ControlPacket;
class Server;

class Connection {
 public:
    virtual void SendPacket(const boost::asio::ip::address &dstAddr,
                            const boost::asio::mutable_buffer &packet,
                            int pktSize) = 0;
    virtual void HandleReceive(const boost::asio::const_buffer &recv_buffer,
                               boost::asio::ip::udp::endpoint remote_endpoint,
                               std::size_t bytes_transferred,
                               const boost::system::error_code& error) {
        GetServer()->ProcessControlPacket(remote_endpoint, recv_buffer,
                                          bytes_transferred, error);
    }
    virtual void NotifyStateChange(const boost::asio::ip::address& remoteHost,
                                   const bool &up) = 0;
    virtual Server *GetServer() const = 0;
    virtual void SetServer(Server *server) = 0;
};

}  // namespace BFD

#endif  // SRC_BFD_BFD_CONNECTION_H_
