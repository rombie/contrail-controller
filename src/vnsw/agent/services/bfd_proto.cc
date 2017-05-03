/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "sandesh/sandesh_types.h"
#include "sandesh/sandesh.h"
#include "net/address_util.h"
#include "init/agent_init.h"
#include "oper/nexthop.h"
#include "oper/tunnel_nh.h"
#include "oper/mirror_table.h"
#include "oper/route_common.h"
#include "pkt/pkt_init.h"
#include "services/bfd_proto.h"
#include "services/services_sandesh.h"
#include "services_init.h"

BfdProto::BfdProto(Agent *agent, boost::asio::io_service &io,
                   bool run_with_vrouter) :
    Proto(agent, "Agent::Services", PktHandler::BFD, io),
    run_with_vrouter_(run_with_vrouter), ip_fabric_interface_index_(-1),
    ip_fabric_interface_(NULL), max_retries_(kMaxRetries),
    retry_timeout_(kRetryTimeout), aging_timeout_(kAgingTimeout) {
    // limit the number of entries in the workqueue
    work_queue_.SetSize(agent->params()->services_queue_limit());
    work_queue_.SetBounded(true);

    vrf_table_listener_id_ = agent->vrf_table()->Register(
                             boost::bind(&BfdProto::VrfNotify, this, _1, _2));
    interface_table_listener_id_ = agent->interface_table()->Register(
                                   boost::bind(&BfdProto::InterfaceNotify,
                                               this, _2));
    nexthop_table_listener_id_ = agent->nexthop_table()->Register(
                                 boost::bind(&BfdProto::NextHopNotify, this, _2));
}

BfdProto::~BfdProto() {
}

void BfdProto::Shutdown() {
    // we may have bfd entries in bfd cache without BfdNH, empty them
    for (BfdIterator it = bfd_cache_.begin(); it != bfd_cache_.end(); ) {
        it = DeleteBfdEntry(it);
    }

    for (GratuitousBfdIterator it = gratuitous_bfd_cache_.begin();
            it != gratuitous_bfd_cache_.end(); it++) {
        for (BfdEntrySet::iterator sit = it->second.begin();
             sit != it->second.end();) {
            BfdEntry *entry = *sit;
            it->second.erase(sit++);
            delete entry;
        }
    }
    gratuitous_bfd_cache_.clear();
    agent_->vrf_table()->Unregister(vrf_table_listener_id_);
    agent_->interface_table()->Unregister(interface_table_listener_id_);
    agent_->nexthop_table()->Unregister(nexthop_table_listener_id_);
}

ProtoHandler *BfdProto::AllocProtoHandler(boost::shared_ptr<PktInfo> info,
                                          boost::asio::io_service &io) {
    return new BfdHandler(agent(), info, io);
}

void BfdProto::VrfNotify(DBTablePartBase *part, DBEntryBase *entry) {
    VrfEntry *vrf = static_cast<VrfEntry *>(entry);
    BfdVrfState *state;

    state = static_cast<BfdVrfState *>(entry->GetState(part->parent(),
                                                   vrf_table_listener_id_));
    if (entry->IsDeleted()) {
        if (state) {
            for (BfdProto::BfdIterator it = bfd_cache_.begin();
                 it != bfd_cache_.end();) {
                BfdEntry *bfd_entry = it->second;
                if (bfd_entry->key().vrf == vrf && bfd_entry->DeleteBfdRoute()) {
                    it = DeleteBfdEntry(it);
                } else
                    it++;
            }
            state->Delete();
        }
        return;
    }

    if (!state){
        state = new BfdVrfState(agent_, this, vrf,
                                vrf->GetInet4UnicastRouteTable(),
                                vrf->GetEvpnRouteTable());
        state->route_table_listener_id = vrf->
            GetInet4UnicastRouteTable()->
            Register(boost::bind(&BfdVrfState::RouteUpdate, state,  _1, _2));
        state->evpn_route_table_listener_id = vrf->GetEvpnRouteTable()->
            Register(boost::bind(&BfdVrfState::EvpnRouteUpdate, state,  _1, _2));
        entry->SetState(part->parent(), vrf_table_listener_id_, state);
    }
}

