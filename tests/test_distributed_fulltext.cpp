#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster.hpp"
#include "zetta/database.hpp"
#include "zetta/index/distributed_fulltext.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/index/oramacore_fulltext_index.hpp"
#include "zetta/node.hpp"
#include "zetta/partition_writer.hpp"
#include "zetta/partitioner.hpp"
#include "zetta/table.hpp"

// Per-partition distributed full-text (#529, the engine half of TC-13 #503). Each
// node builds a full-text sub-index over the partitions it OWNS (compute-near-data),
// and a query is answered by scatter-gather + merge/rank across the sub-indexes. The
// acceptance is that the distributed ranking matches a single consolidated-index
// ORACLE. The merge + scatter-gather plumbing is proven engine-agnostically with a
// deterministic, partition-independent reference scorer (so it runs without the
// private oramacore-cpp submodule); the real BM25F path is exercised when the engine
// is compiled in (skipped otherwise, like the other oramacore tests).
namespace {

using namespace std::chrono_literals;

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::DistributedFulltextIndex;
using zetta::FulltextHit;
using zetta::FulltextQueryRequest;
using zetta::FulltextQueryResponse;
using zetta::IndexBackend;
using zetta::IndexDocument;
using zetta::IndexKind;
using zetta::IndexPartition;
using zetta::IndexQuery;
using zetta::IndexQueryResult;
using zetta::IndexSpec;
using zetta::IndexStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::OramaCoreFulltextIndex;
using zetta::RoundRobinPartitioner;
using zetta::Table;
using zetta::test::TempDir;

// ----- a deterministic, partition-INDEPENDENT reference scorer ----------------
//
// A doc's score for a query is its term frequency — the count of query tokens
// occurring in the doc's indexed text — which depends ONLY on the doc + the query,
// never on corpus statistics. That is what makes "build per-partition then merge"
// equal to "build one consolidated index" exactly, so it is a clean oracle for the
// distributed merge without the private BM25F engine. (Real BM25F is NOT
// partition-independent — its IDF depends on the corpus — which is why the
// real-engine acceptance below uses an unambiguous, tf-dominated corpus.)
std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            current.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

class TermFrequencyBackend final : public IndexBackend {
public:
    std::string name() const override { return "tf_reference"; }
    IndexKind kind() const override { return IndexKind::Search; }

    IndexStatus build(const IndexSpec& spec,
                      std::span<const IndexPartition> partitions) override {
        if (spec.kind != IndexKind::Search) {
            return IndexStatus::unsupported("tf_reference is search-only");
        }
        if (spec.fields.empty()) {
            return IndexStatus::invalid_argument("no fields");
        }
        Built built;
        for (const IndexPartition& partition : partitions) {
            for (const IndexDocument& document : partition.documents) {
                if (!document.doc.is_object()) continue;
                std::map<std::string, int> counts;
                for (const std::string& field : spec.fields) {
                    const auto value = document.doc.find(field);
                    if (value == document.doc.end() || !value->is_string()) continue;
                    for (const std::string& token :
                         tokenize(value->get<std::string>())) {
                        ++counts[token];
                    }
                }
                built.docs.push_back({document.id, std::move(counts)});
            }
        }
        indexes_[spec.key()] = std::move(built);
        return IndexStatus::success();
    }

    IndexQueryResult query(const IndexSpec& spec,
                           const IndexQuery& query) const override {
        IndexQueryResult result;
        const auto found = indexes_.find(spec.key());
        if (found == indexes_.end()) {
            result.status = IndexStatus::not_found("no index for " + spec.key());
            return result;
        }
        const std::vector<std::string> query_tokens =
            tokenize(query.term.is_string() ? query.term.get<std::string>()
                                            : query.term.dump());
        std::vector<FulltextHit> hits;
        for (const Doc& doc : found->second.docs) {
            double score = 0.0;
            for (const std::string& token : query_tokens) {
                const auto count = doc.counts.find(token);
                if (count != doc.counts.end()) score += count->second;
            }
            if (score > 0.0) hits.push_back({doc.id, score});
        }
        std::stable_sort(hits.begin(), hits.end(),
                         [](const FulltextHit& a, const FulltextHit& b) {
                             if (a.score != b.score) return a.score > b.score;
                             return a.row_id < b.row_id;
                         });
        if (query.top_k > 0 && hits.size() > query.top_k) hits.resize(query.top_k);
        for (const FulltextHit& hit : hits) {
            result.row_ids.push_back(hit.row_id);
            result.scores.push_back(hit.score);
        }
        result.status = IndexStatus::success();
        return result;
    }

