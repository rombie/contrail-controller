/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#include "bfd/bfd_server.h"
#include "bfd/bfd_session.h"
#include "bfd/bfd_connection.h"
#include "bfd/bfd_control_packet.h"
#include "bfd/bfd_state_machine.h"
#include "bfd/bfd_common.h"

#include <boost/foreach.hpp>

#include "base/logging.h"
#include "io/event_manager.h"

namespace BFD {

Server::Server(EventManager *evm, Connection *communicator) :
        evm_(evm),
        communicator_(communicator),
        session_manager_(evm) {
    communicator->SetServer(this);
}

Session* Server::GetSession(const ControlPacket *packet) {
    if (packet->receiver_discriminator) {
        return session_manager_.SessionByDiscriminator(
                packet->receiver_discriminator);
    }
    return session_manager_.SessionByKey(packet->sender_host,
                                         packet->if_index);
}

Session *Server::SessionByKey(const boost::asio::ip::address &address,
        const SessionIndex index) {
    tbb::mutex::scoped_lock lock(mutex_);
    return session_manager_.SessionByKey(SessionKey(address, index));
}

ResultCode Server::ProcessControlPacket(
        boost::asio::ip::udp::endpoint local_endpoint,
        boost::asio::ip::udp::endpoint remote_endpoint,
        const boost::asio::const_buffer &recv_buffer,
        std::size_t bytes_transferred, const boost::system::error_code& error) {
    tbb::mutex::scoped_lock lock(mutex_);
    LOG(DEBUG, __func__);

    if (bytes_transferred != (std::size_t) kMinimalPacketLength) {
        LOG(ERROR, __func__ <<  "Wrong packet size: " << bytes_transferred);
        return kResultCode_InvalidPacket;
    }

    boost::scoped_ptr<ControlPacket> packet(ParseControlPacket(
        boost::asio::buffer_cast<const uint8_t *>(recv_buffer),
        bytes_transferred));
    if (packet == NULL) {
        LOG(ERROR, __func__ <<  "Unable to parse packet");
        return kResultCode_InvalidPacket;
    }

    packet->sender_host = remote_endpoint.address();
    packaet->bfd_port = local_endpoint.port();
    packet->if_index = 0;
    packet->vrf_index = 0;
    return ProcessControlPacket(packet.get());
}

ResultCode Server::ProcessControlPacket(const ControlPacket *packet) {
    ResultCode result;
    result = packet->Verify();
    if (result != kResultCode_Ok) {
        LOG(ERROR, "Wrong packet: " << result);
        return result;
    }
    Session *session = NULL;
    session = GetSession(packet);
    if (session == NULL) {
        LOG(ERROR, "Unknown session: " << packet->sender_host << "/"
                   << packet->receiver_discriminator);
        return kResultCode_UnknownSession;
    }
    LOG(DEBUG, "Found session: " << session->toString());
    result = session->ProcessControlPacket(packet);
    if (result != kResultCode_Ok) {
        LOG(ERROR, "Unable to process session: " << result);
        return result;
    }
    LOG(DEBUG, "Packet correctly processed");
    return kResultCode_Ok;
}

ResultCode Server::ConfigureSession(const boost::asio::ip::address &remoteHost,
                                    const SessionConfig &config,
                                    Discriminator *assignedDiscriminator) {
    tbb::mutex::scoped_lock lock(mutex_);
    return session_manager_.ConfigureSession(remoteHost, config,
                                             communicator_,
                                             assignedDiscriminator);
}

ResultCode Server::RemoveSessionReference(const SessoonKey &key) {
    tbb::mutex::scoped_lock lock(mutex_);
    return session_manager_.RemoveSessionReference(key);
}

Session* Server::SessionManager::SessionByDiscriminator(
    Discriminator discriminator) {
    DiscriminatorSessionMap::const_iterator it =
            by_discriminator_.find(discriminator);
    if (it == by_discriminator_.end())
        return NULL;
    return it->second;
}

Session* Server::SessionManager::SessionByKey(const SessionKey &key) {
    KeySessionMap::const_iterator it = by_key_.find(key);
    if (it == by_key_.end())
        return NULL;
    else
        return it->second;
}

ResultCode Server::SessionManager::RemoveSessionReference(
        const SessionKey &key) {
    Session *session = SessionByKey(key);
    if (session == NULL) {
        LOG(DEBUG, __PRETTY_FUNCTION__ << " No such session: " << key);
        return kResultCode_UnknownSession;
    }

    if (!--refcounts_[session]) {
        by_discriminator_.erase(session->local_discriminator());
        by_key_.erase(key);
        delete session;
    }

    return kResultCode_Ok;
}

ResultCode Server::SessionManager::ConfigureSession(const SessionKey &key,
        const SessionConfig &config, Connection *communicator,
        Discriminator *assignedDiscriminator) {
    Session *session = SessionByKey(key);
    if (session) {
        session->UpdateConfig(config);
        refcounts_[session]++;

        LOG(INFO, __func__ << ": Reference count incremented: "
                  << session->key() << "/"
                  << session->local_discriminator() << ","
                  << refcounts_[session] << " refs");

        return kResultCode_Ok;
    }

    *assignedDiscriminator = GenerateUniqueDiscriminator();
    session = new Session(*assignedDiscriminator, remoteHost, evm_, config,
                          communicator);

    by_discriminator_[*assignedDiscriminator] = session;
    by_key_[remoteHost] = session;
    refcounts_[session] = 1;

    LOG(INFO, __func__ << ": New session configured: " << remoteHost << "/"
              << *assignedDiscriminator);

    return kResultCode_Ok;
}

Discriminator Server::SessionManager::GenerateUniqueDiscriminator() {
    class DiscriminatorGenerator {
     public:
        DiscriminatorGenerator() {
            next_ = random()%0x1000000 + 1;
        }

        Discriminator Next() {
            return next_.fetch_and_increment();
        }

     private:
        tbb::atomic<Discriminator> next_;
    };

    static DiscriminatorGenerator generator;

    return generator.Next();
}

Server::SessionManager::~SessionManager() {
    for (DiscriminatorSessionMap::iterator it = by_discriminator_.begin();
         it != by_discriminator_.end(); ++it) {
        it->second->Stop();
        delete it->second;
    }
}
}  // namespace BFD
