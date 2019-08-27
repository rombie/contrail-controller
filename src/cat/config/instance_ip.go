package config

import (
    "encoding/json"
)

type InstanceIp struct {
    ContrailConfigObject
    InstanceIpAddress string `json:"prop:instance_ip_address"`
    InstanceIpFamily string `json:"prop:instance_ip_family"`
    VirtualNetworkRefs []Ref `json:"virtual_network_refs"`
}

func (o *InstanceIp) AddRef(obj *ContrailConfigObject) {
    ref := Ref{
        Uuid: obj.Uuid, Type:obj.Type, Attr:map[string]interface{} {"attr":"",},
    }
    switch obj.Type{
        case "virtual_network":
            o.VirtualNetworkRefs = append(o.VirtualNetworkRefs, ref)
    }
    o.UpdateDB()
}

func NewInstanceIp(name, address, family string) *InstanceIp {
    o := &InstanceIp{
        ContrailConfigObject: createContrailConfigObject("instance_ip", name,
          "", []string{name}),
        InstanceIpAddress: address,
        InstanceIpFamily: family,

    }
    o.UpdateDB()
    return o
}

func (o *InstanceIp) UpdateDB() {
    b, _ := json.Marshal(o)
    UUIDTable[o.Uuid] = o.ToJson(b)
}