    IndexStatus drop(const IndexSpec& spec) override {
        indexes_.erase(spec.key());
        return IndexStatus::success();
    }

private:
    struct Doc {
        std::string id;
        std::map<std::string, int> counts;  // token -> frequency in the doc
    };
    struct Built {
        std::vector<Doc> docs;
    };
    std::map<std::string, Built> indexes_;
};

// The consolidated-index ORACLE: one index over EVERY row of every table, queried
// once — the single source of truth the per-partition distributed query must match.
std::vector<std::string> consolidated_oracle(const std::vector<Table*>& tables,
                                             const IndexSpec& spec,
                                             const IndexQuery& query) {
    TermFrequencyBackend oracle;
    std::vector<IndexPartition> all;
    for (Table* table : tables) {
        for (IndexPartition& partition :
             zetta::read_index_partitions(table->store())) {
            all.push_back(std::move(partition));
        }
    }
    oracle.build(spec, all);
    return oracle.query(spec, query).row_ids;
}

NodeConfig node_config(const TempDir& dir) {
    NodeConfig cfg;
    cfg.data_dir = dir.path().string();
    return cfg;
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

// An article row: a title + a body the index tokenizes.
nlohmann::json article(const std::string& title, const std::string& body) {
    return nlohmann::json{{"title", title}, {"body", body}};
}

IndexSpec articles_spec(const std::string& backend) {
    return IndexSpec{"articles", IndexKind::Search, backend, {"title", "body"}};
}

IndexQuery text_query(const std::string& term, std::size_t top_k = 0) {
    IndexQuery query;
    query.term = term;
    query.top_k = top_k;
    return query;
}

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

// A corpus where "postgres" matches several rows across partitions with clearly
// separated term frequencies, so the global ranking is unambiguous (tf-dominated) —
// robust both for the tf reference AND for real BM25F's per-shard IDF variation.
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

}  // namespace

// ----- the pure merge ---------------------------------------------------------

TEST(DistributedFulltextMerge, RanksByScoreDescThenRowIdAsc) {
    const std::vector<std::vector<FulltextHit>> sources = {
        {{"r1", 3.0}, {"r2", 1.0}},
        {{"r3", 5.0}, {"r4", 1.0}},
    };
    const std::vector<FulltextHit> merged =
        zetta::merge_fulltext_hits(sources, /*top_k=*/0);
    // r3 (5) > r1 (3) > r2 (1) == r4 (1) -> the tie breaks on row id ascending.
    ASSERT_EQ(merged.size(), 4u);
    EXPECT_EQ(merged[0].row_id, "r3");
    EXPECT_EQ(merged[1].row_id, "r1");
    EXPECT_EQ(merged[2].row_id, "r2");  // tie with r4: r2 < r4
    EXPECT_EQ(merged[3].row_id, "r4");
}

TEST(DistributedFulltextMerge, DedupsByRowIdKeepingMaxScoreAndCapsTopK) {
    const std::vector<std::vector<FulltextHit>> sources = {
        {{"dup", 2.0}, {"a", 9.0}},
        {{"dup", 7.0}, {"b", 1.0}},  // "dup" appears twice — keep the higher (7)
    };
    const std::vector<FulltextHit> merged =
        zetta::merge_fulltext_hits(sources, /*top_k=*/2);
    ASSERT_EQ(merged.size(), 2u);  // capped to top 2
    EXPECT_EQ(merged[0].row_id, "a");
    EXPECT_DOUBLE_EQ(merged[0].score, 9.0);
    EXPECT_EQ(merged[1].row_id, "dup");
    EXPECT_DOUBLE_EQ(merged[1].score, 7.0);  // the max of the two duplicates
}

// ----- the scatter-gather wire boundary ---------------------------------------

TEST(DistributedFulltextWire, QueryRequestRoundTripsThroughTheSchema) {
    FulltextQueryRequest request;
    request.tag = "round-7";
    request.table = "articles";
    request.kind = IndexKind::Search;
    request.backend = "oramacore_fulltext";
    request.fields = {"title", "body"};
    request.field = "title";
    request.term = "postgres";
    request.top_k = 5;

    std::string error;
    const auto decoded =
        zetta::decode_fulltext_query(zetta::encode_fulltext_query(request), &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->table, "articles");
    EXPECT_EQ(decoded->kind, IndexKind::Search);
    EXPECT_EQ(decoded->fields, (std::vector<std::string>{"title", "body"}));
    EXPECT_EQ(decoded->term, nlohmann::json("postgres"));
    EXPECT_EQ(decoded->top_k, 5u);
    // The reconstructed spec/query are what a peer queries from.
    EXPECT_EQ(decoded->spec().key(), articles_spec("oramacore_fulltext").key());
    EXPECT_EQ(decoded->query().top_k, 5u);
}

TEST(DistributedFulltextWire, MalformedMessagesAreRejectedAtTheBoundary) {
    std::string error;
    EXPECT_FALSE(zetta::decode_fulltext_query("{not json", &error));
    // Missing required `term`.
    EXPECT_FALSE(zetta::decode_fulltext_query(
        R"({"table":"a","kind":"search","fields":["t"]})", &error));
    // Unknown kind (enum violation).
    EXPECT_FALSE(zetta::decode_fulltext_query(
        R"({"table":"a","kind":"bogus","fields":["t"],"term":"q"})", &error));
    // A non-scalar term (array) is rejected.
    EXPECT_FALSE(zetta::decode_fulltext_query(
        R"({"table":"a","kind":"search","fields":["t"],"term":[1,2]})", &error));

    // Hits response: a hit missing `score` is rejected.
    EXPECT_FALSE(zetta::decode_fulltext_hits(R"({"hits":[{"id":"r1"}]})", &error));
    EXPECT_TRUE(zetta::decode_fulltext_hits(R"({"hits":[]})", &error));  // empty ok
}

TEST(DistributedFulltextWire, HitsResponseRoundTrips) {
    FulltextQueryResponse response;
    response.from = "node-a";
    response.hits = {{"r1", 12.5}, {"r3", 2.0}};
    std::string error;
    const auto decoded =
        zetta::decode_fulltext_hits(zetta::encode_fulltext_hits(response), &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->from, "node-a");
    ASSERT_EQ(decoded->hits.size(), 2u);
    EXPECT_EQ(decoded->hits[0].row_id, "r1");
    EXPECT_DOUBLE_EQ(decoded->hits[0].score, 12.5);
}

// ----- single node, many local partitions: merge == consolidated oracle -------

TEST(DistributedFulltext, PerPartitionMergeMatchesConsolidatedOracleSingleNode) {
    TempDir dir;
    Node node(node_config(dir));
    node.start();

    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    // Spread the rows across 4 partitions (all owned by this node) so the query
    // genuinely merges hits from multiple per-partition sub-indexes.
    table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));
    for (const nlohmann::json& row : postgres_corpus()) table->add_row(row);
    const zetta::PartitionWriteResult written =
        zetta::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
    ASSERT_GT(written.partitions.size(), 1u);  // genuinely multi-partition

    // A single-node cluster (no peers): the component answers from its local
    // per-partition sub-indexes only.
    Cluster cluster(node.node_id(), cluster_config(47080, {}));
    DistributedFulltextIndex distributed(
        cluster, std::make_shared<TermFrequencyBackend>());

    const IndexSpec spec = articles_spec("tf_reference");
    const auto build = distributed.build_local(node, spec);
    ASSERT_TRUE(build.ok()) << build.status.message;
    EXPECT_EQ(build.partitions_indexed, written.partitions.size());
    EXPECT_EQ(build.rows_indexed, written.rows_written);
    EXPECT_GT(build.local_bytes_read, 0u);

    // The distributed (per-partition + merge) ranking equals the consolidated oracle.
    const IndexQuery query = text_query("postgres");
    const auto result = distributed.query(spec, query);
    ASSERT_TRUE(result.ok()) << result.status.message;
    const std::vector<std::string> oracle =
        consolidated_oracle({table.get()}, spec, query);
    EXPECT_EQ(result.row_ids, oracle);
    EXPECT_FALSE(oracle.empty());
    EXPECT_EQ(result.scores.size(), result.row_ids.size());

    // top_k caps the global ranking to the most relevant.
    const auto capped = distributed.query(spec, text_query("postgres", 2));
    ASSERT_TRUE(capped.ok());
    EXPECT_EQ(capped.row_ids,
              (std::vector<std::string>{oracle.front(), oracle.at(1)}));
}

