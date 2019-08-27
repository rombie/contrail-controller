package config

import (
    "encoding/json"
)

type RoutingInstance struct {
    ContrailConfigObject
    IsDefault bool `json:"prop:routing_instance_is_default"`
    RouteTargetRefs []Ref `json:"route_target_refs"`
}

func (o *RoutingInstance) AddRef(obj *ContrailConfigObject) {
    switch obj.Type{
        case "route_target":
            ref := Ref{
              Uuid: obj.Uuid, Type:obj.Type,
              Attr:map[string]interface{} {"attr":"{\"import_export\": null}",},
            }
            o.RouteTargetRefs = append(o.RouteTargetRefs, ref)
    }
    o.UpdateDB()
}

func NewRoutingInstance(name string) *RoutingInstance {
    o := &RoutingInstance{
        ContrailConfigObject: createContrailConfigObject("routing_instance",
            name, "virtual_network",
            []string{"default-domain", "default-project", name, name}),
        IsDefault: false,
    }
    o.UpdateDB()
    return o
}

func (o *RoutingInstance) UpdateDB() {
    b, _ := json.Marshal(o)
    UUIDTable[o.Uuid] = o.ToJson(b)
}
