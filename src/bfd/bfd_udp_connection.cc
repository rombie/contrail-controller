/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#include <boost/asio.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/random.hpp>

#include "bfd/bfd_udp_connection.h"
#include "bfd/bfd_connection.h"
#include "bfd/bfd_control_packet.h"
#include "bfd/bfd_common.h"
#include "bfd/bfd_server.h"

#include "base/logging.h"

namespace BFD {

UDPConnectionManager::UDPRecvServer::UDPRecvServer(UDPConnectionManager *parent,
                                       EventManager *evm,
                                       int recvPort)
        : UdpServer(evm), parent_(parent) {
    Initialize(recvPort);
}

void UDPConnectionManager::UDPRecvServer::RegisterCallback(
                                    RecvCallback callback) {
    this->callback_ = callback;
}

void UDPConnectionManager::UDPRecvServer::HandleReceive(
        const boost::asio::const_buffer &recv_buffer,
        boost::asio::ip::udp::endpoint remote_endpoint,
        std::size_t bytes_transferred,
        const boost::system::error_code& error) {
    if (callback_)
        callback_.get()(remote_endpoint, recv_buffer, bytes_transferred, error);
    else
        parent_->HandleReceive(recv_buffer, remote_endpoint, bytes_transferred,
                               error);
}

UDPConnectionManager::UDPCommunicator::UDPCommunicator(EventManager *evm,
                                                       int remotePort)
                                : UdpServer(evm), remotePort_(remotePort) {
    boost::random::uniform_int_distribution<> dist(kSendPortMin, kSendPortMax);
    for (int i = 0; i < 100 && GetServerState() != OK; ++i) {
        int localPort = dist(randomGen);
        LOG(DEBUG, "Bind UDPCommunicator to localport: " << localPort);
        Initialize(localPort);
        if (GetServerState() != OK) {
            Shutdown();
        }
    }

    if (GetServerState() != OK) {
        LOG(ERROR, "Unable to bind to port in range: " << kSendPortMin
                   << "-" << kSendPortMax);
    }
}

UDPConnectionManager::UDPConnectionManager(EventManager *evm, int recvPort,
                                           int remotePort)
          : udpRecv_(new BFD::UDPConnectionManager::UDPRecvServer(this, evm,
                     recvPort)),
            udpSend_(new BFD::UDPConnectionManager::UDPCommunicator(evm,
                     remotePort)), server_(NULL) {
    if (udpRecv_->GetServerState() != UDPRecvServer::OK)
        LOG(ERROR, "Unable to listen on port " << recvPort);
    else
        udpRecv_->StartReceive();
}

Server *UDPConnectionManager::GetServer() const {
    return server_;
}

void UDPConnectionManager::SetServer(Server *server) {
    server_ = server;
}

void UDPConnectionManager::RegisterCallback(RecvCallback callback) {
    udpRecv_->RegisterCallback(callback);
}

void UDPConnectionManager::SendPacket(boost::asio::ip::address remoteHost,
                                      const ControlPacket *packet) {
    LOG(DEBUG, __func__);
    boost::asio::mutable_buffer send =
        boost::asio::mutable_buffer(new u_int8_t[kMinimalPacketLength],
                                    kMinimalPacketLength);
    int pktSize = EncodeControlPacket(packet,
                                      boost::asio::buffer_cast<uint8_t *>(send),
                                      kMinimalPacketLength);
    if (pktSize != kMinimalPacketLength)
        LOG(ERROR, "Unable to encode packet");
    else
        SendPacket(remoteHost, send, pktSize);
}

void UDPConnectionManager::SendPacket(const boost::asio::ip::address &dstAddr,
                                      const boost::asio::mutable_buffer &send,
                                      int pktSize) {
    LOG(DEBUG, __func__);
    boost::asio::ip::udp::endpoint dstEndpoint(dstAddr, udpSend_->remotePort());
    udpSend_->StartSend(dstEndpoint, pktSize, send);
}

UDPConnectionManager::~UDPConnectionManager() {
    udpRecv_->Shutdown();
    udpSend_->Shutdown();
    UdpServerManager::DeleteServer(udpRecv_);
    UdpServerManager::DeleteServer(udpSend_);
}

void UDPConnectionManager::NotifyStateChange(
            const boost::asio::ip::address& remoteHost, const bool &up) {
}

void UDPConnectionManager::HandleReceive(
        const boost::asio::const_buffer &recv_buffer,
        boost::asio::ip::udp::endpoint remote_endpoint,
        std::size_t bytes_transferred, const boost::system::error_code& error) {
    GetServer()->ProcessControlPacket(remote_endpoint, recv_buffer,
                                      bytes_transferred, error);
}

}  // namespace BFD
