package config

import (
    "encoding/json"
)

type VirtualRouter struct {
    ContrailConfigObject
    VirtualRouterIpAddress string `json:"prop:virtual_router_ip_address"`
    VirtualRouterDpdkEnabled bool `json:"prop:virtual_router_dpdk_enabled"`
    VirtualMachineRefs []Ref `json:"virtual_machine_refs"`
}

func (o *VirtualRouter) AddRef(obj *ContrailConfigObject) {
    ref := Ref{
        Uuid: obj.Uuid, Type:obj.Type, Attr:map[string]interface{} {"attr":"",},
    }
    switch obj.Type{
        case "virtual_machine":
            o.VirtualMachineRefs = append(o.VirtualMachineRefs, ref)
    }
    o.UpdateDB()
}

func NewVirtualRouter(name, ip string) *VirtualRouter {
    o := &VirtualRouter{
        ContrailConfigObject: createContrailConfigObject("virtual_router", name,
          "global-system-config",[]string{"default-global-system-config",name}),
        VirtualRouterIpAddress: ip,
        VirtualRouterDpdkEnabled: false,

    }
    o.UpdateDB()
    return o
}

func (o *VirtualRouter) UpdateDB() {
    b, _ := json.Marshal(o)
    UUIDTable[o.Uuid] = o.ToJson(b)
}