void intrusive_ptr_add_ref(BfdPathPreferenceState *aps) {
    aps->refcount_.fetch_and_increment();
}

void intrusive_ptr_release(BfdPathPreferenceState *aps) {
    BfdVrfState *state = aps->vrf_state();
    int prev = aps->refcount_.fetch_and_decrement();
    if (prev == 1) {
        state->Erase(aps->ip());
        delete aps;
    }
}

BfdPathPreferenceState::BfdPathPreferenceState(BfdVrfState *state,
                                               uint32_t vrf_id,
                                               const IpAddress &ip,
                                               uint8_t plen):
    vrf_state_(state), bfd_req_timer_(NULL), vrf_id_(vrf_id),
    vm_ip_(ip), plen_(plen) {
    refcount_ = 0;
}

BfdPathPreferenceState::~BfdPathPreferenceState() {
    if (bfd_req_timer_) {
        bfd_req_timer_->Cancel();
        TimerManager::DeleteTimer(bfd_req_timer_);
    }
    assert(refcount_ == 0);
}

void BfdPathPreferenceState::StartTimer() {
    if (bfd_req_timer_ == NULL) {
        bfd_req_timer_ = TimerManager::CreateTimer(
                *(vrf_state_->agent->event_manager()->io_service()),
                "Bfd Entry timer for VM",
                TaskScheduler::GetInstance()->
                GetTaskId("Agent::Services"), PktHandler::BFD);
    }
    bfd_req_timer_->Start(kTimeout,
                          boost::bind(&BfdPathPreferenceState::SendBfdRequest,
                                      this));
}

bool BfdPathPreferenceState::SendBfdRequest(WaitForTrafficIntfMap
                                            &wait_for_traffic_map,
                                            BfdTransmittedIntfMap
                                            &bfd_transmitted_map) {
    bool ret = false;
    boost::shared_ptr<PktInfo> pkt(new PktInfo(vrf_state_->agent,
                                               BFD_TX_BUFF_LEN,
                                               PktHandler::BFD, 0));
    BfdHandler bfd_handler(vrf_state_->agent, pkt,
            *(vrf_state_->agent->event_manager()->io_service()));

    WaitForTrafficIntfMap::iterator it = wait_for_traffic_map.begin();
    for (;it != wait_for_traffic_map.end(); it++) {
        const VmInterface *vm_intf = static_cast<const VmInterface *>(
                vrf_state_->agent->interface_table()->FindInterface(it->first));
        if (!vm_intf) {
            continue;
        }

        bool inserted = bfd_transmitted_map.insert(it->first).second;
        MacAddress smac = vm_intf->GetVifMac(vrf_state_->agent);
        it->second++;
        if (inserted == false) {
            //BFD request already sent due to IP route
            continue;
        }
        bfd_handler.SendBfd(BFDOP_REQUEST, smac,
                            gw_ip_.to_v4().to_ulong(),
                            MacAddress(), MacAddress::BroadcastMac(),
                            vm_ip_.to_v4().to_ulong(), it->first, vrf_id_);
        vrf_state_->bfd_proto->IncrementStatsVmBfdReq();

        // reduce the frequency of BFD requests after some tries
        if (it->second >= kMaxRetry) {
            // change frequency only if not in gateway mode with remote VMIs
            if (vm_intf->vmi_type() != VmInterface::REMOTE_VM)
                bfd_req_timer_->Reschedule(kTimeout * 5);
        }

        ret = true;
    }
    return ret;
}

