/*
 * Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_XMPP_RTARGET_MANAGER_H_
#define SRC_BGP_XMPP_RTARGET_MANAGER_H_

#include <map>
#include <set>
#include <string>

#include "bgp/rtarget/rtarget_table.h"

class BgpNeighborRoutingInstance;
class BgpServer;
class BgpXmppChannel;
class IPeer;
class RoutingInstance;

class XmppRTargetManager {
public:
    typedef std::set<RouteTarget> RouteTargetList;

    XmppRTargetManager(BgpServer *bgp_server,
                       BgpXmppChannel *bgp_xmpp_channel);

    void RoutingInstanceCallback(RoutingInstance *rt_instance,
                                 RouteTargetList *targets);
    void PublishRTargetRoute(RoutingInstance *rt_instance,
                             bool add_change);
    void Close();

    void IdentifierUpdateCallback(Ip4Address old_identifier) const;
    void ASNUpdateCallback(as_t old_asn, as_t old_local_asn) const;
    void FillInfo(BgpNeighborRoutingInstance *instance,
                  const RouteTargetList &targets) const;
    void Stale(const RouteTargetList &targets) const;
    void UpdateRouteTargetRouteFlag(RoutingInstance *routing_instance,
             const RouteTargetList &targets, uint32_t flags) const;

private:
    typedef std::set<RoutingInstance *> RoutingInstanceList;
    typedef std::map<RouteTarget, RoutingInstanceList> PublishedRTargetRoutes;

    void AddNewRTargetRoute(RoutingInstance *rtinstance,
                            const RouteTarget &rtarget, BgpAttrPtr attr);
    void DeleteRTargetRoute(RoutingInstance *rtinstance,
                            const RouteTarget &rtarget);

    BgpTable *GetRouteTargetTable() const;
    uint32_t GetRTargetRouteFlag(const RouteTarget &rtarget) const;
    void RTargetRouteOp(as4_t asn, const RouteTarget &rtarget, BgpAttrPtr attr,
                        bool add_change, uint32_t flags = 0) const;
    BgpAttrPtr GetRouteTargetRouteAttr() const;

    virtual bool IsSubscriptionEmpty() const;
    virtual bool IsSubscriptionGrStale(RoutingInstance *instance) const;
    virtual bool IsSubscriptionLlgrStale(RoutingInstance *instance) const;
    virtual bool delete_in_progress() const;
    virtual const IPeer *Peer() const;
    virtual const RouteTargetList &GetSubscribedRTargets(
            RoutingInstance *instance) const;

    PublishedRTargetRoutes rtarget_routes_;
    BgpXmppChannel *bgp_xmpp_channel_;
    BgpServer *bgp_server_;
};

#endif  // SRC_BGP_XMPP_RTARGET_MANAGER_H_
