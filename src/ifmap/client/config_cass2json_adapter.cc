/*
 * Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
 */

#include "config_cass2json_adapter.h"

#include <assert.h>
#include <iostream>

#include <boost/assign/list_of.hpp>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "base/string_util.h"
#include "config_cassandra_client.h"
#include "config_json_parser.h"

using boost::assign::list_of;
using namespace rapidjson;
using namespace std;

const string ConfigCass2JsonAdapter::fq_name_prefix = "fq_name";
const string ConfigCass2JsonAdapter::prop_prefix = "prop:";
const string ConfigCass2JsonAdapter::list_prop_prefix = "propl:";
const string ConfigCass2JsonAdapter::map_prop_prefix = "propm:";
const string ConfigCass2JsonAdapter::child_prefix = "children:";
const string ConfigCass2JsonAdapter::ref_prefix = "ref:";
const string ConfigCass2JsonAdapter::meta_prefix = "META:";
const string ConfigCass2JsonAdapter::backref_prefix = "backref:";
const string ConfigCass2JsonAdapter::parent_prefix = "parent:";
const string ConfigCass2JsonAdapter::parent_type_prefix = "parent_type";
const string ConfigCass2JsonAdapter::comma_str = ",";

const set<string> ConfigCass2JsonAdapter::allowed_properties =
             list_of(prop_prefix)(map_prop_prefix)
                    (list_prop_prefix)(ref_prefix)(parent_prefix);

ConfigCass2JsonAdapter::ConfigCass2JsonAdapter(
       ConfigCassandraClient *cassandra_client, const string &obj_type,
       const CassColumnKVVec &cdvec) : cassandra_client_(cassandra_client),
    type_(""), prop_plen_(prop_prefix.size()),
    propl_plen_(list_prop_prefix.size()),
    propm_plen_(map_prop_prefix.size()),
    meta_plen_(meta_prefix.size()),
    backref_plen_(backref_prefix.size()),
    child_plen_(child_prefix.size()),
    ref_plen_(ref_prefix.size()),
    fq_name_plen_(fq_name_prefix.size()),
    parent_type_plen_(parent_type_prefix.size()),
    parent_plen_(parent_prefix.size()) {
    CreateJsonString(obj_type, cdvec);
}

string ConfigCass2JsonAdapter::GetJsonString(const Value &attr_value) {
    StringBuffer buffer;
    Writer<rapidjson::StringBuffer> writer(buffer);
    attr_value.Accept(writer);
    return buffer.GetString();
}

