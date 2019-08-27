package config

import (
    "encoding/json"
)

type VirtualMachineInterface struct {
    ContrailConfigObject
    VirtualMachineRefs []Ref `json:"virtual_machine_refs"`
    VirtualNetworkRefs []Ref `json:"virtual_network_refs"`
    RoutingInstanceRefs []Ref `json:"routing_instance_refs"`
}

func (o *VirtualMachineInterface) AddRef(obj *ContrailConfigObject) {
    switch obj.Type{
        case "virtual_machine":
            ref := Ref{
                Uuid: obj.Uuid, Type:obj.Type,
                Attr:map[string]interface{} {"attr":"",},
            }
            o.VirtualMachineRefs = append(o.VirtualMachineRefs, ref)
        case "virtual_network":
            ref := Ref{
                Uuid: obj.Uuid, Type:obj.Type,
                Attr:map[string]interface{} {"attr":"",},
            }
            o.VirtualNetworkRefs = append(o.VirtualNetworkRefs, ref)
        case "routing_instance":
            ref := Ref{
                Uuid: obj.Uuid, Type:obj.Type,
                Attr:map[string]interface{} {"attr":"",},
            }
            o.RoutingInstanceRefs = append(o.RoutingInstanceRefs, ref)
    }
    o.UpdateDB()
}

func NewVirtualMachineInterface(name string) *VirtualMachineInterface {
    o := &VirtualMachineInterface{
        ContrailConfigObject: createContrailConfigObject(
          "virtual_machine_interface", name,
          "project", []string{"default-domain", "default-project", name}),
    }
    o.UpdateDB()
    return o
}

func (o *VirtualMachineInterface) UpdateDB() {
    b, _ := json.Marshal(o)
    UUIDTable[o.Uuid] = o.ToJson(b)
}
