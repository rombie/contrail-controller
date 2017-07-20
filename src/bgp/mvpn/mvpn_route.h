/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef SRC_BGP_MVPN_MVPN_ROUTE_H_
#define SRC_BGP_MVPN_MVPN_ROUTE_H_

#include <boost/system/error_code.hpp>

#include <set>
#include <string>
#include <vector>

#include "base/util.h"
#include "bgp/bgp_attr.h"
#include "bgp/bgp_attr_base.h"
#include "bgp/bgp_route.h"
#include "net/address.h"
#include "net/bgp_af.h"
#include "net/rd.h"

class MVpnPrefix {
public:
    enum RouteType {
        Unspecified = 0,
        IntraASPMSIAutoDiscoveryRoute = 1,
        InterASPMSIAutoDiscoveryRoute = 2,
        SPMSIAutoDiscoveryRoute = 3,
        LeafAutoDiscoveryRoute = 4,
        SourceActiveAutoDiscoveryRoute = 5,
        SharedTreeJoinRoute = 6,
        SourceTreeJoinRoute = 7,
    };

    MVpnPrefix();
    MVpnPrefix(uint8_t type, const RouteDistinguisher &rd,
                 const Ip4Address &group, const Ip4Address &source);
    MVpnPrefix(uint8_t type, const RouteDistinguisher &rd,
                 const Ip4Address &originator,
                 const Ip4Address &group, const Ip4Address &source);

    static int FromProtoPrefix(const BgpProtoPrefix &proto_prefix,
                               MVpnPrefix *prefix);
    static int FromProtoPrefix(BgpServer *server,
                               const BgpProtoPrefix &proto_prefix,
                               const BgpAttr *attr, MVpnPrefix *prefix,
                               BgpAttrPtr *new_attr, uint32_t *label,
                               uint32_t *l3_label);
    static MVpnPrefix FromString(const std::string &str,
                                   boost::system::error_code *errorp = NULL);

    std::string ToString() const;
    std::string ToXmppIdString() const;
    static bool IsValidForBgp(uint8_t type);
    static bool IsValid(uint8_t type);
    bool operator==(const MVpnPrefix &rhs) const;
    int CompareTo(const MVpnPrefix &rhs) const;

    uint8_t type() const { return type_; }
    const RouteDistinguisher &route_distinguisher() const { return rd_; }
    Ip4Address router_id() const { return router_id_; }
    Ip4Address group() const { return group_; }
    Ip4Address source() const { return source_; }
    Ip4Address originator() const { return originator_; }
    uint16_t asn() const { return asn_; }
    void set_route_distinguisher(const RouteDistinguisher &rd) { rd_ = rd; }
    uint8_t ip_prefix_length() const { return ip_prefixlen_; }

    void BuildProtoPrefix(BgpProtoPrefix *prefix) const;

private:
    uint8_t type_;
    RouteDistinguisher rd_;
    Ip4Address router_id_;
    Ip4Address originator_;
    Ip4Address group_;
    Ip4Address source_;
    uint8_t ip_prefixlen_;
    uint16_t asn_;
    std::vector<uint8_t> rt_key_;
};

class MVpnRoute : public BgpRoute {
public:
    explicit MVpnRoute(const MVpnPrefix &prefix);
    virtual int CompareTo(const Route &rhs) const;
    virtual std::string ToString() const;
    virtual std::string ToXmppIdString() const;
    virtual bool IsValid() const;

    const MVpnPrefix &GetPrefix() const { return prefix_; }

    virtual KeyPtr GetDBRequestKey() const;
    virtual void SetKey(const DBRequestKey *reqkey);
    virtual void BuildProtoPrefix(BgpProtoPrefix *prefix,
                                  const BgpAttr *attr = NULL,
                                  uint32_t label = 0,
                                  uint32_t l3_label = 0) const;
    virtual void BuildBgpProtoNextHop(std::vector<uint8_t> &nh,
                                      IpAddress nexthop) const;

    virtual bool IsLess(const DBEntry &genrhs) const {
        const MVpnRoute &rhs = static_cast<const MVpnRoute &>(genrhs);
        int cmp = CompareTo(rhs);
        return (cmp < 0);
    }

    virtual u_int16_t Afi() const { return BgpAf::IPv4; }
    virtual u_int8_t Safi() const { return BgpAf::MVpn; }
    virtual u_int8_t XmppSafi() const { return BgpAf::Mcast; }

private:
    MVpnPrefix prefix_;
    mutable std::string xmpp_id_str_;

    DISALLOW_COPY_AND_ASSIGN(MVpnRoute);
};

#endif  // SRC_BGP_MVPN_ERMVPN_ROUTE_H_