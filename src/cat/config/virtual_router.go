package config

import (
    "encoding/json"
)

type VirtualRouter struct {
    ContrailConfigObject
    VirtualRouterIpAddress string `json:"prop:virtual_router_ip_address"`
    VirtualRouterDpdkEnabled bool `json:"prop:virtual_router_dpdk_enabled"`
    VirtualMachineRefs []Ref `json:"virtual_machines_refs"`
}

func (self *VirtualRouter) AddRef(ref_type, uuid string) {
    ref := Ref {Uuid:uuid, Type:ref_type, Attr:map[string]string {"attr":"",},}
    switch ref_type {
        case "virtual-machine":
            self.VirtualMachineRefs = append(self.VirtualMachineRefs, ref)
    }
}

func NewVirtualRouter(name, ip string) *VirtualRouter {
    vr := &VirtualRouter{
        ContrailConfigObject: createContrailConfigObject("virtual_router", name,
          "global-system-config",[]string{"default-global-system-config",name}),
        VirtualRouterIpAddress: ip,
        VirtualRouterDpdkEnabled: false,

    }
    return vr
}

func (vr *VirtualRouter) UpdateDB() {
    b, _ := json.Marshal(vr)
    UUIDTable[vr.Uuid] = vr.ToJson(b)
}
