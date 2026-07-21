// The create_index journey (https://github.com/oramasearch/zetta/issues/825): an
// agent loads a table, then drives the create_index capability exactly as it
// would over MCP — records a Search index over the table, discovers it via
// list_indexes / get_index, and searches it, the distributed full-text path
// returning the consolidated-oracle ranking. Layer 1: the same registry handlers
// the MCP endpoint renders, no HTTP. It injects the partition-independent
// reference scorer so the ranking is exact regardless of whether the oramacore
// engine is compiled in (the real-engine distributed path is the in-app
// cluster.fulltext_index case, TC-13).
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "index/term_frequency_backend.hpp"
#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::data::RoundRobinPartitioner;
using zetta::index::IndexQuery;
using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;
using zetta::test::TermFrequencyBackend;

nlohmann::json article(const std::string& title, const std::string& body) {
    return nlohmann::json{{"title", title}, {"body", body}};
}

// "postgres" matches several rows with clearly separated term frequencies, so the
// global ranking is unambiguous (the test_node_search corpus).
std::vector<nlohmann::json> postgres_corpus() {
    return {
        article("Postgres deep dive",
                "postgres postgres postgres postgres tuning and scaling"),
        article("Database indexes", "btree gin brin and postgres indexes"),
        article("Postgres replication",
                "postgres postgres replication and postgres failover"),
        article("Cooking pasta", "boil water then add salt and pasta"),
        article("Scaling Postgres", "postgres postgres horizontal scaling"),
        article("Widget catalog", "a guide about widgets and gadgets"),
        article("Postgres backups", "postgres nightly backups explained"),
    };
}

// The consolidated-index ORACLE over the whole table — the ranking the
// distributed per-partition path must reproduce (the reference scorer is
// partition-independent, so this is exact).
std::vector<std::string> consolidated_oracle(zetta::data::Table& table,
                                             const zetta::index::IndexSpec& spec,
                                             const IndexQuery& query) {
    TermFrequencyBackend oracle;
    oracle.build(spec, zetta::index::read_index_partitions(table.store()));
    return oracle.query(spec, query).row_ids;
}

}  // namespace

TEST(CreateIndexJourney, CreateThenDiscoverThenSearch) {
    TempDir dir;
    Node node{NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    // The partition-independent reference scorer, both as the node's search
    // backend (the distributed build uses it) and in the index registry (so the
    // create_index backend validation resolves "tf_reference").
    node.set_search_backend(std::make_shared<TermFrequencyBackend>());
    node.index_registry().add(std::make_shared<TermFrequencyBackend>());

    // A single-node loopback cluster so the distributed full-text path engages
    // over the node's own placed partitions.
    ClusterConfig cluster_config;
    cluster_config.listen_host = "127.0.0.1";
    cluster_config.listen_port = 47410;
    cluster_config.port_range = 1;
    Cluster cluster{"ci-create-index", cluster_config};
    cluster.start();
    node.attach_cluster(cluster);

    // Load the corpus as a partitioned table (partitioned placement is what makes
    // the distributed path authoritative — add_dataset's default).
    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    table->set_partitioner(std::make_shared<RoundRobinPartitioner>(3));
    for (const nlohmann::json& row : postgres_corpus()) table->add_row(row);
    const zetta::data::PartitionWriteResult written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;

    Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();
    auto call = [&](const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    };

    // Step 1 — create_index over the table, driven as an agent would over MCP.
    auto created = call("create_index",
                        nlohmann::json{{"table", "articles"},
                                       {"fields", {"title", "body"}},
                                       {"backend", "tf_reference"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value.at("table"), "articles");
    EXPECT_EQ(created.value.at("kind"), "search");
    EXPECT_EQ(created.value.at("backend"), "tf_reference");
    EXPECT_FALSE(created.value.at("exposed").get<bool>());
    const std::string index_id = created.value.at("index_id").get<std::string>();
    EXPECT_FALSE(index_id.empty());

    // Step 2 — discovery read-back: list_indexes + get_index see the new index.
    auto listed = call("list_indexes", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("indexes").size(), 1u);
    EXPECT_EQ(listed.value.at("indexes")[0].at("id"), index_id);
    EXPECT_EQ(listed.value.at("indexes")[0].at("table"), "articles");
    EXPECT_EQ(listed.value.at("indexes")[0].at("kind"), "search");

    auto got = call("get_index", nlohmann::json{{"index_id", index_id}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value.at("table"), "articles");

    // Step 3 — search the created index: the distributed path returns exactly
    // the consolidated-oracle ranking (the capability produced a searchable
    // index end to end).
    auto result = call("search", nlohmann::json{{"target", "articles"},
                                                {"query", "postgres"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("index_id"), index_id);

    IndexQuery oracle_query;
    oracle_query.term = "postgres";
    const auto index = node.find_index(index_id);
    ASSERT_TRUE(index);
    const std::vector<std::string> oracle =
        consolidated_oracle(*table, index->spec(), oracle_query);
    ASSERT_FALSE(oracle.empty());
    const nlohmann::json& hits = result.value.at("hits");
    ASSERT_EQ(hits.size(), oracle.size());
    for (std::size_t rank = 0; rank < oracle.size(); ++rank) {
        EXPECT_EQ(hits[rank].at("id"), oracle[rank]) << "rank " << rank;
    }

    // Step 4 — the error surfaces the engine's real reason: an unloaded table is
    // not_found; an unknown backend is invalid_input; zero fields is rejected at
    // the schema boundary (also invalid_input).
    auto missing = call("create_index", nlohmann::json{{"table", "nope"},
                                                       {"fields", {"x"}}});
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");
    EXPECT_NE(missing.error.message.find("nope"), std::string::npos);

    auto bad_backend =
        call("create_index", nlohmann::json{{"table", "articles"},
                                            {"fields", {"title"}},
                                            {"backend", "does_not_exist"}});
    EXPECT_FALSE(bad_backend.ok);
    EXPECT_EQ(bad_backend.error.code, "invalid_input");

    auto no_fields =
        call("create_index", nlohmann::json{{"table", "articles"},
                                            {"fields", nlohmann::json::array()}});
    EXPECT_FALSE(no_fields.ok);
    EXPECT_EQ(no_fields.error.code, "invalid_input");

    node.stop();  // tear the search plane down before the cluster
}