// ----- multi-node loopback: scatter-gather == consolidated oracle -------------

namespace {

// One node of a distributed-fulltext loopback cluster: a Node holding a partitioned
// shard (its own owned partitions + blobs), a Cluster, and a DistributedFulltextIndex
// over a fresh reference scorer with its local sub-indexes built.
struct FulltextNode {
    FulltextNode(std::string cluster_id, std::uint16_t port,
                 const std::vector<std::string>& seeds,
                 const std::vector<nlohmann::json>& shard_rows)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          distributed(cluster, std::make_shared<TermFrequencyBackend>()) {
        node.start();
        auto database = node.create_database("data");
        EXPECT_TRUE(database);
        auto created = database->create_table("articles");
        EXPECT_TRUE(created);
        created->set_partitioner(std::make_shared<RoundRobinPartitioner>(3));
        for (const nlohmann::json& row : shard_rows) created->add_row(row);
        const zetta::PartitionWriteResult written =
            zetta::write_partitioned(node, *database, *created);
        EXPECT_TRUE(written.ok()) << written.error;
        table = created;
        const auto build =
            distributed.build_local(node, articles_spec("tf_reference"));
        EXPECT_TRUE(build.ok()) << build.status.message;
    }

    TempDir temp_dir;
    Node node;
    Cluster cluster;
    DistributedFulltextIndex distributed;  // declared after cluster (destructs first)
    std::shared_ptr<Table> table;
};

}  // namespace