bool BfdPathPreferenceState::SendBfdRequest() {
    if (l3_wait_for_traffic_map_.size() == 0 &&
        evpn_wait_for_traffic_map_.size() == 0) {
        return false;
    }

    bool ret = false;
    BfdTransmittedIntfMap bfd_transmitted_map;
    if (SendBfdRequest(l3_wait_for_traffic_map_, bfd_transmitted_map)) {
        ret = true;
    }

    if (SendBfdRequest(evpn_wait_for_traffic_map_, bfd_transmitted_map)) {
        ret = true;
    }

    return ret;
}

//Send BFD request on interface in Active-BackUp mode
//So that preference of route can be incremented if the VM replies to BFD
void BfdPathPreferenceState::SendBfdRequestForAllIntf(const
                                                      AgentRoute *route) {
    WaitForTrafficIntfMap new_wait_for_traffic_map;
    WaitForTrafficIntfMap wait_for_traffic_map = evpn_wait_for_traffic_map_;
    if (dynamic_cast<const InetUnicastRouteEntry *>(route)) {
        wait_for_traffic_map = l3_wait_for_traffic_map_;
    }

    for (Route::PathList::const_iterator it = route->GetPathList().begin();
            it != route->GetPathList().end(); it++) {
        const AgentPath *path = static_cast<const AgentPath *>(it.operator->());
        if (path->peer() &&
            path->peer()->GetType() == Peer::LOCAL_VM_PORT_PEER) {
            const NextHop *nh = path->ComputeNextHop(vrf_state_->agent);
            if (nh->GetType() != NextHop::INTERFACE) {
                continue;
            }
            if (path->is_health_check_service()) {
                // skip sending BFD request for Health Check Service IP
                continue;
            }

            const InterfaceNH *intf_nh =
                static_cast<const  InterfaceNH *>(nh);
            const Interface *intf =
                static_cast<const Interface *>(intf_nh->GetInterface());
            if (intf->type() != Interface::VM_INTERFACE) {
                //Ignore non vm interface nexthop
                continue;
            }
            if (path->subnet_service_ip().is_v4() == false) {
                continue;
            }
            if (dynamic_cast<const InetUnicastRouteEntry *>(route)) {
                gw_ip_ = path->subnet_service_ip();
            }
            if (path->path_preference().IsDependentRt() == true) {
                continue;
            }
            uint32_t intf_id = intf->id();
            WaitForTrafficIntfMap::const_iterator wait_for_traffic_it =
                wait_for_traffic_map.find(intf_id);
            if (wait_for_traffic_it == wait_for_traffic_map.end()) {
                new_wait_for_traffic_map.insert(std::make_pair(intf_id, 0));
            } else {
                new_wait_for_traffic_map.insert(std::make_pair(intf_id,
                    wait_for_traffic_it->second));
            }
        }
    }

    if (dynamic_cast<const InetUnicastRouteEntry *>(route)) {
        l3_wait_for_traffic_map_ = new_wait_for_traffic_map;
    } else {
        evpn_wait_for_traffic_map_ = new_wait_for_traffic_map;
    }
    if (new_wait_for_traffic_map.size() > 0) {
        SendBfdRequest();
        StartTimer();
    }
}

BfdDBState::BfdDBState(BfdVrfState *vrf_state, uint32_t vrf_id, IpAddress ip,
                       uint8_t plen) : vrf_state_(vrf_state),
    sg_list_(0), policy_(false), resolve_route_(false) {
    if (plen == Address::kMaxV4PrefixLen && ip != Ip4Address(0)) {
       bfd_path_preference_state_.reset(vrf_state->Locate(ip));
    }
}

BfdDBState::~BfdDBState() {
}

void BfdDBState::UpdateBfdRoutes(const InetUnicastRouteEntry *rt) {
    int plen = rt->plen();
    uint32_t start_ip = rt->addr().to_v4().to_ulong();
    BfdKey start_key(start_ip, rt->vrf());

    BfdProto::BfdIterator start_iter =
        vrf_state_->bfd_proto->FindUpperBoundBfdEntry(start_key);


    while (start_iter != vrf_state_->bfd_proto->bfd_cache().end() &&
           start_iter->first.vrf == rt->vrf() &&
           IsIp4SubnetMember(Ip4Address(start_iter->first.ip),
                             rt->addr().to_v4(), plen)) {
        start_iter->second->Resync(policy_, vn_list_, sg_list_);
        start_iter++;
    }
}

