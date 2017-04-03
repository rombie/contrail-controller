#TODO

contrail-bfd scons target
command line options with default /etc/contrail/contrail-bfd.conf
packaging
provisioning
add rest-url to push bfd state change notifications
integration with contrail-vrouter-agent
introspect
logs, debugs, traces, uves and counters

schema changes to add bfd as new health-checker option
bfd configuration parameters (on a per session/vmi basis ?)

contrail-voruter-agent restart
contrail-bfd restart
compute node restart

contrail-vrouter-agent upgrade
contrail-bfd upgrade

scaling -- impact on cpu with large number of bfd sessions
           Phase 2: Investigate bfd data operations to vrouter kernel module
