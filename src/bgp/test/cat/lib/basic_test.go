package main

import (
    "os"
    "strconv"
    "testing"
)

func TestBasicOneControlNodeOneAgent(t *testing.T) {
    return
    os.Chdir("/build/anantha/bgpaas")
    cat := new(CAT)
    cat.Initialize()
    c1 := cat.AddControlNode(t.Name(), "control-node1", 10000)
    cat.AddAgent(t.Name(), "agent1", []*ControlNode{c1})
    cat.Pause = true; cat.CleanUP()
}

func TestBasicOneControlNodeTenAgents(t *testing.T) {
    os.Chdir("/build/anantha/bgpaas")
    cat := new(CAT).Initialize()
    c1 := cat.AddControlNode(t.Name(), "control-node1", 10000)
    control_nodes := []*ControlNode{c1}
    agents := []*Agent{}
    for i := 1; i <= 10; i++ {
        agents = append(agents,
           cat.AddAgent(t.Name(), "agent" + strconv.Itoa(i), control_nodes))
    }
    cat.Pause = true; cat.CleanUP()
}
