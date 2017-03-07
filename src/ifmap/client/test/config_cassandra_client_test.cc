/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */
#include <boost/foreach.hpp>
#include <fstream>

#include <string>
#include <vector>

#include <boost/foreach.hpp>

#include "base/logging.h"
#include "base/task_annotations.h"
#include "base/test/task_test_util.h"
#include "control-node/control_node.h"
#include "database/cassandra/cql/cql_if.h"
#include "db/db.h"
#include "db/db_graph.h"
#include "ifmap/client/config_amqp_client.h"
#include "ifmap/client/config_cass2json_adapter.h"
#include "ifmap/client/config_cassandra_client.h"
#include "ifmap/client/config_client_manager.h"
#include "ifmap/client/config_json_parser.h"
#include "ifmap/ifmap_config_options.h"
#include "ifmap/ifmap_factory.h"
#include "ifmap/ifmap_link.h"
#include "ifmap/ifmap_link_table.h"
#include "ifmap/ifmap_node.h"
#include "ifmap/ifmap_origin.h"
#include "ifmap/ifmap_server.h"
#include "ifmap/test/ifmap_test_util.h"
#include "io/test/event_manager_test.h"

#include "schema/bgp_schema_types.h"
#include "schema/vnc_cfg_types.h"
#include "testing/gunit.h"

using namespace std;

// Db_GetRow
//     Input: Types from Schema
//     Result: True/False
//     col_list: sizes: [0, cr/2, cr-1, cr, cr+1, 2*cr-1, 2*cr, 2*cr+1]
// Db_GetMultiRow
//     Input: Output of Db_GetRow()
//     Result: True/False
//     col_list_vec: SubSets(t, f, v, j)

template <typename T>
static std::vector<std::vector<T> > getAllSubsets(
        const std::vector<T> &vector) {
    std::vector<std::vector<T> > subsets;

    for (size_t i = 0; i < (1 << vector.size()); i++) {
        std::vector<T> subset;
        for (size_t j = 0; j < vector.size(); j++) {
            if (i & (1 << j))
                subset.push_back(vector[j]);
        }
        subsets.push_back(subset);
    }
    return subsets;
}

static bool get_row_result_;
static int get_row_have_index_;
static bool get_multirow_result_;
static int get_multirow_result_index_;
static int asked = 4;
static vector<size_t> get_row_have_sizes = boost::assign::list_of
    (0)(asked/2)(asked-1)(asked)(asked+1)(asked*3/2)(asked*2)(asked*2+1);
static vector<string> col_list_vec_choices = boost::assign::list_of
    ("type")("fqname")("prop:display_name")("junk");
static vector<vector<string> > col_list_vec_options =
    getAllSubsets(col_list_vec_choices);

static const string fq_name_uuids[] = {
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000001",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000002",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000003",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000004",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000005",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000006",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000007",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000008",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-000000000009",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-00000000000A",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-00000000000B",
    "default-domain:admin:vn1:vn1:7acf5d97-d48b-4409-a30c-00000000000C",
};

class CqlIfTest : public cass::cql::CqlIf {
public:
    CqlIfTest(EventManager *evm, const std::vector<std::string> &cassandra_ips,
              int cassandra_port, const std::string &cassandra_user,
              const std::string &cassandra_password) :
            cass::cql::CqlIf(evm, cassandra_ips, cassandra_port, cassandra_user,
                             cassandra_password) {
    }

    virtual bool Db_Init() { return true; }
    virtual void Db_Uninit() { }
    virtual bool Db_SetTablespace(const std::string &tablespace) {return true;}
    virtual bool Db_UseColumnfamily(const std::string &cfname) { return true; }
    virtual bool Db_GetRow(GenDb::ColList *out, const std::string &cfname,
        const GenDb::DbDataValueVec &rowkey,
        GenDb::DbConsistency::type dconsistency,
        const GenDb::ColumnNameRange &crange,
        const GenDb::FieldNamesToReadVec &read_vec) {
        assert(crange.count_);

        // if (!get_row_result_) return false;

        const GenDb::DbDataValue &type(rowkey[0]);
        assert(type.which() == GenDb::DB_VALUE_BLOB);
        GenDb::Blob type_blob(boost::get<GenDb::Blob>(type));
        string type_name =
            string(reinterpret_cast<const char *>(type_blob.data()),
                                                  type_blob.size());
        if (type_name == "virtual_network")
            return true;

        if (!get_row_have_sizes[get_row_have_index_])
            return true;
        
        string column_name;
        if (!crange.start_.empty()) {
            const GenDb::DbDataValue &dname(crange.start_[0]);
            assert(dname.which() == GenDb::DB_VALUE_BLOB);
            GenDb::Blob dname_blob(boost::get<GenDb::Blob>(dname));
            column_name = string(reinterpret_cast<const char *>(
                                    dname_blob.data()), dname_blob.size());
        }

        // Find the first entry > column_name.
        size_t i = 0;
        while (i < get_row_have_sizes[get_row_have_index_] &&
                column_name >= fq_name_uuids[i]) {
            i++;
        }

        if (i >= get_row_have_sizes[get_row_have_index_])
            return true;

        for (int pushed = 0; i < get_row_have_sizes[get_row_have_index_]; i++) {
            GenDb::DbDataValueVec *n = new GenDb::DbDataValueVec();
            n->push_back(GenDb::Blob(reinterpret_cast<const uint8_t *>
                (fq_name_uuids[i].c_str()), fq_name_uuids[i].size()));
            out->columns_.push_back(new GenDb::NewCol(n, NULL, 10));
            if (++pushed >= (int) crange.count_)
                break;
        }
        return true;
    }
    virtual bool Db_GetMultiRow(GenDb::ColListVec *out,
        const std::string &cfname,
        const std::vector<GenDb::DbDataValueVec> &v_rowkey,
        const GenDb::ColumnNameRange &crange,
        const GenDb::FieldNamesToReadVec &read_vec) {
        // if (!get_multirow_result_) return false;
        return true;
    }
    virtual std::vector<GenDb::Endpoint> Db_GetEndpoints() const {
        return std::vector<GenDb::Endpoint>();
    }
};

