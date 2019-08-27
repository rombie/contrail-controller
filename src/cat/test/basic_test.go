package main

import (
    "strconv"
    "testing"
    "cat"
    "cat/agent"
    "cat/config"
    "cat/controlnode"
    "fmt"
)

var cat_obj *cat.CAT
const confFile = "../../../../build/debug/bgp/test/cat_db.json"

func setup() error {
    if cat_obj != nil {
        return nil
    }
    var err error
    cat_obj, err = cat.New()
    if err != nil {
        fmt.Printf("%v", err)
    }
    return err
}

func generateConfiguration() error {
    vm1 := config.NewConfigObject("virtual_machine", "vm1", "", []string{"vm1"})
    vm1.UpdateDB()

    vm2 := config.NewConfigObject("virtual_machine", "vm2", "", []string{"vm2"})
    vm2.UpdateDB()

    vm3 := config.NewConfigObject("virtual_machine", "vm3", "", []string{"vm3"})
    vm3.UpdateDB()

    vm4 := config.NewConfigObject("virtual_machine", "vm4", "", []string{"vm4"})
    vm4.UpdateDB()

    domain := config.NewConfigObject("domain", "default-domain", "",
                                     []string{"default-domain"})
    domain.UpdateDB()

    vn1 := config.NewConfigObject("virtual_network", "vn1", "", []string{"vn1"})
    vn1.UpdateDB()

    ip := config.NewInstanceIp("ip1", "2.2.2.10", "v4")
    ip.AddRef(vn1)
    ip.UpdateDB()
    fmt.Println(config.UUIDTable)

    vr := config.NewVirtualRouter("agent1", "1.2.3.1")
    vr.AddRef(vm1)
    vr.AddRef(vm2)
    vr.UpdateDB()

    vr = config.NewVirtualRouter("agent2", "1.2.3.2")
    vr.AddRef(vm3)
    vr.AddRef(vm4)
    vr.UpdateDB()

    return config.GenerateDB(confFile)
}

func TestSingleControlNodeSingleAgent(t *testing.T) {
    err := setup()
    if err != nil {
        t.Fail()
    }
    generateConfiguration()
    createControlNodseAndAgents(t, "TestSingleControlNodeSingleAgent", 1, 1)
    cat_obj.PauseAfterRun = true
    cat_obj.Teardown()
}

/*

func TestSingleControlNodeMultipleAgent(t *testing.T) {
    if setup() != nil {
        t.Fail()
    }
    createControlNodseAndAgents(t, "TestSingleControlNodeMultipleAgent",1,3)
    cat_obj.Teardown()
}

func TestMultipleControlNodeSingleAgent(t *testing.T) {
    if setup() != nil {
        t.Fail()
    }
    createControlNodseAndAgents(t, "TestMultipleControlNodeSingleAgent", 2, 1)
    cat_obj.Teardown()
}

func TestMultipleControlNodeMultipleAgent(t *testing.T) {
    if setup() != nil {
        t.Fail()
    }
    createControlNodseAndAgents(t, "TestMultipleControlNodeMultipleAgent", 2, 3)
    cat_obj.Teardown()
}

func TestSingleControlNodeRestart(t *testing.T) {
    if setup() != nil {
        t.Fail()
    }
    control_nodes, agents, _ := createControlNodseAndAgents(t,
        "TestSingleControlNodeRestart", 1, 1)
    control_nodes[0].Restart()
    verifyControlNodseAndAgents(t, control_nodes, agents)
    cat_obj.Teardown()
}

func TestMultipleControlNodeRestart(t *testing.T) {
    if setup() != nil {
        t.Fail()
    }
    control_nodes, agents, _ := createControlNodseAndAgents(
        t, "TestMultipleControlNodeRestart", 2, 3)
    for c := range control_nodes {
        control_nodes[c].Restart()
    }
    verifyControlNodseAndAgents(t, control_nodes, agents)
    cat_obj.Teardown()
}
*/

func createControlNodseAndAgents(t *testing.T, test string, nc,
        na int) ([]*controlnode.ControlNode, []*agent.Agent, error) {
    fmt.Printf("%s: Creating %d control-nodes and %d agents\n", test, nc, na);
    control_nodes := []*controlnode.ControlNode{}
    agents := []*agent.Agent{}

    for c := 0; c < nc; c++ {
        cn, err := cat_obj.AddControlNode(test,
            "control-node" + strconv.Itoa(c), confFile, 0)
        if err != nil {
            return control_nodes, agents, err
        }
        control_nodes = append(control_nodes, cn)
    }

    for a := 0; a < na; a++ {
        ag, err := cat_obj.AddAgent(test, "agent1", control_nodes)
        if err != nil {
            return control_nodes, agents, err
        }
        agents = append(agents, ag)
    }

    verifyControlNodseAndAgents(t, control_nodes, agents)
    return control_nodes, agents, nil
}

func verifyControlNodseAndAgents(t *testing.T,
                                 control_nodes []*controlnode.ControlNode,
                                 agents[]*agent.Agent) {
    for c := range control_nodes {
        if !control_nodes[c].CheckXmppConnections(agents, 30, 1) {
            t.Fail()
        }
    }
}
