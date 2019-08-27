package config

import (
    "cat/types"
    "encoding/json"
    "fmt"
    "io"
    "os"
    // "github.com/Juniper/contrail-go-api/types"
    "github.com/google/uuid"
    "strings"
)

// ./src/contrail-api-client/generateds/generateDS.py -f -o ~/go/src/github.com/Juniper/contrail-go-api/types -g golang-api src/contrail-api-client/schema/vnc_cfg.xsd

var FQNameTable map[string]map[string]string= make(map[string]map[string]string)
var UUIDTable map[string]map[string]string= make(map[string]map[string]string)

type ContrailConfigObject struct {
    Uuid string
    Type string `json:"type"`
    ParentType string `json:"parent_type"`
    FqName []string `json:"fq_name"`

    Perms2 types.PermType `json:"prop:perms2"`
    IdPerms types.IdPermsType `json:"prop:id_perms"`
    DisplayName string `json:"prop:display_name"`
}

func (self *ContrailConfigObject) ToJson (b []byte) map[string]string {
    var v map[string]interface{}
    json.Unmarshal(b, &v)
    json_strings := make(map[string]string)
    for key, _ := range v {
        if strings.HasSuffix(key, "_refs") {
            if v[key] == nil {
                continue
            }
            refs := v[key].([]interface{})
            for i := range refs {
                ref := refs[i].(map[string]interface{})
                k := "ref:" + ref["type"].(string) + ":" + ref["uuid"].(string)
                // json_strings[k] = "{\"attr\": null}"
                c, _ := json.Marshal(ref["attr"])
                json_strings[k] = string(c)
            }
        } else if strings.HasSuffix(key, "_children") {
            if v[key] == nil {
                continue
            }
            children := v[key].([]interface{})
            for i := range children {
                child := children[i].(map[string]interface{})
                k := "children:" + child["type"].(string) + ":" +
                     child["uuid"].(string)
                json_strings[k] = "null"
            }
        } else {
            b, _ = json.Marshal(v[key])
            json_strings[key] = string(b)
        }
    }
    return json_strings
}

func createContrailConfigObject (tp, name, parent_type string,
                                 fq_name []string) ContrailConfigObject {
    u, _ := uuid.NewUUID()
    us := u.String()
    if FQNameTable[tp] == nil {
        FQNameTable[tp] = make(map[string]string)
    }
    c := ContrailConfigObject{
        Uuid: us,
        Type: tp,
        ParentType: parent_type,
        DisplayName: name,
        Perms2: types.PermType {
            Owner: "cloud-admin",
            OwnerAccess: 0,
        },
        IdPerms: types.IdPermsType {
            Enable: true,
            Uuid: &types.UuidType {
                UuidMslong: 7845966321683075561,
                UuidLslong: 11696129868600660048,
            },
            Created: "2019-08-20T15:25:33.570592",
            LastModified: "2019-08-20T15:25:33.570617",
            UserVisible: true,
        },
        FqName: fq_name,
    }
    FQNameTable[tp][strings.Join(fq_name, ":") + ":" + us] = "null"
    return c
}

type Ref struct {
    Uuid string `json:"uuid"`
    Type string `json:"type"`
    Attr map[string]interface{} `json:"attr"`
}

type Child struct {
    Uuid string `json:"uuid"`
    Type string `json:"type"`
}

func GenerateDB(confFile string) error {
    file, err := os.Create(confFile)
    if err != nil {
        return err
    }
    defer file.Close()
    b1, _ := json.Marshal(UUIDTable)
    b2, _ := json.Marshal(FQNameTable)
    conf := fmt.Sprintf("[{ \"operation\": \"db_sync\", \"db\": " + string(b1) +
                        ", \"OBJ_FQ_NAME_TABLE\": " + string(b2) + "}]\n")
    _, err = io.WriteString(file, conf)
    return file.Sync()
}

func NewConfigObject (tp, name, parent string,
                      fqname []string) *ContrailConfigObject {
    obj := createContrailConfigObject(tp, name, parent, fqname)
    obj.UpdateDB()
    return &obj
}

func (c *ContrailConfigObject) UpdateDB() {
    b, _ := json.Marshal(c)
    UUIDTable[c.Uuid] = c.ToJson(b)
}
