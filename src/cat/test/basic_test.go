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

func TestSingleControlNodeSingleAgent(t *testing.T) {
    err := setup()
    if err != nil {
        t.Fail()
    }
    createControlNodseAndAgents(t, "TestSingleControlNodeSingleAgent", 1, 1)
    // cat_obj.PauseAfterRun = true
    cat_obj.Teardown()
}

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

func createControlNodseAndAgents(t *testing.T, test string, nc,
        na int) ([]*controlnode.ControlNode, []*agent.Agent, error) {
    fmt.Printf("%s: Creating %d control-nodes and %d agents\n", test, nc, na);
    generateConfiguration()
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

    verifyConfiguration(t, control_nodes)
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

func generateConfiguration() error {
    config.NewGlobalSystemsConfig("100")
    vm1 := config.NewConfigObject("virtual_machine", "vm1", "", []string{"vm1"})
    vm2 := config.NewConfigObject("virtual_machine", "vm2", "", []string{"vm2"})
    vm3 := config.NewConfigObject("virtual_machine", "vm3", "", []string{"vm3"})
    vm4 := config.NewConfigObject("virtual_machine", "vm4", "", []string{"vm4"})

    domain := config.NewConfigObject("domain", "default-domain", "",
                                     []string{"default-domain"})

    project := config.NewConfigObject("project", "default-project",
        "domain:" + domain.Uuid, []string{"default-domain", "default-project"})

    network_ipam := config.NewConfigObject("network_ipam",
        "default-network-ipam", "project:" + project.Uuid,
        []string{"default-domain", "default-project", "default-network-ipam"})

    rtarget := config.NewConfigObject("route_target", "target:100:8000000", "",
                                      []string{"target:100:8000000"})
    ri1 := config.NewRoutingInstance("vn1")
    ri1.AddRef(rtarget)

    vn1 := config.NewVirtualNetwork("vn1")
    vn1.AddRef(network_ipam)
    vn1.AddChild(&ri1.ContrailConfigObject)

    vr := config.NewVirtualRouter("agent1", "1.2.3.1")
    vr.AddRef(vm1)
    vr.AddRef(vm2)

    vr = config.NewVirtualRouter("agent2", "1.2.3.2")
    vr.AddRef(vm3)
    vr.AddRef(vm4)

    vmi1 := config.NewVirtualMachineInterface("vmi1")
    vmi1.AddRef(vm1)
    vmi1.AddRef(&vn1.ContrailConfigObject)
    vmi1.AddRef(&ri1.ContrailConfigObject)

    instance_ip := config.NewInstanceIp("ip1", "2.2.2.10", "v4")
    instance_ip.AddRef(&vn1.ContrailConfigObject)
    instance_ip.AddRef(&vmi1.ContrailConfigObject)

    return config.GenerateDB(confFile)
}

func verifyConfiguration(t *testing.T,
                         control_nodes []*controlnode.ControlNode) {
    for c := range control_nodes {
        if !control_nodes[c].CheckConfiguration("virtual-machine", 4, 3, 3) {
            t.Fail()
        }
        if !control_nodes[c].CheckConfiguration("virtual-router", 2, 3, 3) {
            t.Fail()
        }
        if !control_nodes[c].CheckConfiguration("virtual-network", 1, 3, 3) {
            t.Fail()
        }
    }
}