void BfdDBState::Delete(const InetUnicastRouteEntry *rt) {
    int plen = rt->plen();
    uint32_t start_ip = rt->addr().to_v4().to_ulong();

    BfdKey start_key(start_ip, rt->vrf());

    BfdProto::BfdIterator start_iter =
        vrf_state_->bfd_proto->FindUpperBoundBfdEntry(start_key);

    while (start_iter != vrf_state_->bfd_proto->bfd_cache().end() &&
           start_iter->first.vrf == rt->vrf() &&
           IsIp4SubnetMember(Ip4Address(start_iter->first.ip),
                             rt->addr().to_v4(), plen)) {
        BfdProto::BfdIterator tmp = start_iter++;
        if (tmp->second->DeleteBfdRoute()) {
            vrf_state_->bfd_proto->DeleteBfdEntry(tmp->second);
        }
    }
}

void BfdDBState::Update(const AgentRoute *rt) {
    if (bfd_path_preference_state_) {
        bfd_path_preference_state_->SendBfdRequestForAllIntf(rt);
    }

    const InetUnicastRouteEntry *ip_rt =
        dynamic_cast<const InetUnicastRouteEntry *>(rt);
    if (ip_rt == NULL) {
        return;
    }

    if (ip_rt->GetActiveNextHop()->GetType() == NextHop::RESOLVE) {
        resolve_route_ = true;
    }

    bool policy = ip_rt->GetActiveNextHop()->PolicyEnabled();
    const SecurityGroupList sg = ip_rt->GetActivePath()->sg_list();
    

    if (policy_ != policy || sg != sg_list_ ||
        vn_list_ != ip_rt->GetActivePath()->dest_vn_list()) {
        policy_ = policy;
        sg_list_ = sg;
        vn_list_ = ip_rt->GetActivePath()->dest_vn_list();
        if (resolve_route_) {
            UpdateBfdRoutes(ip_rt);
        }
    }
}

void BfdVrfState::EvpnRouteUpdate(DBTablePartBase *part, DBEntryBase *entry) {
    EvpnRouteEntry *route = static_cast<EvpnRouteEntry *>(entry);

    BfdDBState *state = static_cast<BfdDBState *>(entry->GetState(part->parent(),
                                                  evpn_route_table_listener_id));

    if (entry->IsDeleted() || deleted) {
        if (state) {
            entry->ClearState(part->parent(), evpn_route_table_listener_id);
            delete state;
        }
        return;
    }

    if (state == NULL) {
        state = new BfdDBState(this, route->vrf_id(), route->ip_addr(),
                               route->GetVmIpPlen());
        entry->SetState(part->parent(), evpn_route_table_listener_id, state);
    }

    state->Update(route);
}

