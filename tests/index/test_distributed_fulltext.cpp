#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "term_frequency_backend.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/index/distributed_fulltext.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/index/oramacore_fulltext_index.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"

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

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::index::DistributedFulltextIndex;
using zetta::index::FulltextHit;
using zetta::index::FulltextQueryRequest;
using zetta::index::FulltextQueryResponse;
using zetta::index::IndexBackend;
using zetta::index::IndexDocument;
using zetta::index::IndexKind;
using zetta::index::IndexPartition;
using zetta::index::IndexQuery;
using zetta::index::IndexQueryResult;
using zetta::index::IndexSpec;
using zetta::index::IndexStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::index::OramaCoreFulltextIndex;
using zetta::data::RoundRobinPartitioner;
using zetta::data::Table;
using zetta::test::TempDir;

// The deterministic, partition-independent reference scorer (see the header's
// comment for why it is a clean oracle for the distributed merge).
using zetta::test::TermFrequencyBackend;
using zetta::test::tokenize;

// The consolidated-index ORACLE: one index over EVERY row of every table, queried
// once — the single source of truth the per-partition distributed query must match.
std::vector<std::string> consolidated_oracle(const std::vector<Table*>& tables,
                                             const IndexSpec& spec,
                                             const IndexQuery& query) {
    TermFrequencyBackend oracle;
    std::vector<IndexPartition> all;
    for (Table* table : tables) {
        for (IndexPartition& partition :
             zetta::index::read_index_partitions(table->store())) {
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

// A corpus engineered so the distributed (per-shard local-IDF) ranking equals the
// consolidated oracle BY CONSTRUCTION, not by luck (#611). Local-IDF query-then-fetch
// is exact only when every shard scores with the same corpus statistics for the query
// term; "clearly separated term frequencies" alone is NOT enough — BM25F's k1
// saturation compresses any tf separation into a bounded score ratio (~2.2x at
// k1 = 1.2), while pairing a matching row with a non-matching one boosts a shard's
// IDF ~3.8x over a shard whose rows all match. So this corpus pins the statistics
// themselves:
//
//   - 8 rows over RoundRobinPartitioner(4) (partition = row ordinal % 4): rows 0-3
//     match "postgres" (total tf 4/3/2/1), rows 4-7 do not, so EVERY partition holds
//     exactly one matching + one non-matching row -> per-partition df/N == 1/2 ==
//     the consolidated df/N (4/8), and IDF is identical everywhere (ln 2).
//   - Every title is 2 tokens and every body 5 tokens after the engine's
//     tokenization (no stop words: "and"/"the"/... would change field lengths), so
//     every average field length — per partition and consolidated — is identical
//     and the BM25F length normalization cancels.
//
// With IDF and length norms equal everywhere, a row's score depends only on its own
// term frequencies — partition-independent, so distributed scores EQUAL the oracle's
// and the ranking follows the distinct tf totals 4 > 3 > 2 > 1 (no ties). The tf
// reference scorer sees the same distinct totals, so the corpus is unambiguous for
// it too.
std::vector<nlohmann::json> postgres_corpus() {
    return {
        // Rows 0-3: the matching rows, one per partition ("postgres" total tf 4/3/2/1).
        article("Postgres tuning", "postgres postgres postgres vacuum analyze"),
        article("Postgres replication", "postgres postgres failover replica streaming"),
        article("Postgres scaling", "postgres shard cluster horizontal capacity"),
        article("Postgres backups", "nightly backup snapshot restore archive"),
        // Rows 4-7: the non-matching rows, one per partition (same field lengths).
        article("Cooking pasta", "boil water pasta salt basil"),
        article("Widget catalog", "widgets gadgets sprockets pulleys levers"),
        article("Garden tips", "prune roses mulch beds weekly"),
        article("Travel notes", "pack light book early trains"),
    };
}

// True when a row of the corpus above matches the "postgres" query (used to assert
// the engineered one-matching-row-per-partition invariant explicitly).
bool matches_postgres(const nlohmann::json& doc) {
    for (const char* field : {"title", "body"}) {
        const auto value = doc.find(field);
        if (value == doc.end() || !value->is_string()) continue;
        for (const std::string& token : tokenize(value->get<std::string>())) {
            if (token == "postgres") return true;
        }
    }
    return false;
}

}  // namespace

// ----- the pure merge ---------------------------------------------------------

TEST(DistributedFulltextMerge, RanksByScoreDescThenRowIdAsc) {
    const std::vector<std::vector<FulltextHit>> sources = {
        {{"r1", 3.0}, {"r2", 1.0}},
        {{"r3", 5.0}, {"r4", 1.0}},
    };
    const std::vector<FulltextHit> merged =
        zetta::index::merge_fulltext_hits(sources, /*top_k=*/0);
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
        zetta::index::merge_fulltext_hits(sources, /*top_k=*/2);
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
        zetta::index::decode_fulltext_query(zetta::index::encode_fulltext_query(request), &error);
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
    EXPECT_FALSE(zetta::index::decode_fulltext_query("{not json", &error));
    // Missing required `term`.
    EXPECT_FALSE(zetta::index::decode_fulltext_query(
        R"({"table":"a","kind":"search","fields":["t"]})", &error));
    // Unknown kind (enum violation).
    EXPECT_FALSE(zetta::index::decode_fulltext_query(
        R"({"table":"a","kind":"bogus","fields":["t"],"term":"q"})", &error));
    // A non-scalar term (array) is rejected.
    EXPECT_FALSE(zetta::index::decode_fulltext_query(
        R"({"table":"a","kind":"search","fields":["t"],"term":[1,2]})", &error));

    // Hits response: a hit missing `score` is rejected.
    EXPECT_FALSE(zetta::index::decode_fulltext_hits(R"({"hits":[{"id":"r1"}]})", &error));
    EXPECT_TRUE(zetta::index::decode_fulltext_hits(R"({"hits":[]})", &error));  // empty ok
}

TEST(DistributedFulltextWire, HitsResponseRoundTrips) {
    FulltextQueryResponse response;
    response.from = "node-a";
    response.hits = {{"r1", 12.5}, {"r3", 2.0}};
    response.sub_indexes = 3;
    std::string error;
    const auto decoded =
        zetta::index::decode_fulltext_hits(zetta::index::encode_fulltext_hits(response), &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->from, "node-a");
    ASSERT_EQ(decoded->hits.size(), 2u);
    EXPECT_EQ(decoded->hits[0].row_id, "r1");
    EXPECT_DOUBLE_EQ(decoded->hits[0].score, 12.5);
    EXPECT_EQ(decoded->sub_indexes, 3u);

    // A payload without the optional sub_indexes (an older peer) decodes to 0.
    const auto legacy = zetta::index::decode_fulltext_hits(
        R"({"from":"node-b","hits":[{"id":"r1","score":1.0}]})", &error);
    ASSERT_TRUE(legacy) << error;
    EXPECT_EQ(legacy->sub_indexes, 0u);
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
    const zetta::data::PartitionWriteResult written =
        zetta::data::write_partitioned(node, *database, *table);
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
        const zetta::data::PartitionWriteResult written =
            zetta::data::write_partitioned(node, *database, *created);
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
    const zetta::data::PartitionWriteResult written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;

    // Assert the engineered fixture invariant the exact-ranking acceptance rests on
    // (see postgres_corpus): every partition holds exactly one matching and one
    // non-matching row, so per-partition BM25F statistics equal the consolidated
    // ones. If placement ever changes shape, fail HERE with the reason rather than
    // as a mysterious ranking flip below (#611).
    ASSERT_EQ(written.partitions.size(), 4u);
    for (const auto& partition : written.partitions) {
        const std::optional<std::string> body =
            database->large_objects().read(partition->blob_ref());
        ASSERT_TRUE(body) << "partition body missing: " << partition->blob_ref();
        const zetta::data::PartitionBodyReadResult read =
            zetta::data::read_partition_body(*body, partition->id());
        ASSERT_TRUE(read.ok()) << read.error;
        std::size_t rows = 0;
        std::size_t matching = 0;
        for (const zetta::data::PartitionRow& row : read.rows) {
            ++rows;
            if (matches_postgres(row.doc)) ++matching;
        }
        EXPECT_EQ(rows, 2u) << "partition '" << partition->id()
                            << "' lost the engineered one-match-one-miss shape";
        EXPECT_EQ(matching, 1u) << "partition '" << partition->id()
                                << "' lost the engineered one-match-one-miss shape";
    }

    Cluster cluster(node.node_id(), cluster_config(47090, {}));
    // Default backend = the real OramaCoreFulltextIndex (BM25F).
    DistributedFulltextIndex distributed(cluster);

    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    ASSERT_TRUE(distributed.build_local(node, spec).ok());

    // The single consolidated BM25F index over all rows = the reference oracle.
    OramaCoreFulltextIndex oracle_backend;
    std::vector<IndexPartition> all = zetta::index::read_index_partitions(table->store());
    ASSERT_TRUE(oracle_backend.build(spec, all).ok());

    const IndexQuery query = text_query("postgres");
    const auto distributed_result = distributed.query(spec, query);
    ASSERT_TRUE(distributed_result.ok()) << distributed_result.status.message;
    const auto oracle_result = oracle_backend.query(spec, query);
    ASSERT_TRUE(oracle_result.status.ok());

    // Per-partition statistics equal the consolidated ones by construction (the
    // fixture invariant asserted above), so the distributed scatter-gather ranking —
    // and each row's BM25F score — EQUALS the consolidated index's exactly.
    EXPECT_EQ(distributed_result.row_ids, oracle_result.row_ids);
    ASSERT_EQ(distributed_result.row_ids.size(), 4u);  // the 4 matching rows
    ASSERT_EQ(oracle_result.scores.size(), distributed_result.scores.size());
    for (std::size_t i = 0; i < distributed_result.scores.size(); ++i) {
        EXPECT_NEAR(distributed_result.scores[i], oracle_result.scores[i], 1e-9)
            << "score diverged at rank " << i;
    }
}

// ----- a hit's row fields travel with it (#1243) -----------------------------

TEST(DistributedFulltextWire, ReturnFieldsAndHitFieldsRoundTrip) {
    FulltextQueryRequest request;
    request.table = "articles";
    request.kind = IndexKind::Search;
    request.fields = {"title", "body"};
    request.term = "postgres";
    request.return_fields = std::vector<std::string>{"title"};
    std::string error;
    const auto decoded = zetta::index::decode_fulltext_query(
        zetta::index::encode_fulltext_query(request), &error);
    ASSERT_TRUE(decoded) << error;
    ASSERT_TRUE(decoded->return_fields);
    EXPECT_EQ(*decoded->return_fields, (std::vector<std::string>{"title"}));

    // An older coordinator sends no return_fields: ids and scores only.
    request.return_fields.reset();
    const auto legacy = zetta::index::decode_fulltext_query(
        zetta::index::encode_fulltext_query(request), &error);
    ASSERT_TRUE(legacy) << error;
    EXPECT_FALSE(legacy->return_fields);

    FulltextQueryResponse response;
    response.hits = {{"r1", 2.0, nlohmann::json{{"title", "Postgres"}}}, {"r2", 1.0}};
    const auto hits = zetta::index::decode_fulltext_hits(
        zetta::index::encode_fulltext_hits(response), &error);
    ASSERT_TRUE(hits) << error;
    EXPECT_EQ(hits->hits[0].fields, (nlohmann::json{{"title", "Postgres"}}));
    EXPECT_TRUE(hits->hits[1].fields.is_null());
}

TEST(DistributedFulltextMerge, KeepsTheFieldsOfTheHitItKeeps) {
    const std::vector<std::vector<FulltextHit>> sources = {
        {{"r1", 1.0, nlohmann::json{{"title", "low"}}}},
        {{"r1", 3.0, nlohmann::json{{"title", "high"}}},
         {"r2", 2.0, nlohmann::json{{"title", "other"}}}},
    };
    const std::vector<FulltextHit> merged = zetta::index::merge_fulltext_hits(sources);
    ASSERT_EQ(merged.size(), 2u);
    EXPECT_EQ(merged[0].row_id, "r1");
    EXPECT_EQ(merged[0].fields, (nlohmann::json{{"title", "high"}}));
    EXPECT_EQ(merged[1].fields, (nlohmann::json{{"title", "other"}}));
}