// Return true if the caller needs to append a comma. False otherwise.
void ConfigCass2JsonAdapter::AddOneEntry(const string &obj_type, Value &d,
                                         const JsonAdapterDataType &c) {
    Document::AllocatorType &a = json_document_.GetAllocator();

    // If the key has 'prop:' at the start, remove it.
    string prop_key = c.key.substr(0, prop_plen_);
    if (prop_key == prop_prefix) {
        if (c.value != "null") {
            Document prop_document(&json_document_.GetAllocator());
            prop_document.Parse<0>(c.value.c_str());
            Value vk;
            d.AddMember(vk.SetString(c.key.substr(prop_plen_).c_str(), a),
                        prop_document, a);
            return;
        }
        return;
    }

    if (c.key.substr(0, propl_plen_) == list_prop_prefix) {
        size_t from_front_pos = c.key.find(':');
        size_t from_back_pos = c.key.rfind(':');
        string property_list = c.key.substr(from_front_pos+1,
                                            from_back_pos-from_front_pos-1);
        int index = 0;
        assert(stringToInteger(c.key.substr(from_back_pos+1), index));
        if (!d.HasMember(property_list.c_str())) {
            Value v;
            Value vk;
            d.AddMember(vk.SetString(property_list.c_str(), a), v.SetArray(),
                        a);
        }

        Value v;
        d[property_list.c_str()].PushBack(v.SetString(c.value.c_str(), a), a);
        return;
    }

    if (c.key.substr(0, propm_plen_) == map_prop_prefix) {
        size_t from_front_pos = c.key.find(':');
        size_t from_back_pos = c.key.rfind(':');
        string property_map = c.key.substr(from_front_pos+1,
                                           from_back_pos-from_front_pos-1);
        string wrapper = cassandra_client_->mgr()->GetWrapperFieldName(type_,
                                                       property_map);
        if (!d.HasMember(property_map.c_str())) {
            Value v;
            Value vk;
            d.AddMember(vk.SetString(property_map.c_str(), a),
                        v.SetObject(), a);

            Value va;
            Value vak;
            d[property_map.c_str()].AddMember(vak.SetString(wrapper.c_str(), a),
                                              va.SetArray(), a);
        }

        Value v;
        d[property_map.c_str()][wrapper.c_str()].PushBack(
                v.SetString(c.value.c_str(), a), a);
        return;
    }

    if (c.key.substr(0, ref_plen_) == ref_prefix) {
        size_t from_front_pos = c.key.find(':');
        size_t from_back_pos = c.key.rfind(':');
        assert(from_front_pos != string::npos);
        assert(from_back_pos != string::npos);
        string ref_type = c.key.substr(from_front_pos+1,
                                             (from_back_pos-from_front_pos-1));
        string ref_uuid = c.key.substr(from_back_pos+1);

        string fq_name_ref = cassandra_client_->UUIDToFQName(ref_uuid);
        if (fq_name_ref == "ERROR")
            return;
        string r = ref_type + "_refs";
        if (!d.HasMember(r.c_str())) {
            Value v;
            Value vk;
            d.AddMember(vk.SetString(r.c_str(), a), v.SetArray(), a);
        }

        Value v;
        v.SetObject();

        Value vs1;
        Value vs2;
        v.AddMember("to", vs1.SetString(fq_name_ref.c_str(), a), a);
        v.AddMember("uuid", vs2.SetString(ref_uuid.c_str(), a), a);

        bool link_with_attr =
            cassandra_client_->mgr()->IsLinkWithAttr(obj_type, ref_type);
        if (link_with_attr) {
            Document ref_document(&json_document_.GetAllocator());
            ref_document.Parse<0>(c.value.c_str());
            Value& attr_value = ref_document["attr"];
            Value vs;
            v.AddMember("attr",
                        vs.SetString(GetJsonString(attr_value).c_str(), a), a);
        }
        d[r.c_str()].PushBack(v, a);
        return;
    }

    if (c.key.substr(0, parent_plen_) == parent_prefix) {
        size_t pos = c.key.rfind(':');
        assert(pos != string::npos);
        size_t type_pos = c.key.find(':');
        assert(type_pos != string::npos);
        Value v;
        d.AddMember("parent_type",
          v.SetString(c.key.substr(type_pos+1, pos-type_pos-1).c_str(), a), a);
        return;
    }

    if (c.key.substr(0, fq_name_plen_) == fq_name_prefix) {
        Document fq_name_document(&json_document_.GetAllocator());
        fq_name_document.Parse<0>(c.value.c_str());
        Value vk;
        d.AddMember(vk.SetString(c.key.c_str(), a), fq_name_document, a);
        return;
    }

    if (c.key.compare("type") == 0) {
        // Prepend the 'type'. This is "our key", with value being the json
        // sub-document containing all other columns.
        assert(type_ != obj_type);
        type_ = c.value;
        type_.erase(remove(type_.begin(), type_.end(), '\"' ), type_.end());
        return;
    }

#if 0
    // If the key has 'parent_type' at the start, ignore the column.
    if (c.key.substr(0, parent_type_plen_) == parent_type_prefix)
        return;

    // If the key has 'children:' at the start, ignore the column.
    if (c.key.substr(0, child_plen_) == child_prefix)
        return;

    // If the key has 'backref:' at the start, ignore the column.
    if (c.key.substr(0, backref_plen_) == backref_prefix)
        return;

    // If the key has 'META:' at the start, ignore the column.
    if (c.key.substr(0, meta_plen_) == meta_prefix)
        return;
    cout << "Unknown tag:" << c.key << endl;
#endif
}

bool ConfigCass2JsonAdapter::CreateJsonString(const string &obj_type,
                                              const CassColumnKVVec &cdvec) {
    Value d;
    d.SetObject();
    for (size_t i = 0; i < cdvec.size(); ++i)
        AddOneEntry(obj_type, d, cdvec[i]);
    assert(type_ != "");
    Value vk;
    json_document_.SetObject().AddMember(
        vk.SetString(type_.c_str(), json_document_.GetAllocator()), d,
        json_document_.GetAllocator());

    return true;
}