void BfdVrfState::RouteUpdate(DBTablePartBase *part, DBEntryBase *entry) {
    InetUnicastRouteEntry *route = static_cast<InetUnicastRouteEntry *>(entry);

    BfdDBState *state =
        static_cast<BfdDBState *>
        (entry->GetState(part->parent(), route_table_listener_id));

    const InterfaceNH *intf_nh = dynamic_cast<const InterfaceNH *>(
            route->GetActiveNextHop());
    const Interface *intf = (intf_nh) ?
        static_cast<const Interface *>(intf_nh->GetInterface()) : NULL;

    BfdKey key(route->addr().to_v4().to_ulong(), route->vrf());
    BfdEntry *bfdentry = bfd_proto->GratuitousBfdEntry(key, intf);
    if (entry->IsDeleted() || deleted) {
        if (state) {
            bfd_proto->DeleteGratuitousBfdEntry(bfdentry);
            entry->ClearState(part->parent(), route_table_listener_id);
            state->Delete(route);
            delete state;
        }
        return;
    }

    if (!state) {
        state = new BfdDBState(this, route->vrf_id(), route->addr(),
                               route->plen());
        entry->SetState(part->parent(), route_table_listener_id, state);
    }

    if (route->vrf()->GetName() == agent->fabric_vrf_name() &&
        route->GetActiveNextHop()->GetType() == NextHop::RECEIVE &&
        bfd_proto->agent()->router_id() == route->addr().to_v4()) {
        //Send Grat BFD
        bfd_proto->AddGratuitousBfdEntry(key);
        bfd_proto->SendBfdIpc(BfdProto::BFD_SEND_GRATUITOUS,
                              route->addr().to_v4().to_ulong(), route->vrf(),
                              bfd_proto->ip_fabric_interface());
    } else {
        if (intf_nh) {
            if (!intf->IsDeleted() && intf->type() == Interface::VM_INTERFACE) {
                BfdKey intf_key(route->addr().to_v4().to_ulong(), route->vrf());
                bfd_proto->AddGratuitousBfdEntry(intf_key);
                bfd_proto->SendBfdIpc(BfdProto::BFD_SEND_GRATUITOUS,
                        route->addr().to_v4().to_ulong(), intf->vrf(), intf);
            }
       }
    }

    //Check if there is a local VM path, if yes send a
    //BFD request, to trigger route preference state machine
    if (state && route->vrf()->GetName() != agent->fabric_vrf_name()) {
        state->Update(route);
    }
}

bool BfdVrfState::DeleteRouteState(DBTablePartBase *part, DBEntryBase *entry) {
    RouteUpdate(part, entry);
    return true;
}

bool BfdVrfState::DeleteEvpnRouteState(DBTablePartBase *part,
                                       DBEntryBase *entry) {
    EvpnRouteUpdate(part, entry);
    return true;
}


void BfdVrfState::Delete() {
    if (walk_id_ != DBTableWalker::kInvalidWalkerId)
        return;
    deleted = true;
    DBTableWalker *walker = agent->db()->GetWalker();
    walk_id_ = walker->WalkTable(rt_table, NULL,
            boost::bind(&BfdVrfState::DeleteRouteState, this, _1, _2),
            boost::bind(&BfdVrfState::WalkDone, _1, this));
    evpn_walk_id_ = walker->WalkTable(evpn_rt_table, NULL,
            boost::bind(&BfdVrfState::DeleteEvpnRouteState, this, _1, _2),
            boost::bind(&BfdVrfState::WalkDone, _1, this));
}

void BfdVrfState::WalkDone(DBTableBase *partition, BfdVrfState *state) {
    if (partition == state->rt_table) {
        state->walk_id_ = DBTableWalker::kInvalidWalkerId;
        state->l3_walk_completed_ = true;
    } else {
        state->evpn_walk_id_ = DBTableWalker::kInvalidWalkerId;
        state->evpn_walk_completed_ = true;
    }

    if (state->PreWalkDone(partition)) {
        delete state;
    }
}

bool BfdVrfState::PreWalkDone(DBTableBase *partition) {
    if (bfd_proto->ValidateAndClearVrfState(vrf, this) == false) {
        return false;
    }

    rt_table->Unregister(route_table_listener_id);
    table_delete_ref.Reset(NULL);

    evpn_rt_table->Unregister(evpn_route_table_listener_id);
    evpn_table_delete_ref.Reset(NULL);
    return true;
}

BfdPathPreferenceState* BfdVrfState::Locate(const IpAddress &ip) {
    BfdPathPreferenceState* ptr = bfd_path_preference_map_[ip];

    if (ptr == NULL) {
        ptr = new BfdPathPreferenceState(this, vrf->vrf_id(), ip, 32);
        bfd_path_preference_map_[ip] = ptr;
    }
    return ptr;
}

void BfdVrfState::Erase(const IpAddress &ip) {
    bfd_path_preference_map_.erase(ip);
}

