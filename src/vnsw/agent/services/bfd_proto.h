/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_bfd_proto_hpp
#define vnsw_agent_bfd_proto_hpp

#include "pkt/proto.h"
#include "services/bfd_handler.h"
#include "services/bfd_entry.h"

#define BFD_TRACE(obj, ...)                                                 \
do {                                                                        \
    Bfd##obj::TraceMsg(BfdTraceBuf, __FILE__, __LINE__, ##__VA_ARGS__);     \
} while (false)                                                             \

struct BfdVrfState;

class BfdProto : public Proto {
public:
    static const uint16_t kGratRetries = 2;
    static const uint32_t kGratRetryTimeout = 2000;        // milli seconds
    static const uint16_t kMaxRetries = 8;
    static const uint32_t kRetryTimeout = 2000;            // milli seconds
    static const uint32_t kAgingTimeout = (5 * 60 * 1000); // milli seconds

    typedef std::map<BfdKey, BfdEntry *> BfdCache;
    typedef std::pair<BfdKey, BfdEntry *> BfdCachePair;
    typedef std::map<BfdKey, BfdEntry *>::iterator BfdIterator;
    typedef std::set<BfdKey> BfdKeySet;
    typedef std::set<BfdEntry *> BfdEntrySet;
    typedef std::map<BfdKey, BfdEntrySet> GratuitousBfdCache;
    typedef std::pair<BfdKey, BfdEntrySet> GratuitousBfdCachePair;
    typedef std::map<BfdKey, BfdEntrySet>::iterator GratuitousBfdIterator;

    enum BfdMsgType {
        BFD_RESOLVE,
        BFD_DELETE,
        BFD_SEND_GRATUITOUS,
        RETRY_TIMER_EXPIRED,
        AGING_TIMER_EXPIRED,
        GRATUITOUS_TIMER_EXPIRED,
    };

    struct BfdIpc : InterTaskMsg {
        BfdIpc(BfdProto::BfdMsgType msg, BfdKey &akey, InterfaceConstRef itf)
            : InterTaskMsg(msg), key(akey), interface(itf) {}
        BfdIpc(BfdProto::BfdMsgType msg, in_addr_t ip, const VrfEntry *vrf,
               InterfaceConstRef itf) :
            InterTaskMsg(msg), key(ip, vrf), interface(itf) {}

        BfdKey key;
        InterfaceConstRef interface;
    };

    struct BfdStats {
        BfdStats() { Reset(); }
        void Reset() {
            bfd_req = bfd_replies = bfd_gratuitous = 
            resolved = max_retries_exceeded = errors = 0;
            bfd_invalid_packets = bfd_invalid_interface = bfd_invalid_vrf =
                bfd_invalid_address = vm_bfd_req = 0;
        }

        uint32_t bfd_req;
        uint32_t bfd_replies;
        uint32_t bfd_gratuitous;
        uint32_t resolved;
        uint32_t max_retries_exceeded;
        uint32_t errors;
        uint32_t bfd_invalid_packets;
        uint32_t bfd_invalid_interface;
        uint32_t bfd_invalid_vrf;
        uint32_t bfd_invalid_address;
        uint32_t vm_bfd_req;
    };

    struct InterfaceBfdInfo {
        InterfaceBfdInfo() : bfd_key_list(), stats() {}
        BfdKeySet bfd_key_list;
        BfdStats stats;
    };
    typedef std::map<uint32_t, InterfaceBfdInfo> InterfaceBfdMap;
    typedef std::pair<uint32_t, InterfaceBfdInfo> InterfaceBfdPair;

    void Shutdown();
    BfdProto(Agent *agent, boost::asio::io_service &io, bool run_with_vrouter);
    virtual ~BfdProto();

    ProtoHandler *AllocProtoHandler(boost::shared_ptr<PktInfo> info,
                                    boost::asio::io_service &io);
    bool TimerExpiry(BfdKey &key, uint32_t timer_type, const Interface *itf);

    bool AddBfdEntry(BfdEntry *entry);
    bool DeleteBfdEntry(BfdEntry *entry);
    BfdEntry *FindBfdEntry(const BfdKey &key);
    std::size_t GetBfdCacheSize() { return bfd_cache_.size(); }
    const BfdCache& bfd_cache() { return bfd_cache_; }
    const GratuitousBfdCache& gratuitous_bfd_cache() { return gratuitous_bfd_cache_; }
    const InterfaceBfdMap& interface_bfd_map() { return interface_bfd_map_; }

    Interface *ip_fabric_interface() const { return ip_fabric_interface_; }
    uint32_t ip_fabric_interface_index() const {
        return ip_fabric_interface_index_;
    }
    const MacAddress &ip_fabric_interface_mac() const {
        return ip_fabric_interface_mac_;
    }
    void set_ip_fabric_interface(Interface *itf) { ip_fabric_interface_ = itf; }
    void set_ip_fabric_interface_index(uint32_t ind) {
        ip_fabric_interface_index_ = ind;
    }
    void set_ip_fabric_interface_mac(const MacAddress &mac) {
        ip_fabric_interface_mac_ = mac;
    }

