/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */

#include "bfd/bfd_session.h"
#include "bfd/bfd_control_packet.h"
#include "bfd/bfd_common.h"
#include "bfd/bfd_connection.h"

#include <tbb/mutex.h>
#include <boost/asio.hpp>
#include <boost/random.hpp>
#include <string>
#include <algorithm>

#include "base/logging.h"

namespace BFD {

Session::Session(Discriminator localDiscriminator,
        const SessionKey &key,
        EventManager *evm,
        const SessionConfig &config, Connection *communicator) :
        localDiscriminator_(localDiscriminator),
        key_(key),
        sendTimer_(TimerManager::CreateTimer(*evm->io_service(),
            "BFD TX", TaskScheduler::GetInstance()->GetTaskId("BFD"), 0)),
        recvTimer_(TimerManager::CreateTimer(*evm->io_service(),
            "BFD RX", TaskScheduler::GetInstance()->GetTaskId("BFD"), 0)),
        currentConfig_(config),
        nextConfig_(config),
        sm_(CreateStateMachine(evm, this)),
        pollSequence_(false),
        communicator_(communicator),
        local_endpoint_(key.local_address, GetRandomLocalPort()),
        remote_endpoint_(key.remote_address, key.remote_port),
        stopped_(false) {
    ScheduleSendTimer();
    ScheduleRecvDeadlineTimer();
    sm_->SetCallback(boost::optional<ChangeCb>(
        boost::bind(&Session::CallStateChangeCallbacks, this, _1, _2)));
}

uint16_t Session::GetRandomLocalPort() const {
    boost::random::uniform_int_distribution<> dist(kSendPortMin, kSendPortMax);
    return dist(randomGen);
}

Session::~Session() {
    Stop();
}

bool Session::SendTimerExpired() {
    LOG(DEBUG, __func__);
    tbb::mutex::scoped_lock lock(mutex_);

    ControlPacket packet;
    PreparePacket(nextConfig_, &packet);
    SendPacket(&packet);

    // Workaround: Timer code isn't re-entrant
    this->sendTimer_->Reschedule(tx_interval().total_milliseconds());
    return true;
}

bool Session::RecvTimerExpired() {
    LOG(DEBUG, __func__);
    tbb::mutex::scoped_lock lock(mutex_);
    sm_->ProcessTimeout();

    return false;
}

std::string Session::toString() const {
    std::ostringstream out;
    out << "SessoonKey: " << key_.to_string() << "\n";
    out << "LocalDiscriminator: 0x" << std::hex << localDiscriminator_ << "\n";
    out << "RemoteDiscriminator: 0x" << std::hex << remoteSession_.discriminator
        << "\n";
    out << "DesiredMinTxInterval: " << currentConfig_.desiredMinTxInterval
        << "\n";
    out << "RequiredMinRxInterval: " << currentConfig_.requiredMinRxInterval
        << "\n";
    out << "RemoteMinRxInterval: " << remoteSession_.minRxInterval << "\n";

    return out.str();
}

void Session::ScheduleSendTimer() {
    TimeInterval ti = tx_interval();
    LOG(DEBUG, __func__ << " " << ti);

    sendTimer_->Start(ti.total_milliseconds(),
                      boost::bind(&Session::SendTimerExpired, this));
}

void Session::ScheduleRecvDeadlineTimer() {
    TimeInterval ti = detection_time();
    LOG(DEBUG, __func__ << ti);

    recvTimer_->Cancel();
    recvTimer_->Start(ti.total_milliseconds(),
                      boost::bind(&Session::RecvTimerExpired, this));
}

BFDState Session::local_state_non_locking() const {
    return sm_->GetState();
}

BFDState Session::local_state() const {
    tbb::mutex::scoped_lock lock(mutex_);

    return local_state_non_locking();
}

//  If periodic BFD Control packets are already being sent (the remote
//  system is not in Demand mode), the Poll Sequence MUST be performed by
//  setting the Poll (P) bit on those scheduled periodic transmissions;
//  additional packets MUST NOT be sent.
void Session::InitPollSequence() {
    tbb::mutex::scoped_lock lock(mutex_);

    pollSequence_ = true;
    if (local_state_non_locking() != kUp &&
        local_state_non_locking() != kAdminDown) {
        ControlPacket packet;
        PreparePacket(nextConfig_, &packet);
        SendPacket(&packet);
    }
}

void Session::PreparePacket(const SessionConfig &config,
                            ControlPacket *packet) {

    packet->state = local_state_non_locking();
    packet->poll = pollSequence_;
    packet->sender_discriminator = localDiscriminator_;
    packet->receiver_discriminator = remoteSession_.discriminator;
    packet->detection_time_multiplier = config.detectionTimeMultiplier;
    packet->desired_min_tx_interval = config.desiredMinTxInterval;
    packet->required_min_rx_interval = config.requiredMinRxInterval;
}

ResultCode Session::ProcessControlPacket(const ControlPacket *packet) {
    tbb::mutex::scoped_lock lock(mutex_);

    remoteSession_.discriminator = packet->sender_discriminator;
    if (remoteSession_.minRxInterval != packet->required_min_rx_interval) {
        // TODO(bfd) schedule timer based on previous packet
        ScheduleSendTimer();
        remoteSession_.minRxInterval = packet->required_min_rx_interval;
    }
    remoteSession_.minTxInterval = packet->desired_min_tx_interval;
    remoteSession_.detectionTimeMultiplier = packet->detection_time_multiplier;
    remoteSession_.state = packet->state;

    sm_->ProcessRemoteState(packet->state);

    // poll sequence
    if (packet->poll) {
        ControlPacket newPacket;
        PreparePacket(nextConfig_, &newPacket);
        newPacket.poll = false;  // poll & final are forbidden in single packet
        newPacket.final = true;
        SendPacket(&newPacket);
    }
    if (packet->final) {
        pollSequence_ = false;
        currentConfig_ = nextConfig_;
    }

    if (local_state_non_locking() == kUp ||
        local_state_non_locking() == kInit) {
        ScheduleRecvDeadlineTimer();
    }

    return kResultCode_Ok;
}

void Session::SendPacket(const ControlPacket *packet) {
    LOG(DEBUG, __func__);
    boost::asio::mutable_buffer buffer =
        boost::asio::mutable_buffer(new u_int8_t[kMinimalPacketLength],
                                    kMinimalPacketLength);
    int pktSize = EncodeControlPacket(packet,
        boost::asio::buffer_cast<uint8_t *>(buffer), kMinimalPacketLength);
    if (pktSize != kMinimalPacketLength) {
        LOG(ERROR, "Unable to encode packet");
    } else {
        communicator_->SendPacket(local_endpoint_, remote_endpoint_,
                                  key_.index, buffer, pktSize);
    }
}

TimeInterval Session::detection_time() {
    return std::max(currentConfig_.requiredMinRxInterval,
                    remoteSession_.minTxInterval) *
            remoteSession_.detectionTimeMultiplier;
}

TimeInterval Session::tx_interval() {
    TimeInterval minInterval, maxInterval;

    if (local_state_non_locking() == kUp) {
        TimeInterval negotiatedInterval =
                std::max(currentConfig_.desiredMinTxInterval,
                         remoteSession_.minRxInterval);

        minInterval = negotiatedInterval * 3/4;
        if (currentConfig_.detectionTimeMultiplier == 1)
            maxInterval = negotiatedInterval * 9/10;
        else
            maxInterval = negotiatedInterval;
    } else {
        minInterval = kIdleTxInterval * 3/4;
        maxInterval = kIdleTxInterval;
    }
    boost::random::uniform_int_distribution<>
            dist(minInterval.total_microseconds(),
                 maxInterval.total_microseconds());
    return boost::posix_time::microseconds(dist(randomGen));
}

const SessionKey &Session::key() const {
    tbb::mutex::scoped_lock lock(mutex_);
    return key_;
}

void Session::Stop() {
    tbb::mutex::scoped_lock lock(mutex_);

    if (stopped_ == false) {
        TimerManager::DeleteTimer(sendTimer_);
        TimerManager::DeleteTimer(recvTimer_);
        stopped_ = true;
        sm_->SetCallback(boost::optional<ChangeCb>());
    }
}

SessionConfig Session::config() const {
    tbb::mutex::scoped_lock lock(mutex_);
    return nextConfig_;
}

BFDRemoteSessionState Session::remote_state() const {
    tbb::mutex::scoped_lock lock(mutex_);
    return remoteSession_;
}

Discriminator Session::local_discriminator() const {
    tbb::mutex::scoped_lock lock(mutex_);
    return localDiscriminator_;
}

void Session::CallStateChangeCallbacks(
    const SessionKey &key, const BFD::BFDState &new_state) {
    for (Callbacks::const_iterator it = callbacks_.begin();
         it != callbacks_.end(); ++it) {
        it->second(key, new_state);
    }
}

void Session::RegisterChangeCallback(ClientId client_id, ChangeCb cb) {
    tbb::mutex::scoped_lock lock(mutex_);
    callbacks_[client_id] = cb;
}

void Session::UnregisterChangeCallback(ClientId client_id) {
    tbb::mutex::scoped_lock lock(mutex_);
    callbacks_.erase(client_id);
}

void Session::UpdateConfig(const SessionConfig& config) {
    // TODO(bfd) implement UpdateConfig
    LOG(ERROR, "Session::UpdateConfig not implemented");
}

int Session::reference_count() {
    tbb::mutex::scoped_lock lock(mutex_);
    return callbacks_.size();
}

bool Session::Up() const {
    return local_state() == kUp;
}

}  // namespace BFD
