/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#ifndef SRC_BFD_BFD_SESSION_H_
#define SRC_BFD_BFD_SESSION_H_

#include "bfd/bfd_common.h"
#include "bfd/bfd_state_machine.h"

#include <string>
#include <map>
#include <boost/scoped_ptr.hpp>
#include <boost/asio/ip/address.hpp>

#include "base/timer.h"
#include "tbb/mutex.h"
#include "io/event_manager.h"

namespace BFD {
class Connection;
class SessionConfig;
class ControlPacket;

struct SessionConfig {
    TimeInterval desiredMinTxInterval;  // delay
    TimeInterval requiredMinRxInterval; // timeout
    int detectionTimeMultiplier;        // max-retries ?
};

struct BFDRemoteSessionState {
    BFDRemoteSessionState() : discriminator(0),
        minRxInterval(boost::posix_time::seconds(0)),
        minTxInterval(boost::posix_time::seconds(0)),
        detectionTimeMultiplier(0),
        state(kInit) {}

    Discriminator discriminator;
    TimeInterval minRxInterval;
    TimeInterval minTxInterval;
    int detectionTimeMultiplier;
    BFDState state;
};

class Session {
 public:
    Session(Discriminator localDiscriminator,
            boost::asio::ip::address remoteHost,
            EventManager *evm,
            const SessionConfig &config,
            Connection *communicator);
    virtual ~Session();

    void Stop();
    ResultCode ProcessControlPacket(const ControlPacket *packet);
    void InitPollSequence();
    void RegisterChangeCallback(ClientId client_id,
                                StateMachine::ChangeCb cb);
    void UnregisterChangeCallback(ClientId client_id);
    void UpdateConfig(const SessionConfig& config);

    std::string               toString() const;
    boost::asio::ip::address  remote_host() const;
    BFDState                  local_state() const;
    SessionConfig             config() const;
    BFDRemoteSessionState     remote_state() const;
    Discriminator             local_discriminator() const;
    bool                      Up() const;

    TimeInterval detection_time();
    TimeInterval tx_interval();

    // Yields number of registered callbacks.
    // Server::SessionManager will delete a Session instance if its
    // reference count drops to zero.
    int reference_count();

 protected:
    bool RecvTimerExpired();

 private:
    typedef std::map<ClientId, StateMachine::ChangeCb> Callbacks;

    bool SendTimerExpired();
    void ScheduleSendTimer();
    void ScheduleRecvDeadlineTimer();
    void PreparePacket(const SessionConfig &config, ControlPacket *packet);
    void SendPacket(const ControlPacket *packet);
    void CallStateChangeCallbacks(const BFD::BFDState &new_state);

    BFDState local_state_non_locking() const;

    mutable tbb::mutex       mutex_;
    Discriminator            localDiscriminator_;
    boost::asio::ip::address remoteHost_;
    Timer                    *sendTimer_;
    Timer                    *recvTimer_;
    SessionConfig            currentConfig_;
    SessionConfig            nextConfig_;
    BFDRemoteSessionState    remoteSession_;
    boost::scoped_ptr<StateMachine> sm_;
    bool                     pollSequence_;
    Connection               *communicator_;
    bool                     stopped_;
    Callbacks                callbacks_;
};

}  // namespace BFD

#endif  // SRC_BFD_BFD_SESSION_H_
