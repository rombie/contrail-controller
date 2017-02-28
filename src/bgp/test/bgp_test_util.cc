/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/test/bgp_test_util.h"

#include <assert.h>
#include <stdio.h>
#include <pugixml/pugixml.hpp>

#include "base/test/task_test_util.h"
#include "db/db.h"
#include "ifmap/test/ifmap_test_util.h"
#include "schema/bgp_schema_types.h"
#include "schema/vnc_cfg_types.h"

using pugi::xml_document;
using pugi::xml_node;
using pugi::node_pcdata;
using namespace std;

namespace bgp_util {
string NetworkConfigGenerate(DB *db,
        const vector<string> &instance_names,
        const multimap<string, string> &connections,
        const vector<string> &networks,
        const vector<int> &network_ids) {
    assert(networks.empty() || instance_names.size() == networks.size());
    assert(networks.size() == network_ids.size());

    int index;
    xml_document xdoc;
    xml_node env = xdoc.append_child("Envelope");
    xml_node update = env.append_child("Body").append_child("response").
            append_child("pollResult").append_child("updateResult");

    index = 0;
    for (vector<string>::const_iterator iter = instance_names.begin();
         iter != instance_names.end(); ++iter) {
        xml_node item = update.append_child("resultItem");
        xml_node id = item.append_child("identity");
        string vn;
        if (networks.empty()) {
            vn.append(*iter);
        } else {
            vn.append(networks[index]);
        }
        id.append_attribute("name") = vn.c_str();
        xml_node meta = item.append_child("metadata");
        xml_node vn_properties = meta.append_child("virtual-network-properties");
        xml_node net_id = vn_properties.append_child("network-id");
        int value;
        if (network_ids.empty()) {
            value = index + 1;
        } else {
            value = network_ids[index];
        }
        char value_str[16];
        snprintf(value_str, sizeof(value), "%d", value);
        autogen::VirtualNetworkType *vn_property =
            new autogen::VirtualNetworkType();;
        vn_property->network_id = value;
        ifmap_test_util::IFMapMsgNodeAdd(db, "virtual-network", vn, 0,
                                         "network-id", vn_property);
        net_id.append_child(node_pcdata).set_value(value_str);
        index++;
    }
    index = 0;
    for (vector<string>::const_iterator iter = instance_names.begin();
         iter != instance_names.end(); ++iter) {
        xml_node item = update.append_child("resultItem");
        xml_node id1 = item.append_child("identity");
        string instance;
        instance.append(*iter);
        id1.append_attribute("name") = instance.c_str();
        xml_node id2 = item.append_child("identity");
        ostringstream target;
        target << "target:64496:" << (index + 1);
        id2.append_attribute("name") = target.str().c_str();
        xml_node meta = item.append_child("metadata");
        meta.append_child("instance-target");
        ifmap_test_util::IFMapMsgLink(db, "routing-instance", instance,
            "route-target", target.str(), "instance-target");
        index++;
    }
    index = 0;
    for (vector<string>::const_iterator iter = instance_names.begin();
         iter != instance_names.end(); ++iter) {
        xml_node item = update.append_child("resultItem");
        xml_node id1 = item.append_child("identity");
        string vn;
        if (networks.empty()) {
            vn.append(*iter);
        } else {
            vn.append(networks[index]);
        }
        id1.append_attribute("name") = vn.c_str();
        xml_node id2 = item.append_child("identity");
        string instance;
        instance.append(*iter);
        id2.append_attribute("name") = instance.c_str();
        xml_node meta = item.append_child("metadata");
        meta.append_child("virtual-network-routing-instance");
        ifmap_test_util::IFMapMsgLink(db, "virtual-network", vn,
                                      "routing-instance", instance,
                                      "virtual-network-routing-instance");
        index++;
    }
    for (multimap<string, string>::const_iterator iter = connections.begin();
         iter != connections.end(); ++iter) {
        xml_node item = update.append_child("resultItem");
        xml_node id1 = item.append_child("identity");
        string instance1;
        instance1.append(iter->first);
        id1.append_attribute("name") = instance1.c_str();
        xml_node id2 = item.append_child("identity");
        string instance2;
        instance2.append(iter->second);
        id2.append_attribute("name") = instance2.c_str();
        xml_node meta = item.append_child("metadata");
        meta.append_child("connection");
        ifmap_test_util::IFMapMsgLink(db, "routing-instance", instance1,
                                      "routing-instance", instance2,
                                      "connection");
    }
    ostringstream oss;
    xdoc.save(oss);
    task_util::WaitForIdle();
    return oss.str();
}

}
