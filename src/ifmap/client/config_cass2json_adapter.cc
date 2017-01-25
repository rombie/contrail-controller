/*
 * Copyright (c) 2016 Juniper Networks, Inc. All rights reserved.
 */

#include "config_cass2json_adapter.h"

#include <assert.h>
#include <boost/algorithm/string/predicate.hpp>
#include <iostream>

#include <boost/assign/list_of.hpp>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "base/string_util.h"
#include "config_cassandra_client.h"
#include "config_json_parser.h"

using boost::assign::list_of;
using rapidjson::Document;
using rapidjson::StringBuffer;
using rapidjson::Value;
using rapidjson::Writer;
using std::string;
using std::set;

const string ConfigCass2JsonAdapter::fq_name_prefix = "fq_name";
const string ConfigCass2JsonAdapter::prop_prefix = "prop:";
const string ConfigCass2JsonAdapter::list_prop_prefix = "propl:";
const string ConfigCass2JsonAdapter::map_prop_prefix = "propm:";
const string ConfigCass2JsonAdapter::ref_prefix = "ref:";
const string ConfigCass2JsonAdapter::parent_prefix = "parent:";
const string ConfigCass2JsonAdapter::parent_type_prefix = "parent_type";

const set<string> ConfigCass2JsonAdapter::allowed_properties =
             list_of(prop_prefix)(map_prop_prefix)
                    (list_prop_prefix)(ref_prefix)(parent_prefix);

ConfigCass2JsonAdapter::ConfigCass2JsonAdapter(
       ConfigCassandraClient *cassandra_client, const string &obj_type,
       const CassColumnKVVec &cdvec) : cassandra_client_(cassandra_client),
    type_("") {
    CreateJsonString(obj_type, cdvec);
}

string ConfigCass2JsonAdapter::GetJsonString(const Value &attr_value) {
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    attr_value.Accept(writer);
    return buffer.GetString();
}

// Return true if the caller needs to append a comma. False otherwise.
void ConfigCass2JsonAdapter::AddOneEntry(const string &obj_type, Value &d,
                                         const JsonAdapterDataType &c) {
    Document::AllocatorType &a = json_document_.GetAllocator();

    if (boost::starts_with(c.key, prop_prefix)) {
        if (c.value == "null")
            return;
        string c_value = c.value;
        if (c.key == "prop:security_group_id" && c.value[0] != '\"')
            c_value = "\"" + c.value + "\"";
        Document prop_document(&json_document_.GetAllocator());
        prop_document.Parse<0>(c_value.c_str());
        assert(!prop_document.HasParseError());
        Value vk;
        d.AddMember(vk.SetString(c.key.substr(prop_prefix.size()).c_str(), a),
                    prop_document, a);
        return;
    }

    if (boost::starts_with(c.key, map_prop_prefix) ||
            boost::starts_with(c.key, list_prop_prefix)) {
        size_t from_front_pos = c.key.find(':');
        size_t from_back_pos = c.key.rfind(':');
        string prop_map = c.key.substr(from_front_pos + 1,
                                       from_back_pos - from_front_pos - 1);
        string wrapper = cassandra_client_->mgr()->GetWrapperFieldName(type_,
                                                       prop_map);
        if (!d.HasMember(prop_map.c_str())) {
            Value v;
            Value vk;
            d.AddMember(vk.SetString(prop_map.c_str(), a), v.SetObject(), a);
            Value va;
            Value vak;
            d[prop_map.c_str()].AddMember(vak.SetString(wrapper.c_str(), a),
                                          va.SetArray(), a);
        }

        Document map_document(&json_document_.GetAllocator());
        map_document.Parse<0>(c.value.c_str());
        assert(!map_document.HasParseError());
        d[prop_map.c_str()][wrapper.c_str()].PushBack(map_document, a);
        return;
    }

    if (boost::starts_with(c.key, ref_prefix)) {
        size_t from_front_pos = c.key.find(':');
        size_t from_back_pos = c.key.rfind(':');
        assert(from_front_pos != string::npos);
        assert(from_back_pos != string::npos);
        string ref_type = c.key.substr(from_front_pos + 1,
                                       from_back_pos-from_front_pos - 1);
        string ref_uuid = c.key.substr(from_back_pos + 1);

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
            assert(!ref_document.HasParseError());
            Value &attr_value = ref_document["attr"];
            v.AddMember("attr", attr_value, a);
        } else {
            Value vm;
            v.AddMember("attr", vm.SetObject(), a);
        }
        d[r.c_str()].PushBack(v, a);
        return;
    }

    if (boost::starts_with(c.key, parent_prefix)) {
        size_t pos = c.key.rfind(':');
        assert(pos != string::npos);
        size_t type_pos = c.key.find(':');
        assert(type_pos != string::npos);
        Value v;
        Value vk;
        d.AddMember(vk.SetString(parent_type_prefix.c_str(), a),
                    v.SetString(c.key.substr(type_pos + 1,
                                             pos-type_pos - 1).c_str(), a), a);
        return;
    }

    if (!c.key.compare(fq_name_prefix)) {
        Document fq_name_document(&json_document_.GetAllocator());
        fq_name_document.Parse<0>(c.value.c_str());
        assert(!fq_name_document.HasParseError());
        Value vk;
        d.AddMember(vk.SetString(c.key.c_str(), a), fq_name_document, a);
        return;
    }

    if (!c.key.compare("type")) {
        // Prepend the 'type'. This is "our key", with value being the json
        // sub-document containing all other columns.
        assert(type_ != obj_type);
        type_ = c.value;
        type_.erase(remove(type_.begin(), type_.end(), '\"' ), type_.end());
        return;
    }
}

bool ConfigCass2JsonAdapter::CreateJsonString(const string &obj_type,
                                              const CassColumnKVVec &cdvec) {
    Value d;
    d.SetObject();

    // First look for and part "type" field. We usually expect it to be at the
    // end as column names are suppose to be allways sorted lexicographically.
    size_t type_index = -1;
    for (size_t i = cdvec.size() - 1; i >= 0; i--) {
        if (cdvec[i].key == "type") {
            AddOneEntry(obj_type, d, cdvec[i]);
            type_index = i;
            break;
        }
    }

    assert(type_ != "");
    for (size_t i = 0; i < cdvec.size(); ++i) {
        if (i != type_index)
            AddOneEntry(obj_type, d, cdvec[i]);
    }

    Value vk;
    json_document_.SetObject().AddMember(
        vk.SetString(type_.c_str(), json_document_.GetAllocator()), d,
        json_document_.GetAllocator());
    return true;
}