    void  AddGratuitousBfdEntry(BfdKey &key);
    void DeleteGratuitousBfdEntry(BfdEntry *entry);
    BfdEntry* GratuitousBfdEntry (const BfdKey &key, const Interface *intf);
    BfdProto::GratuitousBfdIterator
        GratuitousBfdEntryIterator(const BfdKey &key, bool *key_valid);
    void IncrementStatsBfdReq() { bfd_stats_.bfd_req++; }
    void IncrementStatsBfdReplies() { bfd_stats_.bfd_replies++; }
    void IncrementStatsGratuitous() { bfd_stats_.bfd_gratuitous++; }
    void IncrementStatsResolved() { bfd_stats_.resolved++; }
    void IncrementStatsMaxRetries() { bfd_stats_.max_retries_exceeded++; }
    void IncrementStatsErrors() { bfd_stats_.errors++; }
    void IncrementStatsVmBfdReq() { bfd_stats_.vm_bfd_req++; }
    void IncrementStatsInvalidPackets() {
        IncrementStatsErrors();
        bfd_stats_.bfd_invalid_packets++;
    }
    void IncrementStatsInvalidInterface() {
        IncrementStatsErrors();
        bfd_stats_.bfd_invalid_interface++;
    }
    void IncrementStatsInvalidVrf() {
        IncrementStatsErrors();
        bfd_stats_.bfd_invalid_vrf++;
    }
    void IncrementStatsInvalidAddress() {
        IncrementStatsErrors();
        bfd_stats_.bfd_invalid_address++;
    }
    const BfdStats &GetStats() const { return bfd_stats_; }
    void ClearStats() { bfd_stats_.Reset(); }

    void IncrementStatsBfdRequest(uint32_t idx);
    void IncrementStatsBfdReply(uint32_t idx);
    void IncrementStatsResolved(uint32_t idx);
    InterfaceBfdInfo& BfdMapIndexToEntry(uint32_t idx);
    uint32_t BfdRequestStatsCounter(uint32_t idx);
    uint32_t BfdReplyStatsCounter(uint32_t idx);
    uint32_t BfdResolvedStatsCounter(uint32_t idx);
    void ClearInterfaceBfdStats(uint32_t idx);

    uint16_t max_retries() const { return max_retries_; }
    uint32_t retry_timeout() const { return retry_timeout_; }
    uint32_t aging_timeout() const { return aging_timeout_; }
    void set_max_retries(uint16_t retries) { max_retries_ = retries; }
    void set_retry_timeout(uint32_t timeout) { retry_timeout_ = timeout; }
    void set_aging_timeout(uint32_t timeout) { aging_timeout_ = timeout; }
    void SendBfdIpc(BfdProto::BfdMsgType type, in_addr_t ip,
                    const VrfEntry *vrf, InterfaceConstRef itf);
    bool ValidateAndClearVrfState(VrfEntry *vrf, const BfdVrfState *vrf_state);
    BfdIterator FindUpperBoundBfdEntry(const BfdKey &key);
    BfdIterator FindLowerBoundBfdEntry(const BfdKey &key);

    DBTableBase::ListenerId vrf_table_listener_id() const {
        return vrf_table_listener_id_;
    }
private:
    void VrfNotify(DBTablePartBase *part, DBEntryBase *entry);
    void NextHopNotify(DBEntryBase *entry);
    void InterfaceNotify(DBEntryBase *entry);
    void SendBfdIpc(BfdProto::BfdMsgType type, BfdKey &key,
                    InterfaceConstRef itf);
    BfdProto::BfdIterator DeleteBfdEntry(BfdProto::BfdIterator iter);

    BfdCache bfd_cache_;
    BfdStats bfd_stats_;
    GratuitousBfdCache gratuitous_bfd_cache_;
    bool run_with_vrouter_;
    uint32_t ip_fabric_interface_index_;
    MacAddress ip_fabric_interface_mac_;
    Interface *ip_fabric_interface_;
    DBTableBase::ListenerId vrf_table_listener_id_;
    DBTableBase::ListenerId interface_table_listener_id_;
    DBTableBase::ListenerId nexthop_table_listener_id_;
    InterfaceBfdMap interface_bfd_map_;

    uint16_t max_retries_;
    uint32_t retry_timeout_;   // milli seconds
    uint32_t aging_timeout_;   // milli seconds

    DISALLOW_COPY_AND_ASSIGN(BfdProto);
};

//Stucture used to retry BFD queries when a particular route is in
//backup state.
class BfdPathPreferenceState {
public:
    static const uint32_t kMaxRetry = 30 * 5; //retries upto 5 minutes,
                                              //30 tries/per minutes
    static const uint32_t kTimeout = 2000;
    typedef std::map<uint32_t, uint32_t> WaitForTrafficIntfMap;
    typedef std::set<uint32_t> BfdTransmittedIntfMap;

    BfdPathPreferenceState(BfdVrfState *state, uint32_t vrf_id,
                           const IpAddress &vm_ip, uint8_t plen);
    ~BfdPathPreferenceState();

