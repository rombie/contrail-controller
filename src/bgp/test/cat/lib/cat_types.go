package main

type CAT struct {
    Directory string
    Pause bool
    Verbose bool
    Ip string
    ReportDir string
    LogDir string
    ControlNodes []*ControlNode
    Agents []*Agent
}

type Ports struct {
    ProcessID int `json:ProcessID`
    XmppPort int `json:XmppPort`
    BgpPort int `json:BgpPort`
    HtppPort int `json:HttpPort`
}

type Component struct {
    Cat *CAT
    Pid int
    Name string
    LogDir string
    ConfDir string
    Ports Ports
    ConfFile string
    PortsFile string
    Verbose bool
    HttpPort int
    XmppPort int
}

type ControlNode struct {
    Component
    BgpPort int
}

type Agent struct {
    Component
    XmppPorts []int
}
