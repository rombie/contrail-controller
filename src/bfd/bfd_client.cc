/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/random.hpp>
#include <set>

#include "base/logging.h"
#include "bfd/bfd_client.h"
#include "bfd/bfd_common.h"
#include "bfd/bfd_connection.h"
#include "bfd/bfd_server.h"
#include "bfd/bfd_session.h"

using namespace BFD;
using boost::bind;

Client::Client(Connection *cm, ClientId id) : id_(id), cm_(cm) {
}

Client::~Client() {
    for (Sessions::iterator it = bfd_sessions_.begin();
         it != bfd_sessions_.end(); ++it) {
        DeleteConnection(*it);
    }
}

Session *Client::GetSession(const boost::asio::ip::address& ip) const {
    if (bfd_sessions_.find(ip) == bfd_sessions_.end())
        return NULL;
    return cm_->GetServer()->SessionByAddress(ip);
}

bool Client::Up(const boost::asio::ip::address& ip) const {
    Session *session = GetSession(ip);
    return session && session->Up();
}

ResultCode Client::AddConnection(
    const boost::asio::ip::address& remoteHost, const SessionConfig &config) {
    if (bfd_sessions_.find(remoteHost) != bfd_sessions_.end()) {
        // TODO(bfd) implement configuration update
        return kResultCode_Error;
    }

    Discriminator discriminator;
    ResultCode result =
      cm_->GetServer()->ConfigureSession(remoteHost, config, &discriminator);
    bfd_sessions_.insert(remoteHost);

    Session *session = GetSession(remoteHost);
    if (!session)
      return kResultCode_Error;
    session->RegisterChangeCallback(id_, bind(&Client::Notify, this, _1,
                                              session));
    Notify(session->local_state(), session);
    return result;
}

void Client::Notify(const BFD::BFDState &new_state, Session *session) {
    cm_->NotifyStateChange(session->remote_host(), new_state == kUp);
}

ResultCode Client::DeleteConnection(
    const boost::asio::ip::address& remoteHost) {
    if (bfd_sessions_.find(remoteHost) == bfd_sessions_.end()) {
        return kResultCode_UnknownSession;
    }
    ResultCode result = cm_->GetServer()->RemoveSessionReference(remoteHost);
    bfd_sessions_.erase(remoteHost);
    return result;
}
