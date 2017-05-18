/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#ifndef SRC_BFD_BFD_SERVER_H_
#define SRC_BFD_BFD_SERVER_H_

#include "base/queue_task.h"
#include "bfd/bfd_common.h"

#include <tbb/mutex.h>

#include <map>
#include <set>
#include <boost/asio.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/scoped_ptr.hpp>

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
        const SessionIndex &session_index,
        const boost::asio::const_buffer &recv_buffer,
        std::size_t bytes_transferred, const boost::system::error_code& error);

    // If a BFD session with specified [remoteHost] already exists, its
    // configuration is updated with [config], otherwise it gets created.
    // ! TODO implement configuration update
    ResultCode ConfigureSession(const SessionKey &key,
                                const SessionConfig &config,
                                Discriminator *assignedDiscriminator);

    // Instances of BFD::Session are removed after last IP address
    // reference is gone.
    ResultCode RemoveSessionReference(const SessionKey &key);
    Session *SessionByKey(const boost::asio::ip::address &address,
                          const SessionIndex &index = SessionIndex());
    Session *SessionByKey(const SessionKey &key);
    Session *SessionByKey(const SessionKey &key) const;
    Connection *communicator() const { return communicator_; }
    void AddConnection(const SessionKey &key, const SessionConfig &config,
                       ChangeCb cb);
    void DeleteConnection(const SessionKey &key);
    void DeleteClientConnections(const ClientId client_id);

 private:
    class SessionManager : boost::noncopyable {
     public:
        explicit SessionManager(EventManager *evm) : evm_(evm) {}
        ~SessionManager();

        ResultCode ConfigureSession(const SessionKey &key,
                                    const SessionConfig &config,
                                    Connection *communicator,
                                    Discriminator *assignedDiscriminator);

        // see: Server:RemoveSessionReference
        ResultCode RemoveSessionReference(const SessionKey &key);

        Session *SessionByDiscriminator(Discriminator discriminator);
        Session *SessionByKey(const SessionKey &key);
        Session *SessionByKey(const SessionKey &key) const;

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

    enum EventType {
        BEGIN_EVENT,
        ADD_CONNECTION = BEGIN_EVENT,
        DELETE_CONNECTION,
        DELETE_CLIENT_CONNECTIONS,
        PROCESS_PACKET,
        END_EVENT = PROCESS_PACKET,
    };

    struct Event {
        Event(EventType type, const SessionKey &key,
              const SessionConfig &config, ChangeCb cb) :
                type(type), key(key), config(config), cb(cb) {
        }
        Event(EventType type, const SessionKey &key) :
                type(type), key(key) {
        }
        Event(EventType type, const ControlPacket *packet) :
                type(type), packet(packet) {
        }

        EventType type;
        SessionKey key;
        SessionConfig config;
        ChangeCb cb;
        const ControlPacket *packet;
    };

    void AddConnection(Event *event);
    void DeleteConnection(Event *event);
    void DeleteClientConnections(Event *event);
    void EnqueueEvent(Event *event);
    bool EventCallback(Event *event);

    Session *GetSession(const ControlPacket *packet);

    mutable tbb::mutex mutex_;
    EventManager *evm_;
    Connection *communicator_;
    SessionManager session_manager_;
    boost::scoped_ptr<WorkQueue<Event *> > event_queue_;
    typedef std::set<SessionKey> Sessions;
    Sessions sessions_;
};

}  // namespace BFD

#endif  // SRC_BFD_BFD_SERVER_H_
