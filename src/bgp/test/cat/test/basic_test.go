package main

import (
    "strconv"
    "testing"
    "cat"
    "cat/agent"
    "cat/controlnode"
)

func createControlNodseAndAgents(t *testing.T, test string, nc,
                                 na int) (*cat.CAT,
        []*controlnode.ControlNode, []*agent.Agent, error) {
    ct, err := cat.New()
    if err != nil {
        t.Fail()
    }
    control_nodes := []*controlnode.ControlNode{}
    agents := []*agent.Agent{}

    for c := 0; c < nc; c++ {
        cn, err := ct.AddControlNode(test, "control-node" + strconv.Itoa(c), 0)
        if err != nil {
            return ct, control_nodes, agents, err
        }
        control_nodes = append(control_nodes, cn)
    }

    for a := 0; a < na; a++ {
        ag, err := ct.AddAgent(test, "agent1", control_nodes)
        if err != nil {
            return ct, control_nodes, agents, err
        }
        agents = append(agents, ag)
    }
    verifyControlNodseAndAgents(t, control_nodes, agents)
    return ct, control_nodes, agents, err
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

func TestSingleControlNodeSingleAgent(t *testing.T) {
    cat, _, _, _ := createControlNodseAndAgents(
        t, "TestSingleControlNodeSingleAgent", 1, 1)
    cat.Teardown()
}

func TestSingleControlNodeMultipleAgent(t *testing.T) {
    cat, _, _, _ := createControlNodseAndAgents(
        t, "TestSingleControlNodeMultipleAgent",1,3)
    cat.Teardown()
}

func TestMultipleControlNodeSingleAgent(t *testing.T) {
    cat, _, _, _ := createControlNodseAndAgents(
        t, "TestMultipleControlNodeSingleAgent", 3, 1)
    cat.Teardown()
}

func TestMultipleControlNodeMultipleAgent(t *testing.T) {
    cat, _, _, _ := createControlNodseAndAgents(
        t, "TestMultipleControlNodeMultipleAgent", 3, 3)
    cat.Teardown()
}

func TestSingleControlNodeRestart(t *testing.T) {
    cat, control_nodes, agents, _ := createControlNodseAndAgents(
        t, "TestSingleControlNodeRestart", 1, 1)
    control_nodes[0].Restart()
    verifyControlNodseAndAgents(t, control_nodes, agents)
    cat.Teardown()
}

func TestMultipleControlNodeRestart(t *testing.T) {
    cat, control_nodes, agents, _ := createControlNodseAndAgents(
        t, "TestMultipleControlNodeRestart", 2, 3)
    for c := range control_nodes {
        control_nodes[c].Restart()
    }
    verifyControlNodseAndAgents(t, control_nodes, agents)
    cat.Teardown()
}
