#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "term_frequency_backend.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/index/search.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

// The canonical distributed search entry point (#641): Node::search resolves the
// registered Index, routes through the per-partition distributed path (scatter-
// gather across the cluster) when the table has placed partitions, degrades to
// the consolidated registry backend when it has none, returns per-node
// provenance, and records the whole search as ONE trace (root `search` span +
// per-contribution children).
namespace {

using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::index::IndexKind;
using zetta::index::IndexQuery;
using zetta::index::IndexSpec;
using zetta::index::IndexValue;
using zetta::index::SearchPath;
using zetta::index::SearchRequest;
using zetta::index::SearchResult;
using zetta::Node;
using zetta::NodeConfig;
using zetta::data::RoundRobinPartitioner;
using zetta::data::Table;
using zetta::observability::Span;
using zetta::observability::SpanRecorder;
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

IndexValue articles_index(const std::string& backend) {
    IndexValue value;
    value.table = "articles";
    value.kind = IndexKind::Search;
    value.backend = backend;
    value.fields = {"title", "body"};
    return value;
}

// A corpus whose "postgres" matches have clearly separated term frequencies, so
// the global ranking is unambiguous (the test_distributed_fulltext.cpp corpus).
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

// The consolidated-index ORACLE over every table's rows (one index, one query).
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

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

std::size_t count_spans_named(const std::vector<Span>& spans,
                              const std::string& name) {
    return static_cast<std::size_t>(
        std::count_if(spans.begin(), spans.end(),
                      [&name](const Span& span) { return span.name == name; }));
}

}  // namespace

// ----- resolution + validation -------------------------------------------------

TEST(NodeSearch, RejectsEmptyOrUnknownTargets) {
    TempDir dir;
    Node node(node_config(dir));
    node.start();

    EXPECT_EQ(node.search(SearchRequest{}).status.code,
              zetta::index::IndexStatusCode::InvalidArgument);

    SearchRequest no_query;
    no_query.target = "articles";
    EXPECT_EQ(node.search(no_query).status.code,
              zetta::index::IndexStatusCode::InvalidArgument);

    SearchRequest unknown;
    unknown.target = "articles";
    unknown.query = "postgres";
    const SearchResult result = node.search(unknown);
    EXPECT_EQ(result.status.code, zetta::index::IndexStatusCode::NotFound);
    EXPECT_NE(result.status.message.find("articles"), std::string::npos);
}

TEST(NodeSearch, ResolvesTargetByIndexIdAndByTableName) {
    TempDir dir;
    Node node(node_config(dir));
    node.start();
    node.index_registry().add(std::make_shared<TermFrequencyBackend>());

    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    for (const nlohmann::json& row : postgres_corpus()) table->add_row(row);

    const auto index = node.create_index(articles_index("tf_reference"));
    ASSERT_TRUE(index);
    // Build the consolidated index the degraded path serves from.
    const IndexSpec spec = index->spec();
    ASSERT_TRUE(node.index_registry()
                    .resolve(spec)
                    ->build(spec, zetta::index::read_index_partitions(
                                      table->store()))
                    .ok());

    SearchRequest by_table;
    by_table.target = "articles";
    by_table.query = "postgres";
    const SearchResult from_table = node.search(by_table);
    ASSERT_TRUE(from_table.ok()) << from_table.status.message;
    EXPECT_EQ(from_table.index_id, index->id());

    SearchRequest by_id;
    by_id.target = index->id();
    by_id.query = "postgres";
    const SearchResult from_id = node.search(by_id);
    ASSERT_TRUE(from_id.ok()) << from_id.status.message;
    EXPECT_EQ(from_id.index_id, index->id());
    EXPECT_EQ(from_id.hits.size(), from_table.hits.size());
}

// ----- the degraded (consolidated) path -----------------------------------------

TEST(NodeSearch, UnpartitionedTableDegradesToConsolidatedWithSelfProvenance) {
    TempDir dir;
    Node node(node_config(dir));
    node.start();
    node.index_registry().add(std::make_shared<TermFrequencyBackend>());

    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    for (const nlohmann::json& row : postgres_corpus()) table->add_row(row);

    const auto index = node.create_index(articles_index("tf_reference"));
    ASSERT_TRUE(index);
    const IndexSpec spec = index->spec();
    ASSERT_TRUE(node.index_registry()
                    .resolve(spec)
                    ->build(spec, zetta::index::read_index_partitions(
                                      table->store()))
                    .ok());

    SpanRecorder recorder;
    SearchRequest request;
    request.target = "articles";
    request.query = "postgres";
    const SearchResult result = node.search(request, &recorder);
    ASSERT_TRUE(result.ok()) << result.status.message;

    // No placed partitions -> the consolidated registry backend answered.
    EXPECT_EQ(result.path, SearchPath::Consolidated);
    IndexQuery oracle_query;
    oracle_query.term = "postgres";
    const std::vector<std::string> oracle =
        consolidated_oracle({table.get()}, spec, oracle_query);
    ASSERT_FALSE(oracle.empty());
    ASSERT_EQ(result.hits.size(), oracle.size());
    for (std::size_t i = 0; i < oracle.size(); ++i) {
        EXPECT_EQ(result.hits[i].row_id, oracle[i]) << "rank " << i;
    }

    // Per-node provenance: exactly this node, marked local.
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes[0].node_id, node.node_id());
    EXPECT_TRUE(result.nodes[0].local);
    EXPECT_EQ(result.nodes[0].hits, result.hits.size());
    EXPECT_EQ(result.wire_bytes_moved, 0u);

    // One trace: the root `search` span + the `search:local` child.
    ASSERT_FALSE(result.trace_id.empty());
    const std::vector<Span> spans = recorder.trace(result.trace_id);
    EXPECT_EQ(count_spans_named(spans, "search"), 1u);
    EXPECT_EQ(count_spans_named(spans, "search:local"), 1u);
}

