package config

import (
    "cat/types"
    "encoding/json"
    "github.com/google/uuid"
)

type VirtualNetwork struct {
    ContrailConfigObject
    NetworkIpamRefs []Ref `json:"network_ipam_refs"`
}

func (o *VirtualNetwork) AddRef(obj *ContrailConfigObject) {
    switch obj.Type{
        case "network_ipam":
            u, _ := uuid.NewUUID()
            subnet := types.VnSubnetsType{
                IpamSubnets: []types.IpamSubnetType{
                types.IpamSubnetType {
                    Subnet: &types.SubnetType {
                        IpPrefix: "1.1.1.0",
                        IpPrefixLen: 24,
                    },
                    AddrFromStart: true,
                    EnableDhcp:true,
                    DefaultGateway: "1.1.1.1",
                    SubnetUuid: u.String(),
                    DnsServerAddress: "1.1.1.2",
                } },
            }
            ref := Ref{ Uuid: obj.Uuid, Type:obj.Type,
                        Attr:map[string]interface{} {"attr": subnet,},
            }
            o.NetworkIpamRefs = append(o.NetworkIpamRefs, ref)
    }
}

func NewVirtualNetwork(name string) *VirtualNetwork {
    o := &VirtualNetwork{
        ContrailConfigObject: createContrailConfigObject("virtual_network",
         name, "project", []string{"default-domain", "default-project", name}),
    }
    return o
}

func (o *VirtualNetwork) UpdateDB() {
    b, _ := json.Marshal(o)
    UUIDTable[o.Uuid] = o.ToJson(b)
}