BfdVrfState::BfdVrfState(Agent *agent_ptr, BfdProto *proto, VrfEntry *vrf_entry,
                         AgentRouteTable *table, AgentRouteTable *evpn_table):
    agent(agent_ptr), bfd_proto(proto), vrf(vrf_entry), rt_table(table),
    evpn_rt_table(evpn_table), route_table_listener_id(DBTableBase::kInvalidId),
    evpn_route_table_listener_id(DBTableBase::kInvalidId),
    table_delete_ref(this, table->deleter()),
    evpn_table_delete_ref(this, evpn_table->deleter()),
    deleted(false),
    walk_id_(DBTableWalker::kInvalidWalkerId),
    evpn_walk_id_(DBTableWalker::kInvalidWalkerId),
    l3_walk_completed_(false), evpn_walk_completed_(false) {
}

BfdVrfState::~BfdVrfState() {
    assert(bfd_path_preference_map_.size() == 0);
}

void BfdProto::InterfaceNotify(DBEntryBase *entry) {
    Interface *itf = static_cast<Interface *>(entry);
    if (entry->IsDeleted()) {
        InterfaceBfdMap::iterator it = interface_bfd_map_.find(itf->id());
        if (it != interface_bfd_map_.end()) {
            InterfaceBfdInfo &intf_entry = it->second;
            BfdKeySet::iterator key_it = intf_entry.bfd_key_list.begin();
            while (key_it != intf_entry.bfd_key_list.end()) {
                BfdKey key = *key_it;
                ++key_it;
                BfdIterator bfd_it = bfd_cache_.find(key);
                if (bfd_it != bfd_cache_.end()) {
                    BfdEntry *bfd_entry = bfd_it->second;
                    if (bfd_entry->DeleteBfdRoute()) {
                        DeleteBfdEntry(bfd_it);
                    }
                }
            }
            intf_entry.bfd_key_list.clear();
            interface_bfd_map_.erase(it);
        }
        if (itf->type() == Interface::PHYSICAL &&
            itf->name() == agent_->fabric_interface_name()) {
            set_ip_fabric_interface(NULL);
            set_ip_fabric_interface_index(-1);
        }
    } else {
        if (itf->type() == Interface::PHYSICAL &&
            itf->name() == agent_->fabric_interface_name()) {
            set_ip_fabric_interface(itf);
            set_ip_fabric_interface_index(itf->id());
            if (run_with_vrouter_) {
                set_ip_fabric_interface_mac(itf->mac());
            } else {
                set_ip_fabric_interface_mac(MacAddress());
            }
        }
    }
}

BfdProto::InterfaceBfdInfo& BfdProto::BfdMapIndexToEntry(uint32_t idx) {
    InterfaceBfdMap::iterator it = interface_bfd_map_.find(idx);
    if (it == interface_bfd_map_.end()) {
        InterfaceBfdInfo entry;
        std::pair<InterfaceBfdMap::iterator, bool> ret;
        ret = interface_bfd_map_.insert(InterfaceBfdPair(idx, entry));
        return ret.first->second;
    } else {
        return it->second;
    }
}

void BfdProto::IncrementStatsBfdRequest(uint32_t idx) {
    InterfaceBfdInfo &entry = BfdMapIndexToEntry(idx);
    entry.stats.bfd_req++;
}

void BfdProto::IncrementStatsBfdReply(uint32_t idx) {
    InterfaceBfdInfo &entry = BfdMapIndexToEntry(idx);
    entry.stats.bfd_replies++;
}

void BfdProto::IncrementStatsResolved(uint32_t idx) {
    InterfaceBfdInfo &entry = BfdMapIndexToEntry(idx);
    entry.stats.resolved++;
}

uint32_t BfdProto::BfdRequestStatsCounter(uint32_t idx) {
    InterfaceBfdInfo &entry = BfdMapIndexToEntry(idx);
    return entry.stats.bfd_req;
}

