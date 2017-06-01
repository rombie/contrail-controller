<h1>TODO</h1>

1. Add introspect
2. Add logs, debugs, traces, uves and counters
3. Support compute-node/contrail-voruter-agent restart (or upgrade)
4. Integrate with contrail-vrouter-agent
5. Add integration tests (with mock-agent ?)

<h1>TODO-Future</h1>

1. Authentication
2. echo mode
3. Demand Mode
4. Offload periodic bfd packet send to vrouter kernel module

<h1>DONE</h1>

1. Add contrail-bfd scons target
2. Add schema changes to add bfd as new health-checker option
3. Add more unit tests (Work in progress)

<h1>N/A</h1>

1. Add to packaging
2. Add to provisioning
3. Add command line options with default /etc/contrail/contrail-bfd.conf
4. Add rest-url to push bfd state change notifications
5. Support contrail-bfd restart
6. Support contrail-bfd upgrade
7. Add bfd configuration parameters (on a per session/vmi basis ?)

<h1>Qs</h1>

1. scaling impact on cpu with large number of bfd sessions (~100 ?)
2. Always stick to passive mode (Makes it easier to choose the right interface)
