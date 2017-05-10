/*
 * Copyright (c) 2014 CodiLime, Inc. All rights reserved.
 */
#ifndef SRC_BFD_BFD_COMMON_H_
#define SRC_BFD_BFD_COMMON_H_

#include <ostream>
#include <string>
#include <boost/asio.hpp>
#include <boost/optional.hpp>
#include <boost/date_time.hpp>
#include <boost/random/taus88.hpp>

namespace BFD {
typedef uint32_t Discriminator;
typedef boost::posix_time::time_duration TimeInterval;
typedef uint32_t ClientId;

enum BFDState {
    kAdminDown, kDown, kInit, kUp
};

enum Port {
    kSingleHop = 3784,
    kMultiHop = 4784
};

enum AuthType {
    kReserved,
    kSimplePassword,
    kKeyedMD5,
    kMeticulousKeyedMD5,
    kKeyedSHA1,
    kMeticulousKeyedSHA1,
};

enum ResultCode {
    kResultCode_Ok,
    kResultCode_UnknownSession,
    kResultCode_Error,
    kResultCode_InvalidPacket,
    kResultCode_NotImplemented,
};

enum Diagnostic {
    kNoDiagnostic,
    kControlDetectionTimeExpired,
    kEchoFunctionFailed,
    kNeighborSignaledSessionDown,
    kForwardingPlaneReset,
    kPathDown,
    kConcatenatedPathDown,
    kAdministrativelyDown,
    kReverseConcatenatedPathDown,
    kDiagnosticFirstInvalid
};

std::ostream &operator<<(std::ostream &, enum BFDState);
boost::optional<BFDState> BFDStateFromString(const char *);
typedef uint32_t SessionIndex; // IFIndex or VrfIndex
struct SessionKey {
public:
    SessionKey(const boost::asio::ip::address &local_address,
            const boost::asio::ip::address &remote_address,
            SessionIndex index, uint16_t remote_port) :
            local_address(local_address), remote_address(remote_address),
            index(index), remote_port(remote_port) {
    }

    SessionKey(const boost::asio::ip::address &remote_address) :
        remote_address(remote_address), index(0) {
    }

    SessionKey(const boost::asio::ip::address &local_address,
               const boost::asio::ip::address &remote_address) :
        local_address(local_address), remote_address(remote_address), index(0) {
    }

    const std::string to_string() const {
        std::ostringstream os;
        os << remote_address << " " << index;
        return os.str();
    }

    boost::asio::ip::address local_address;
    boost::asio::ip::address remote_address;
    SessionIndex index; // InterfaceIndex or VrfIndex
    uint16_t remote_port;
};

extern const int kMinimalPacketLength;
extern const TimeInterval kIdleTxInterval;
extern boost::random::taus88 randomGen;
}  // namespace BFD

#endif  // SRC_BFD_BFD_COMMON_H_
