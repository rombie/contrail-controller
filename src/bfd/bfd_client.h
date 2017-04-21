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
                             const SessionConfig &config);
    ResultCode DeleteConnection(const boost::asio::ip::address &remoteHost);

private:
    void Notify(const BFD::BFDState &new_state, Session *session);
    Session *GetSession(const boost::asio::ip::address& ip) const;

    ClientId id_;
    Connection *cm_;
    typedef std::set<boost::asio::ip::address> Sessions;
    Sessions bfd_sessions_;
};

}  // namespace BFD

#endif  // SRC_BFD_CLIENT_H_