    bool SendBfdRequest();
    bool SendBfdRequest(WaitForTrafficIntfMap &wait_for_traffic_map,
                        BfdTransmittedIntfMap &bfd_transmitted_intf_map);
    void SendBfdRequestForAllIntf(const AgentRoute *route);
    void StartTimer();

    BfdVrfState* vrf_state() {
        return vrf_state_;
    }

    const IpAddress& ip() const {
        return vm_ip_;
    }

    bool IntfPresentInIpMap(uint32_t id) {
        if (l3_wait_for_traffic_map_.find(id) ==
                l3_wait_for_traffic_map_.end()) {
            return false;
        }
        return true;
    }

    bool IntfPresentInEvpnMap(uint32_t id) {
        if (evpn_wait_for_traffic_map_.find(id) ==
                evpn_wait_for_traffic_map_.end()) {
            return false;
        }
        return true;
    }

    uint32_t IntfRetryCountInIpMap(uint32_t id) {
        return l3_wait_for_traffic_map_[id];
    }

    uint32_t IntfRetryCountInEvpnMap(uint32_t id) {
        return evpn_wait_for_traffic_map_[id];
    }

private:
    friend void intrusive_ptr_add_ref(BfdPathPreferenceState *aps);
    friend void intrusive_ptr_release(BfdPathPreferenceState *aps);
    BfdVrfState *vrf_state_;
    Timer *bfd_req_timer_;
    uint32_t vrf_id_;
    IpAddress vm_ip_;
    uint8_t plen_;
    IpAddress gw_ip_;
    WaitForTrafficIntfMap l3_wait_for_traffic_map_;
    WaitForTrafficIntfMap evpn_wait_for_traffic_map_;
    tbb::atomic<int> refcount_;
};

typedef boost::intrusive_ptr<BfdPathPreferenceState> BfdPathPreferenceStatePtr;

void intrusive_ptr_add_ref(BfdPathPreferenceState *aps);
void intrusive_ptr_release(BfdPathPreferenceState *aps);

struct BfdVrfState : public DBState {
public:
    typedef std::map<const IpAddress,
                     BfdPathPreferenceState*> BfdPathPreferenceStateMap;
    typedef std::pair<const IpAddress,
                      BfdPathPreferenceState*> BfdPathPreferenceStatePair;
    BfdVrfState(Agent *agent, BfdProto *proto, VrfEntry *vrf,
                AgentRouteTable *table, AgentRouteTable *evpn_table);
    ~BfdVrfState();
    void RouteUpdate(DBTablePartBase *part, DBEntryBase *entry);
    void EvpnRouteUpdate(DBTablePartBase *part, DBEntryBase *entry);
    void ManagedDelete() { deleted = true;}
    void Delete();
    bool DeleteRouteState(DBTablePartBase *part, DBEntryBase *entry);
    bool DeleteEvpnRouteState(DBTablePartBase *part, DBEntryBase *entry);
    bool PreWalkDone(DBTableBase *partition);
    static void WalkDone(DBTableBase *partition, BfdVrfState *state);

    BfdPathPreferenceState* Locate(const IpAddress &ip);
    void Erase(const IpAddress &ip);
    BfdPathPreferenceState* Get(const IpAddress ip) {
        return bfd_path_preference_map_[ip];
    }

    bool l3_walk_completed() const {
        return l3_walk_completed_;
    }

    bool evpn_walk_completed() const {
        return evpn_walk_completed_;
    }

    Agent *agent;
    BfdProto *bfd_proto;
    VrfEntry *vrf;
    AgentRouteTable *rt_table;
    AgentRouteTable *evpn_rt_table;
    DBTableBase::ListenerId route_table_listener_id;
    DBTableBase::ListenerId evpn_route_table_listener_id;
    LifetimeRef<BfdVrfState> table_delete_ref;
    LifetimeRef<BfdVrfState> evpn_table_delete_ref;
    bool deleted;
    DBTableWalker::WalkId walk_id_;
    DBTableWalker::WalkId evpn_walk_id_;
    BfdPathPreferenceStateMap bfd_path_preference_map_;
    bool l3_walk_completed_;
    bool evpn_walk_completed_;
    friend class BfdProto;
};

class BfdDBState : public DBState {
public:
    static const uint32_t kMaxRetry = 30 * 5; //retries upto 5 minutes,
                                              //30 tries/per minutes
    static const uint32_t kTimeout = 2000;
    typedef std::map<uint32_t, uint32_t> WaitForTrafficIntfMap;

    BfdDBState(BfdVrfState *vrf_state, uint32_t vrf_id,
               IpAddress vm_ip_addr, uint8_t plen);
    ~BfdDBState();
    void Update(const AgentRoute *route);
    void UpdateBfdRoutes(const InetUnicastRouteEntry *route);
    void Delete(const InetUnicastRouteEntry *rt);
private:
    BfdVrfState *vrf_state_;
    SecurityGroupList sg_list_;
    bool policy_;
    bool resolve_route_;
    VnListType vn_list_;
    BfdPathPreferenceStatePtr bfd_path_preference_state_;
};
#endif // vnsw_agent_bfd_proto_hpp
