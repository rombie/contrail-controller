/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#ifndef SRC_BFD_UDP_CONNECTION_H_
#define SRC_BFD_UDP_CONNECTION_H_

#include "bfd/bfd_connection.h"

#include <boost/optional.hpp>

#include "io/udp_server.h"

namespace BFD {
class UDPConnectionManager : public Connection {
 public:
    typedef boost::function<void(boost::asio::ip::udp::endpoint remote_endpoint,
                                 const boost::asio::const_buffer &recv_buffer,
                                 std::size_t bytes_transferred,
                                 const boost::system::error_code& error)>
                RecvCallback;

    UDPConnectionManager(EventManager *evm, int recvPort = kRecvPortDefault,
                         int remotePort = kRecvPortDefault);
    ~UDPConnectionManager();
    void RegisterCallback(RecvCallback callback);

    virtual void HandleReceive(const boost::asio::const_buffer &recv_buffer,
                               boost::asio::ip::udp::endpoint remote_endpoint,
                               std::size_t bytes_transferred,
                               const boost::system::error_code& error);
    virtual void SendPacket(const boost::asio::ip::address &dstAddr,
                            const boost::asio::mutable_buffer &send,
                            int pktSize);
    void SendPacket(boost::asio::ip::address remoteHost,
                    const ControlPacket *packet);
    virtual Server *GetServer() const;
    virtual void SetServer(Server *server);
    virtual void NotifyStateChange(const boost::asio::ip::address& remoteHost,
                                   const bool &up);

 private:
    static const int kRecvPortDefault = 3784;
    static const int kSendPortMin = 49152;
    static const int kSendPortMax = 65535;

    class UDPRecvServer : public UdpServer {
     public:
        UDPRecvServer(UDPConnectionManager *parent,
                      EventManager *evm, int recvPort);
        void RegisterCallback(RecvCallback callback);
        void HandleReceive(const boost::asio::const_buffer &recv_buffer,
                boost::asio::ip::udp::endpoint remote_endpoint,
                std::size_t bytes_transferred,
                const boost::system::error_code& error);

     private:
        UDPConnectionManager *parent_;
        boost::optional<RecvCallback> callback_;
    } *udpRecv_;

    class UDPCommunicator : public UdpServer {
     public:
        UDPCommunicator(EventManager *evm, int remotePort);
        // TODO(bfd) add multiple instances to randomize source port (RFC5881)
        int remotePort() const { return remotePort_; }
     private:
        const int remotePort_;
    } *udpSend_;

    Server *server_;
};
}  // namespace BFD

#endif  // SRC_BFD_UDP_CONNECTION_H_
