/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#ifndef SRC_BFD_BFD_SERVER_H_
#define SRC_BFD_BFD_SERVER_H_

#include "bfd/bfd_common.h"

#include <tbb/mutex.h>

#include <map>
#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>

class EventManager;

namespace BFD {
class Connection;
class Session;
class ControlPacket;
class SessionConfig;

// This class manages sessions with other BFD peers.
class Server {
 public:
    Server(EventManager *evm, Connection *communicator);
    ResultCode ProcessControlPacket(const ControlPacket *packet);
    ResultCode ProcessControlPacket(
        boost::asio::ip::udp::endpoint local_endpoint,
        boost::asio::ip::udp::endpoint remote_endpoint,
        const boost::asio::const_buffer &recv_buffer,
        std::size_t bytes_transferred, const boost::system::error_code& error);

    // If a BFD session with specified [remoteHost] already exists, its
    // configuration is updated with [config], otherwise it gets created.
    // ! TODO implement configuration update
    ResultCode ConfigureSession(const boost::asio::ip::address &remoteHost,
                                const SessionConfig &config,
                                Discriminator *assignedDiscriminator);

    // Instances of BFD::Session are removed after last IP address
    // reference is gone.
    ResultCode RemoveSessionReference(const SessionKey &key);
    Session *SessionByKey(const boost::asio::ip::address &address,
                          const SessionIndex index = 0);
    Connection *communicator() const { return communicator_; }

 private:
    class SessionManager : boost::noncopyable {
     public:
        explicit SessionManager(EventManager *evm) : evm_(evm) {}
        ~SessionManager();

        // see: Server::ConfigureSession
        ResultCode ConfigureSession(const boost::asio::ip::address
                                    &remoteHost,
                                    const SessionConfig &config,
                                    Connection *communicator,
                                    Discriminator
                                    *assignedDiscriminator);

        // see: Server:RemoveSessionReference
        ResultCode RemoveSessionReference(const SessionKey &key);

        Session *SessionByDiscriminator(Discriminator discriminator);
        Session *SessionByKey(const boost::asio::ip::address &address,
                              SessionIndex index = 0);

     private:
        typedef std::map<Discriminator, Session *> DiscriminatorSessionMap;
        typedef std::map<SessionKey, Session *> KeySessionMap;
        typedef std::map<Session *, unsigned int> RefcountMap;

        Discriminator GenerateUniqueDiscriminator();

        EventManager *evm_;
        DiscriminatorSessionMap by_discriminator_;
        KeySessionMap by_key_;
        RefcountMap refcounts_;
    };

    Session *GetSession(const ControlPacket *packet);

    tbb::mutex mutex_;
    EventManager *evm_;
    Connection *communicator_;
    SessionManager session_manager_;
};

}  // namespace BFD

#endif  // SRC_BFD_BFD_SERVER_H_