// ----- the distributed path: entry point == oracle, provenance, one trace ------

namespace {

// One node of the loopback cluster, driven ONLY through the canonical entry
// point: a Node holding a partitioned shard, its Cluster, the injected
// reference scorer, and the Search index entity registered — the sub-indexes
// are built by the node itself (reconcile_search_indexes via tick/search),
// never by the test.
struct SearchNode {
    SearchNode(std::string cluster_id, std::uint16_t port,
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
        EXPECT_TRUE(node.create_index(articles_index("tf_reference")));
    }

    ~SearchNode() { node.stop(); }  // tears the search plane down before cluster

    TempDir temp_dir;
    Node node;
    Cluster cluster;
    std::shared_ptr<Table> table;
};

}  // namespace

TEST(NodeSearch, DistributedSearchMatchesOracleWithProvenanceAndOneTrace) {
    constexpr std::size_t kNodes = 3;
    constexpr std::uint16_t kBasePort = 47140;

    // The full corpus, sharded round-robin — each node owns a disjoint slice.
    const std::vector<nlohmann::json> corpus = postgres_corpus();
    std::vector<std::vector<nlohmann::json>> shards(kNodes);
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        shards[i % kNodes].push_back(corpus[i]);
    }

    const std::vector<std::string> ids = {"ns-node-a", "ns-node-b", "ns-node-c"};
    std::vector<std::unique_ptr<SearchNode>> nodes;
    for (std::size_t index = 0; index < kNodes; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<SearchNode>(
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

    // The consolidated oracle over EVERY node's rows.
    IndexQuery query;
    query.term = "postgres";
    const IndexSpec spec = articles_index("tf_reference").spec();
    std::vector<Table*> tables;
    for (auto& entry : nodes) tables.push_back(entry->table.get());
    const std::vector<std::string> oracle =
        consolidated_oracle(tables, spec, query);
    ASSERT_FALSE(oracle.empty());

    // Every node, coordinating through THE entry point, produces the identical
    // global ranking — equal to the oracle — with full per-node provenance.
    for (std::size_t index = 0; index < kNodes; ++index) {
        SpanRecorder recorder;
        SearchRequest request;
        request.target = "articles";
        request.query = "postgres";
        const SearchResult result =
            nodes[index]->node.search(request, &recorder);
        ASSERT_TRUE(result.ok())
            << "node " << index << ": " << result.status.message;
        EXPECT_EQ(result.path, SearchPath::Distributed) << "node " << index;

        ASSERT_EQ(result.hits.size(), oracle.size()) << "node " << index;
        for (std::size_t rank = 0; rank < oracle.size(); ++rank) {
            EXPECT_EQ(result.hits[rank].row_id, oracle[rank])
                << "node " << index << " rank " << rank;
        }

        // Per-node provenance: all three nodes answered; exactly one (this
        // one) is local; the peers moved bytes over the wire.
        ASSERT_EQ(result.nodes.size(), kNodes) << "node " << index;
        std::size_t locals = 0;
        for (const zetta::index::NodeContribution& contribution : result.nodes) {
            EXPECT_FALSE(contribution.node_id.empty());
            if (contribution.local) {
                ++locals;
                // Provenance carries the signal-plane (cluster) id — the id a
                // production node shares with its Node (main passes one id to
                // both); this fixture gives the Cluster its own.
                EXPECT_EQ(contribution.node_id, nodes[index]->cluster.node_id());
                EXPECT_EQ(contribution.wire_bytes, 0u);
            } else {
                EXPECT_GT(contribution.wire_bytes, 0u);
            }
            EXPECT_GE(contribution.latency_ms, 0.0);
        }
        EXPECT_EQ(locals, 1u) << "node " << index;
        EXPECT_GT(result.wire_bytes_moved, 0u) << "node " << index;

        // ONE trace: the root `search` span, one `search:local` child, and a
        // `search:peer` child per scattered peer.
        ASSERT_FALSE(result.trace_id.empty());
        const std::vector<Span> spans = recorder.trace(result.trace_id);
        EXPECT_EQ(count_spans_named(spans, "search"), 1u) << "node " << index;
        EXPECT_EQ(count_spans_named(spans, "search:local"), 1u)
            << "node " << index;
        EXPECT_EQ(count_spans_named(spans, "search:peer"), kNodes - 1)
            << "node " << index;
    }
}