TEST(DistributedFulltext, ScatterGatherMatchesConsolidatedOracleAcrossNodes) {
    constexpr std::size_t kNodes = 3;
    constexpr std::uint16_t kBasePort = 47100;

    // The full corpus, sharded round-robin across the nodes — each node holds a
    // disjoint slice as its own owned partitions.
    const std::vector<nlohmann::json> corpus = postgres_corpus();
    std::vector<std::vector<nlohmann::json>> shards(kNodes);
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        shards[i % kNodes].push_back(corpus[i]);
    }

    const std::vector<std::string> ids = {"ft-node-a", "ft-node-b", "ft-node-c"};
    std::vector<std::unique_ptr<FulltextNode>> nodes;
    for (std::size_t index = 0; index < kNodes; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<FulltextNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds,
            shards[index]));
    }

    for (auto& node : nodes) node->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    })) << "cluster did not converge";

    // The consolidated oracle over EVERY node's rows.
    const IndexSpec spec = articles_spec("tf_reference");
    const IndexQuery query = text_query("postgres");
    std::vector<Table*> tables;
    for (auto& node : nodes) tables.push_back(node->table.get());
    const std::vector<std::string> oracle = consolidated_oracle(tables, spec, query);
    ASSERT_FALSE(oracle.empty());

    // Every node, acting as the query coordinator, scatters to its peers and merges —
    // and each produces the IDENTICAL global ranking, equal to the oracle.
    std::vector<DistributedFulltextIndex::QueryResult> results(kNodes);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < kNodes; ++index) {
        threads.emplace_back([&, index] {
            results[index] = nodes[index]->distributed.query(spec, query, 5s);
        });
    }
    for (auto& thread : threads) thread.join();

    for (std::size_t index = 0; index < kNodes; ++index) {
        const auto& result = results[index];
        EXPECT_TRUE(result.ok()) << "node " << index << ": " << result.status.message;
        EXPECT_EQ(result.row_ids, oracle) << "node " << index << " disagreed";
        EXPECT_EQ(result.nodes_answered, kNodes) << "node " << index;
        EXPECT_GT(result.wire_bytes_moved, 0u) << "node " << index;
    }
}

// ----- the real BM25F engine (skipped when the submodule is absent) -----------

TEST(DistributedFulltext, BuildsRealFulltextSubIndexesAndRanksLikeOracle) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    TempDir dir;
    Node node(node_config(dir));
    node.start();

    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));
    for (const nlohmann::json& row : postgres_corpus()) table->add_row(row);
    const zetta::PartitionWriteResult written =
        zetta::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;

    Cluster cluster(node.node_id(), cluster_config(47090, {}));
    // Default backend = the real OramaCoreFulltextIndex (BM25F).
    DistributedFulltextIndex distributed(cluster);

    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    ASSERT_TRUE(distributed.build_local(node, spec).ok());

    // The single consolidated BM25F index over all rows = the reference oracle.
    OramaCoreFulltextIndex oracle_backend;
    std::vector<IndexPartition> all = zetta::read_index_partitions(table->store());
    ASSERT_TRUE(oracle_backend.build(spec, all).ok());

    const IndexQuery query = text_query("postgres");
    const auto distributed_result = distributed.query(spec, query);
    ASSERT_TRUE(distributed_result.ok()) << distributed_result.status.message;
    const auto oracle_result = oracle_backend.query(spec, query);
    ASSERT_TRUE(oracle_result.status.ok());

    // The tf-dominated corpus ranks unambiguously, so the per-partition scatter-gather
    // ranking equals the consolidated-index ranking (and the same set of rows match).
    EXPECT_EQ(distributed_result.row_ids, oracle_result.row_ids);
    EXPECT_FALSE(distributed_result.row_ids.empty());
}
