package main

import (
//    "os"
//    "strconv"
    "encoding/json"
    "fmt"
    "testing"
)

func TestContrailConfiguration(t *testing.T) {
    vr := new(VirtualRouter)
    vr.Create("vrouter1", "127.0.0.1")
    vr.UpdateRef("virtual-machine", "6ce20cfc-c399-11e9-a251-002590c75050")
    b, _ := json.Marshal(vr)
    fmt.Println(vr.toJson(b))
}

/*
func TestSingleControlNodeSingleAgent(t *testing.T) {
    name := "TestSingleControlNodeSingleAgent"
    cat := new(CAT).Initialize()
    c1 := cat.AddControlNode(name, "control-node1", 0)
    agent := cat.AddAgent(name, "agent1", []*ControlNode{c1})
    agents := []*Agent{agent}
    if !c1.CheckXmppConnections(agents, 30, 1) {
        t.Fail()
    }
    cat.CleanUp()
}

func TestSingleControlNodeMultipleAgent(t *testing.T) {
    name := "TestSingleControlNodeMultipleAgent"
    cat := new(CAT).Initialize()
    c1 := cat.AddControlNode(name, "control-node1", 0)
    agents := []*Agent{}
    for i := 1; i <= 2; i++ {
        agents = append(agents,
           cat.AddAgent(name, "agent" + strconv.Itoa(i), []*ControlNode{c1}))
    }
    if !c1.CheckXmppConnections(agents, 30, 1) {
        t.Fail()
    }
    cat.CleanUp()
}

func TestMultipleControlNodeSingleAgent(t *testing.T) {
    name := "TestMultipleControlNodeSingleAgent"
    cat := new(CAT).Initialize()
    control_nodes := []*ControlNode{}
    for i := 1; i <= 2; i++ {
        control_nodes = append(control_nodes,
            cat.AddControlNode(name, "control-node" + strconv.Itoa(i), 0))
    }
    agent := cat.AddAgent(name, "agent1", control_nodes)
    agents := []*Agent{agent}
    for i := 1; i <= 2; i++ {
        if !control_nodes[i-1].CheckXmppConnections(agents, 30, 1) {
            t.Fail()
        }
    }
    cat.CleanUp()
}

func TestMultipleControlNodeMultipleAgent(t *testing.T) {
    name := "TestMultipleControlNodeSingleAgent"
    os.Chdir("controller/src/bgp/test/cat/lib")
    cat := new(CAT).Initialize()
    control_nodes := []*ControlNode{}
    for i := 1; i <= 2; i++ {
        control_nodes = append(control_nodes,
            cat.AddControlNode(name, "control-node" + strconv.Itoa(i), 0))
    }
    agents := []*Agent{}
    for i := 1; i <= 2; i++ {
        agents = append(agents,
           cat.AddAgent(name, "agent" + strconv.Itoa(i), control_nodes))
    }
    for i := 1; i <= 2; i++ {
        if !control_nodes[i-1].CheckXmppConnections(agents, 30, 1) {
            t.Fail()
        }
    }
    cat.CleanUp()
}

func TestSingleControlNodeRestart(t *testing.T) {
    name := "TestSingleControlNodeRestart"
    cat := new(CAT).Initialize()
    c1 := cat.AddControlNode(name, "control-node1", 0)
    agent := cat.AddAgent(name, "agent1", []*ControlNode{c1})
    agents := []*Agent{agent}
    if !c1.CheckXmppConnections(agents, 30, 1) {
        t.Fail()
    }
    c1.Restart()
    if !c1.CheckXmppConnections(agents, 30, 1) {
        t.Fail()
    }
    cat.CleanUp()
}

func TestMultipleControlNodeRestart(t *testing.T) {
    name := "TestMultipleControlNodeRestart"
    os.Chdir("controller/src/bgp/test/cat/lib")
    cat := new(CAT).Initialize()
    control_nodes := []*ControlNode{}
    for i := 1; i <= 2; i++ {
        control_nodes = append(control_nodes,
            cat.AddControlNode(name, "control-node" + strconv.Itoa(i), 0))
    }
    agents := []*Agent{}
    for i := 1; i <= 2; i++ {
        agents = append(agents,
           cat.AddAgent(name, "agent" + strconv.Itoa(i), control_nodes))
    }
    for i := 1; i <= 2; i++ {
        if !control_nodes[i-1].CheckXmppConnections(agents, 30, 1) {
            t.Fail()
        }
    }
    for i := 1; i <= 2; i++ {
        control_nodes[i-1].Restart()
    }
    for i := 1; i <= 2; i++ {
        if !control_nodes[i-1].CheckXmppConnections(agents, 30, 1) {
            t.Fail()
        }
    }
    cat.CleanUp()
}
*/
