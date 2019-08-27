package config

import (
    "encoding/json"
)

type GlobalSystemsConfig struct {
    ContrailConfigObject
    AutonomousSystem string `json:"prop:autonomous_system"`
}

func NewGlobalSystemsConfig(as string) *GlobalSystemsConfig {
    o := &GlobalSystemsConfig{
        ContrailConfigObject: createContrailConfigObject(
          "global_system_config", "default-global-system-config",
          "", []string{"default-global-system-config"}),
        AutonomousSystem: as,
    }
    o.UpdateDB()
    return o
}

func (o *GlobalSystemsConfig) UpdateDB() {
    b, _ := json.Marshal(o)
    UUIDTable[o.Uuid] = o.ToJson(b)
}