class ConfigCassandraClientMock : public ConfigCassandraClient {
public:
    ConfigCassandraClientMock(ConfigClientManager *mgr, EventManager *evm,
        const IFMapConfigOptions &options, ConfigJsonParser *in_parser,
        int num_workers) : ConfigCassandraClient(mgr, evm, options, in_parser,
            num_workers) {
    }

private:
    virtual void PraseAndEnqueueToIFMapTable(
        const string &uuid_key, const ConfigCassandraParseContext &context,
        const CassColumnKVVec &cass_data_vec) {
    }
    virtual uint32_t GetCRangeCount() const { return asked; }
};

typedef std::tr1::tuple<bool, size_t, bool, size_t> TestParams;
class ConfigCassandraClientTest : public ::testing::TestWithParam<TestParams> {
protected:
    ConfigCassandraClientTest() :
        thread_(&evm_),
        db_(TaskScheduler::GetInstance()->GetTaskId("db::IFMapTable")),
        ifmap_server_(new IFMapServer(&db_, &graph_, evm_.io_service())),
        config_client_manager_(new ConfigClientManager(&evm_,
            ifmap_server_.get(), "localhost", "config-test", config_options_)) {
    }

    virtual void SetUp() {
        get_row_result_ = std::tr1::get<0>(GetParam());
        get_row_have_index_ = std::tr1::get<1>(GetParam());
        get_multirow_result_ = std::tr1::get<2>(GetParam());
        get_multirow_result_index_ = std::tr1::get<3>(GetParam());

        ConfigCass2JsonAdapter::set_assert_on_parse_error(true);
        IFMapLinkTable_Init(&db_, &graph_);
        vnc_cfg_JsonParserInit(config_client_manager_->config_json_parser());
        vnc_cfg_Server_ModuleInit(&db_, &graph_);
        bgp_schema_JsonParserInit(config_client_manager_->config_json_parser());
        bgp_schema_Server_ModuleInit(&db_, &graph_);
        thread_.Start();
        task_util::WaitForIdle();
    }

    virtual void TearDown() {
        ifmap_server_->Shutdown();
        task_util::WaitForIdle();
        IFMapLinkTable_Clear(&db_);
        IFMapTable::ClearTables(&db_);
        config_client_manager_->config_json_parser()->MetadataClear("vnc_cfg");
        evm_.Shutdown();
        thread_.Join();
        task_util::WaitForIdle();
    }

    EventManager evm_;
    ServerThread thread_;
    DB db_;
    DBGraph graph_;
    const IFMapConfigOptions config_options_;
    boost::scoped_ptr<IFMapServer> ifmap_server_;
    boost::scoped_ptr<ConfigClientManager> config_client_manager_;
};


TEST_P(ConfigCassandraClientTest, Basic) {
    config_client_manager_->Initialize();
    task_util::WaitForIdle();
}

INSTANTIATE_TEST_CASE_P(ConfigCassandraClientTestWithParams,
                        ConfigCassandraClientTest,
::testing::Combine(
    ::testing::Bool(), // Result: True/False
    ::testing::Range<size_t>(0, get_row_have_sizes.size()), // Db_GetRow
    ::testing::Bool(), // Result: True/False
    ::testing::Range<size_t>(0, col_list_vec_options.size()) // Db_GetMultiRow
));

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    LoggingInit();
    ControlNode::SetDefaultSchedulingPolicy();
    ConfigAmqpClient::set_disable(true);
    IFMapFactory::Register<ConfigCassandraClient>(
        boost::factory<ConfigCassandraClientMock *>());
    IFMapFactory::Register<cass::cql::CqlIf>(boost::factory<CqlIfTest *>());
    int status = RUN_ALL_TESTS();
    TaskScheduler::GetInstance()->Terminate();
    return status;
}