uint32_t BfdProto::BfdReplyStatsCounter(uint32_t idx) {
    InterfaceBfdInfo &entry = BfdMapIndexToEntry(idx);
    return entry.stats.bfd_replies;
}

uint32_t BfdProto::BfdResolvedStatsCounter(uint32_t idx) {
    InterfaceBfdInfo &entry = BfdMapIndexToEntry(idx);
    return entry.stats.resolved;
}

void BfdProto::ClearInterfaceBfdStats(uint32_t idx) {
    InterfaceBfdInfo &entry = BfdMapIndexToEntry(idx);
    entry.stats.Reset();
}

void BfdProto::NextHopNotify(DBEntryBase *entry) {
    NextHop *nh = static_cast<NextHop *>(entry);

    switch(nh->GetType()) {
    case NextHop::BFD: {
        BfdNH *bfd_nh = (static_cast<BfdNH *>(nh));
        if (bfd_nh->IsDeleted()) {
            SendBfdIpc(BfdProto::BFD_DELETE, bfd_nh->GetIp()->to_ulong(),
                       bfd_nh->GetVrf(), bfd_nh->GetInterface()); 
        } else if (bfd_nh->IsValid() == false && bfd_nh->GetInterface()) {
            SendBfdIpc(BfdProto::BFD_RESOLVE, bfd_nh->GetIp()->to_ulong(),
                       bfd_nh->GetVrf(), bfd_nh->GetInterface()); 
        }
        break;
    }

    default:
        break;
    }
}

bool BfdProto::TimerExpiry(BfdKey &key, uint32_t timer_type,
                           const Interface* itf) {
    if (bfd_cache_.find(key) != bfd_cache_.end()) {
        SendBfdIpc((BfdProto::BfdMsgType)timer_type, key, itf);
    }
    return false;
}

 void BfdProto::AddGratuitousBfdEntry(BfdKey &key) {
     BfdEntrySet empty_set;
     gratuitous_bfd_cache_.insert(GratuitousBfdCachePair(key, empty_set));
}

void BfdProto::DeleteGratuitousBfdEntry(BfdEntry *entry) {
    if (!entry)
        return ;

    BfdProto::GratuitousBfdIterator iter = gratuitous_bfd_cache_.find(entry->key());
    if (iter == gratuitous_bfd_cache_.end()) {
        return;
    }

    iter->second.erase(entry);
    delete entry;
    if (iter->second.empty()) {
        gratuitous_bfd_cache_.erase(iter);
    }
}

BfdEntry *
BfdProto::GratuitousBfdEntry(const BfdKey &key, const Interface *intf) {
    BfdProto::GratuitousBfdIterator it = gratuitous_bfd_cache_.find(key);
    if (it == gratuitous_bfd_cache_.end())
        return NULL;

    for (BfdEntrySet::iterator sit = it->second.begin();
         sit != it->second.end(); sit++) {
        BfdEntry *entry = *sit;
        if (entry->interface() == intf)
            return *sit;
    }

    return NULL;
}

BfdProto::GratuitousBfdIterator
BfdProto::GratuitousBfdEntryIterator(const BfdKey &key, bool *key_valid) {
    BfdProto::GratuitousBfdIterator it = gratuitous_bfd_cache_.find(key);
    if (it == gratuitous_bfd_cache_.end())
        return it;
    const VrfEntry *vrf = key.vrf;
    if (!vrf)
        return it;
    const BfdVrfState *state = static_cast<const BfdVrfState *>
                         (vrf->GetState(vrf->get_table_partition()->parent(),
                          vrf_table_listener_id_));
    // If VRF is delete marked, do not add BFD entries to cache
    if (state == NULL || state->deleted == true)
        return it;
    *key_valid = true;
    return it;
}

void BfdProto::SendBfdIpc(BfdProto::BfdMsgType type, in_addr_t ip,
                          const VrfEntry *vrf, InterfaceConstRef itf) {
    BfdIpc *ipc = new BfdIpc(type, ip, vrf, itf);
    agent_->pkt()->pkt_handler()->SendMessage(PktHandler::BFD, ipc);
}

