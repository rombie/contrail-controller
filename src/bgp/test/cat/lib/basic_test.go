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
    cat := new(CAT)
    cat.Initialize()
    c1 := cat.AddControlNode(name, "control-node1", 10000)
    cat.AddAgent(name, "agent1", []*ControlNode{c1})
    cat.CleanUp()
}

func TestOneControlNodeTenAgents(t *testing.T) {
    name := "TestOneControlNodeTenAgents"
    os.Chdir("/build/anantha/bgpaas")
    cat := new(CAT).Initialize()
    c1 := cat.AddControlNode(name, "control-node1", 10000)
    control_nodes := []*ControlNode{c1}
    agents := []*Agent{}
    for i := 1; i <= 10; i++ {
        agents = append(agents,
           cat.AddAgent(name, "agent" + strconv.Itoa(i), control_nodes))
    }
    fmt.Println(agents)
    cat.CleanUp()
}
