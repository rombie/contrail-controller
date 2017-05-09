/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */
#ifndef SRC_BFD_CLIENT_H_
#define SRC_BFD_CLIENT_H_

#include "bfd/bfd_common.h"

namespace BFD {

class Connection;
class Session;
class SessionConfig;

class Client {
public:
    Client(Connection *cm, ClientId client_id = 0);
    virtual ~Client();
    ResultCode AddConnection(const boost::asio::ip::address &remoteHost,
                             const SessionConfig &config, uint32_t index = 0,
                             bool multi_hop = false);
    ResultCode DeleteConnection(const boost::asio::ip::address &remoteHost,
                                uint32_t index = 0);
    bool Up(const boost::asio::ip::address& ip, uint32_t index = 0) const;

private:
    void Notify(const BFD::BFDState &new_state, Session *session);
    Session *GetSession(const boost::asio::ip::address& ip,
                        const SessionIndex index = 0) const;

    ClientId id_;
    Connection *cm_;
    typedef std::set<SessionKey> Sessions;
    Sessions bfd_sessions_;
};

}  // namespace BFD

#endif  // SRC_BFD_CLIENT_H_
