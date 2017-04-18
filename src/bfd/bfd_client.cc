
using namespace BFD;
using boost::bind;

Client::Client(Server *server, ClientId client_id) :
    client_id_(client_id), server_(server), cb_(cb), changed_(true) {
}

ResultCode Client::AddConnection(
    const boost::asio::ip::address& remoteHost, const SessionConfig &config) {
    if (bfd_sessions_.find(remoteHost) != bfd_sessions_.end()) {
        // TODO(bfd) implement configuration update
        return kResultCode_Error;
    }

    Discriminator discriminator;
    ResultCode result =
      server_->ConfigureSession(remoteHost, config, &discriminator);
    bfd_sessions_.insert(remoteHost);

    Session *session = GetSession(remoteHost);
    if (NULL == session)
      return kResultCode_Error;
    session->RegisterChangeCallback(client_id_, bind(&Client::Notify, this));
    Notify();
    return result;
}

void Client::Notify() {
    LOG(DEBUG, "Notify: " << client_id_);
    if (!bfd_sessions_.empty() && http_sessions_.empty()) {
        changed_ = true;
        return;
    }
    changed_ = false;

    if (http_sessions_.empty())
        return;

    REST::JsonStateMap map;
    for (Sessions::iterator it = bfd_sessions_.begin();
         it != bfd_sessions_.end(); ++it) {
        Session *session = GetSession(*it);
        map.states[session->remote_host()] = session->local_state();
    }

    std::string json;
    map.EncodeJsonString(&json);
    for (HttpSessionSet::iterator it = http_sessions_.begin();
         it != http_sessions_.end(); ++it) {
        if (false == it->get()->IsClosed()) {
            LOG(DEBUG, "Notify: " << client_id_ << " Send notification to "
              << it->get()->ToString());
            REST::SendResponse(it->get(), json);
        }
    }
    http_sessions_.clear();
}