void BfdProto::SendBfdIpc(BfdProto::BfdMsgType type, BfdKey &key,
                          InterfaceConstRef itf) {
    BfdIpc *ipc = new BfdIpc(type, key, itf);
    agent_->pkt()->pkt_handler()->SendMessage(PktHandler::BFD, ipc);
}

bool BfdProto::AddBfdEntry(BfdEntry *entry) {
    const VrfEntry *vrf = entry->key().vrf;
    const BfdVrfState *state = static_cast<const BfdVrfState *>
                         (vrf->GetState(vrf->get_table_partition()->parent(),
                          vrf_table_listener_id_));
    // If VRF is delete marked, do not add BFD entries to cache
    if (state == NULL || state->deleted == true)
        return false;

    bool ret = bfd_cache_.insert(BfdCachePair(entry->key(), entry)).second;
    uint32_t intf_id = entry->interface()->id();
    InterfaceBfdMap::iterator it = interface_bfd_map_.find(intf_id);
    if (it == interface_bfd_map_.end()) {
        InterfaceBfdInfo intf_entry;
        intf_entry.bfd_key_list.insert(entry->key());
        interface_bfd_map_.insert(InterfaceBfdPair(intf_id, intf_entry));
    } else {
        InterfaceBfdInfo &intf_entry = it->second;
        BfdKeySet::iterator key_it = intf_entry.bfd_key_list.find(entry->key());
        if (key_it == intf_entry.bfd_key_list.end()) {
            intf_entry.bfd_key_list.insert(entry->key());
        }
    }
    return ret;
}

bool BfdProto::DeleteBfdEntry(BfdEntry *entry) {
    if (!entry)
        return false;

    BfdProto::BfdIterator iter = bfd_cache_.find(entry->key());
    if (iter == bfd_cache_.end()) {
        return false;
    }

    DeleteBfdEntry(iter);
    return true;
}

BfdProto::BfdIterator
BfdProto::DeleteBfdEntry(BfdProto::BfdIterator iter) {
    BfdEntry *entry = iter->second;
    bfd_cache_.erase(iter++);
    delete entry;
    return iter;
}

BfdEntry *BfdProto::FindBfdEntry(const BfdKey &key) {
    BfdIterator it = bfd_cache_.find(key);
    if (it == bfd_cache_.end())
        return NULL;
    return it->second;
}

bool BfdProto::ValidateAndClearVrfState(VrfEntry *vrf,
                                        const BfdVrfState *vrf_state) {
    if (!vrf_state->deleted) {
        BFD_TRACE(Trace, "BFD state not cleared - VRF is not delete marked",
                  "", vrf->GetName(), "");
        return false;
    }

    if (vrf_state->l3_walk_completed() == false) {
        return false;
    }

    if (vrf_state->evpn_walk_completed() == false) {
        return false;
    }

    if (vrf_state->walk_id_ != DBTableWalker::kInvalidWalkerId ||
        vrf_state->evpn_walk_id_ != DBTableWalker::kInvalidWalkerId) {
        BFD_TRACE(Trace, "BFD state not cleared - Route table walk not complete",
                  "", vrf->GetName(), "");
        return false;
    }

    DBState *state = static_cast<DBState *>
        (vrf->GetState(vrf->get_table_partition()->parent(),
                       vrf_table_listener_id_));
    if (state) {
        vrf->ClearState(vrf->get_table_partition()->parent(),
                        vrf_table_listener_id_);
    }
    return true;
}

BfdProto::BfdIterator
BfdProto::FindUpperBoundBfdEntry(const BfdKey &key) {
        return bfd_cache_.upper_bound(key);
}

BfdProto::BfdIterator
BfdProto::FindLowerBoundBfdEntry(const BfdKey &key) {
        return bfd_cache_.lower_bound(key);
}
