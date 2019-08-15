package main

import (
    "fmt"
    "os"
    "strconv"
    "testing"
)

func TestOneControlNodeOneAgent(t *testing.T) {
    name := "TestOneControlNodeOneAgent"
    os.Chdir("/build/anantha/bgpaas")
    cat := new(CAT).Initialize()
    c1 := cat.AddControlNode(name, "control-node1", 10000)
    agent := cat.AddAgent(name, "agent1", []*ControlNode{c1})
    assert(c1.CheckXmppConnections(agents))
    cat.CleanUp()
}

func TestOneControlNodeTenAgents(t *testing.T) {
    name := "TestOneControlNodeTenAgents"
    os.Chdir("/build/anantha/bgpaas")
    cat := new(CAT).Initialize()
    c1 := cat.AddControlNode(name, "control-node1", 10000)
    agents := []*Agent{}
    for i := 1; i <= 10; i++ {
        agents = append(agents,
           cat.AddAgent(name, "agent" + strconv.Itoa(i), []*ControlNode{c1}))
    }
    assert(c1.CheckXmppConnections(agents))
    cat.CleanUp()
}
