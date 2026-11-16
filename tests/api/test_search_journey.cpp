// SPDX-License-Identifier: MIT

// The distributed search journey (#642, docs/testing/mcp-journeys.md): on a
// real 3-node loopback cluster holding a partitioned table, an agent drives
// the capability registry exactly as it would over MCP — list_indexes to
// discover what is searchable, search for the ranked hits with per-node
// provenance, get_trace to read the scatter-gather timeline back. Layer 1:
// the same handlers the MCP endpoint renders, no HTTP.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
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
using zetta::data::Table;
using zetta::index::IndexKind;
using zetta::index::IndexQuery;
using zetta::index::IndexSpec;
using zetta::index::IndexValue;
using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;
using zetta::test::TermFrequencyBackend;

NodeConfig node_config(const TempDir& dir) {
    NodeConfig config;
    config.data_dir = dir.path().string();
    return config;
}

ClusterConfig cluster_config(std::uint16_t port,
                             const std::vector<std::string>& seeds) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;
    config.seeds = seeds;
    return config;
}

nlohmann::json article(const std::string& title, const std::string& body) {
    return nlohmann::json{{"title", title}, {"body", body}};
}

IndexValue articles_index() {
    IndexValue value;
    value.table = "articles";
    value.kind = IndexKind::Search;
    value.backend = "tf_reference";
    value.fields = {"title", "body"};
    return value;
}

// The test_node_search.cpp corpus: "postgres" matches with clearly separated
// term frequencies, so the global ranking is unambiguous.
std::vector<nlohmann::json> postgres_corpus() {
    return {
        article("Postgres deep dive",
                "postgres postgres postgres postgres tuning and scaling"),  // 5
        article("Database indexes", "btree gin brin and postgres indexes"),  // 1
        article("Postgres replication",
                "postgres postgres replication and postgres failover"),  // 3
        article("Cooking pasta", "boil water then add salt and pasta"),  // 0
        article("Scaling Postgres", "postgres postgres horizontal scaling"),  // 2
        article("Widget catalog", "a guide about widgets and gadgets"),  // 0
        article("Postgres backups", "postgres nightly backups explained"),  // 1
    };
}

// The consolidated-index ORACLE over every node's rows — the ranking the
// distributed scatter-gather must reproduce.
std::vector<std::string> consolidated_oracle(const std::vector<Table*>& tables,
                                             const IndexSpec& spec,
                                             const IndexQuery& query) {
    TermFrequencyBackend oracle;
    std::vector<zetta::index::IndexPartition> all;
    for (Table* table : tables) {
        for (zetta::index::IndexPartition& partition :
             zetta::index::read_index_partitions(table->store())) {
            all.push_back(std::move(partition));
        }
    }
    oracle.build(spec, all);
    return oracle.query(spec, query).row_ids;
}

// Patient convergence (the multi-node rule): poll with a deadline, ticking the
// nodes so replication reconciles, never a fixed small count.
template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

std::size_t count_spans_named(const nlohmann::json& spans,
                              const std::string& name) {
    std::size_t count = 0;
    for (const nlohmann::json& span : spans) {
        if (span.at("name") == name) ++count;
    }
    return count;
}

// One node of the loopback cluster: a Node holding a partitioned, self-owned
// shard of the corpus, its Cluster, the injected partition-independent
// reference scorer, and the Search index entity registered — the
// test_node_search.cpp acceptance fixture, driven here through the capability
// registry instead of Node::search directly. The sub-indexes are built by the
// node itself (reconcile_search_indexes via tick), never by the test.
struct JourneyNode {
    JourneyNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds,
                const std::vector<nlohmann::json>& shard_rows)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)) {
        node.set_search_backend(std::make_shared<TermFrequencyBackend>());
        node.start();
        auto database = node.create_database("data");
        EXPECT_TRUE(database);
        auto created = database->create_table("articles");
        EXPECT_TRUE(created);
        created->set_partitioner(std::make_shared<RoundRobinPartitioner>(3));
        for (const nlohmann::json& row : shard_rows) created->add_row(row);
        const zetta::data::PartitionWriteResult written =
            zetta::data::write_partitioned(node, *database, *created);
        EXPECT_TRUE(written.ok()) << written.error;
        table = created;
        EXPECT_TRUE(node.create_index(articles_index()));
    }

    ~JourneyNode() { node.stop(); }  // tears the search plane down before cluster

    TempDir temp_dir;
    Node node;
    Cluster cluster;
    std::shared_ptr<Table> table;
};

}  // namespace

// Journey (#642 acceptance): list_indexes -> search -> ranked hits with node
// provenance on a multi-node loopback cluster, then the trace read back with
// its per-peer spans.
TEST(SearchJourney, ListIndexesThenDistributedSearchThenTraceReadBack) {
    constexpr std::size_t kNodes = 3;
    constexpr std::uint16_t kBasePort = 47250;

    // The full corpus, sharded round-robin — each node owns a disjoint slice.
    const std::vector<nlohmann::json> corpus = postgres_corpus();
    std::vector<std::vector<nlohmann::json>> shards(kNodes);
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        shards[i % kNodes].push_back(corpus[i]);
    }

    const std::vector<std::string> ids = {"sj-node-a", "sj-node-b", "sj-node-c"};
    std::vector<std::unique_ptr<JourneyNode>> nodes;
    for (std::size_t index = 0; index < kNodes; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<JourneyNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds,
            shards[index]));
    }

    for (auto& entry : nodes) {
        entry->cluster.start();
        entry->node.attach_cluster(entry->cluster);
    }
    ASSERT_TRUE(wait_for([&] {
        for (const auto& entry : nodes) {
            if (entry->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    })) << "cluster did not converge";

    // Every node keeps its own sub-indexes current from its tick — the wiring
    // that lets ANY node answer a peer's scatter.
    for (auto& entry : nodes) entry->node.tick();
    const auto index = nodes[0]->node.indexes().front();
    ASSERT_TRUE(index);

    // The agent's surface: the same registry the MCP endpoint renders, on the
    // coordinating node, recording into that node's own span ring (#640).
    Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();
    Node& coordinator = nodes[0]->node;
    auto call = [&](const std::string& name, nlohmann::json input) {
        ApiContext context{coordinator, nullptr, "", trace,
                           &coordinator.span_recorder()};
        return registry.call(name, input, context);
    };

    // Step 1 — discovery: list_indexes names the one searchable index.
    auto listed = call("list_indexes", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("indexes").size(), 1u);
    const nlohmann::json& discovered = listed.value.at("indexes")[0];
    EXPECT_EQ(discovered.at("id"), index->id());
    EXPECT_EQ(discovered.at("table"), "articles");
    EXPECT_EQ(discovered.at("kind"), "search");

    // Step 2 — search by the DISCOVERED id: the distributed scatter-gather
    // answers with the oracle's global ranking and full per-node provenance.
    auto result = call(
        "search",
        nlohmann::json{{"target", discovered.at("id")},
                       {"query", "postgres"},
                       {"intent", "find the postgres articles"}});
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("index_id"), index->id());
    EXPECT_EQ(result.value.at("path"), "distributed");

    IndexQuery oracle_query;
    oracle_query.term = "postgres";
    std::vector<Table*> tables;
    for (auto& entry : nodes) tables.push_back(entry->table.get());
    const std::vector<std::string> oracle =
        consolidated_oracle(tables, articles_index().spec(), oracle_query);
    ASSERT_FALSE(oracle.empty());
    const nlohmann::json& hits = result.value.at("hits");
    ASSERT_EQ(hits.size(), oracle.size());
    for (std::size_t rank = 0; rank < oracle.size(); ++rank) {
        EXPECT_EQ(hits[rank].at("id"), oracle[rank]) << "rank " << rank;
    }

    // Per-node provenance (the AX contract): all three nodes answered, exactly
    // one local, the peers moved bytes, every contribution carries its cost.
    const nlohmann::json& provenance = result.value.at("nodes");
    ASSERT_EQ(provenance.size(), kNodes);
    std::size_t locals = 0;
    for (const nlohmann::json& contribution : provenance) {
        EXPECT_FALSE(contribution.at("node_id").get<std::string>().empty());
        EXPECT_GE(contribution.at("latency_ms").get<double>(), 0.0);
        if (contribution.at("local").get<bool>()) {
            ++locals;
            EXPECT_EQ(contribution.at("node_id"), nodes[0]->cluster.node_id());
        } else {
            EXPECT_GT(contribution.at("wire_bytes").get<std::uint64_t>(), 0u);
        }
    }
    EXPECT_EQ(locals, 1u);
    EXPECT_GT(result.value.at("wire_bytes_moved").get<std::uint64_t>(), 0u);

    // Step 3 — the trace read back over the same surface: the api:search call
    // span rides ONE trace with the engine's root search span, its local
    // child, and one search:peer child per scattered peer — the scatter-gather
    // timeline an agent sees through get_trace (#640).
    ASSERT_FALSE(result.trace_id.empty());
    auto fetched = call("get_trace", nlohmann::json{{"trace_id", result.trace_id}});
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    const nlohmann::json& spans = fetched.value.at("spans");
    EXPECT_EQ(count_spans_named(spans, "api:search"), 1u);
    EXPECT_EQ(count_spans_named(spans, "search"), 1u);
    EXPECT_EQ(count_spans_named(spans, "search:local"), 1u);
    EXPECT_EQ(count_spans_named(spans, "search:peer"), kNodes - 1);
    for (const nlohmann::json& span : spans) {
        if (span.at("name") == "api:search") {
            EXPECT_EQ(span.at("attributes").at("intent"),
                      "find the postgres articles");
            EXPECT_TRUE(span.at("ok").get<bool>());
        }
    }

    // Negative branch: a missing index and an empty query fail with
    // path-qualified reasons — the engine's verbatim, and the schema pointer.
    auto unknown = call("search", nlohmann::json{{"target", "nonexistent"},
                                                 {"query", "postgres"}});
    EXPECT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error.code, "engine_error");
    EXPECT_NE(unknown.error.message.find("nonexistent"), std::string::npos);

    auto empty_query = call("search", nlohmann::json{{"target", "articles"},
                                                     {"query", ""}});
    EXPECT_FALSE(empty_query.ok);
    EXPECT_EQ(empty_query.error.code, "invalid_input");
    EXPECT_NE(empty_query.error.message.find("/query"), std::string::npos);
}
