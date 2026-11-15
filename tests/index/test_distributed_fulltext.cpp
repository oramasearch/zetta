// SPDX-License-Identifier: MIT

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
                 const std::vector<nlohmann::json>& shard_rows,
                 std::shared_ptr<IndexBackend> backend =
                     std::make_shared<TermFrequencyBackend>(),
                 const IndexSpec& spec = articles_spec("tf_reference"))
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          distributed(cluster, std::move(backend)) {
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
        const auto build = distributed.build_local(node, spec);
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

// ----- a limited search over a skewed corpus equals one index (#851) ----------
//
// The corpus the issue describes: the search term is concentrated in some rows
// and rare in others, so wherever the rows land, the shards' corpus statistics
// for it differ from the whole table's. Scored with each shard's own IDF, a lone
// weak match on a shard full of misses outscores a strong match on a shard full
// of matches, and a per-shard top_k cutoff silently drops a row that belongs in
// the global top k. With the global-statistics pass, a limited search over the
// sharded table equals the same search over one unsharded index — membership,
// order and scores — at every depth and under every distribution.

namespace {

// A 5-token body mentioning "postgres" `tf` times (fillers are not stop words and
// stem to themselves), so every body has the same length and only the corpus
// statistics differ between shards.
std::string postgres_body(int tf) {
    static const char* const kFillers[] = {"vacuum", "replica", "shard", "backup", "index"};
    std::string body;
    for (int i = 0; i < 5; ++i) {
        if (!body.empty()) body += ' ';
        body += i < tf ? "postgres" : kFillers[i];
    }
    return body;
}

// 20 rows. Under RoundRobinPartitioner(2) the even ordinals land together: one
// weak match (tf 1) among nine misses. The odd ordinals: four strong matches
// (tf 5/4/3/2) among six misses. Other partition counts and node splits skew it
// differently; the expectation never changes.
std::vector<nlohmann::json> skewed_corpus() {
    std::vector<nlohmann::json> even;
    std::vector<nlohmann::json> odd;
    even.push_back(article("notes", postgres_body(1)));
    for (int i = 0; i < 9; ++i) even.push_back(article("notes", "boil water pasta salt basil"));
    for (int tf = 5; tf >= 2; --tf) odd.push_back(article("notes", postgres_body(tf)));
    for (int i = 0; i < 6; ++i) odd.push_back(article("notes", "prune roses mulch beds weekly"));
    std::vector<nlohmann::json> rows;
    for (std::size_t i = 0; i < even.size(); ++i) {
        rows.push_back(even[i]);
        rows.push_back(odd[i]);
    }
    return rows;
}

// The consolidated BM25F oracle over `rows`, uncapped: (row ids, scores).
IndexQueryResult bm25_oracle(const std::vector<Table*>& tables, const IndexSpec& spec) {
    OramaCoreFulltextIndex oracle;
    std::vector<IndexPartition> all;
    for (Table* table : tables) {
        for (IndexPartition& partition : zetta::index::read_index_partitions(table->store())) {
            all.push_back(std::move(partition));
        }
    }
    EXPECT_TRUE(oracle.build(spec, all).ok());
    return oracle.query(spec, text_query("postgres"));
}

// `result` must be exactly the first `depth` rows of `oracle` (0 = all of them),
// with identical scores.
void expect_oracle_prefix(const DistributedFulltextIndex::QueryResult& result,
                          const IndexQueryResult& oracle, std::size_t depth,
                          const std::string& label) {
    ASSERT_TRUE(result.ok()) << label << ": " << result.status.message;
    const std::size_t expected =
        depth == 0 ? oracle.row_ids.size() : std::min(depth, oracle.row_ids.size());
    const std::vector<std::string> ids(oracle.row_ids.begin(),
                                       oracle.row_ids.begin() + expected);
    EXPECT_EQ(result.row_ids, ids) << label << " top " << depth;
    ASSERT_EQ(result.scores.size(), result.row_ids.size()) << label;
    for (std::size_t i = 0; i < result.scores.size() && i < expected; ++i) {
        EXPECT_EQ(result.scores[i], oracle.scores[i])
            << label << " top " << depth << ": score diverged at rank " << i;
    }
}

constexpr std::size_t kDepths[] = {1, 2, 3, 4, 5, 10, 50, 0};

}  // namespace

TEST(DistributedFulltext, LimitedSearchOverSkewedPartitionsEqualsOneIndex) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    std::uint16_t port = 48400;
    for (const std::size_t partitions : {2u, 3u, 4u, 7u}) {
        const std::string label = std::to_string(partitions) + " partitions";
        TempDir dir;
        Node node(node_config(dir));
        node.start();
        auto database = node.create_database("data");
        ASSERT_TRUE(database);
        auto table = database->create_table("articles");
        ASSERT_TRUE(table);
        table->set_partitioner(std::make_shared<RoundRobinPartitioner>(partitions));
        for (const nlohmann::json& row : skewed_corpus()) table->add_row(row);
        const zetta::data::PartitionWriteResult written =
            zetta::data::write_partitioned(node, *database, *table);
        ASSERT_TRUE(written.ok()) << written.error;

        Cluster cluster(node.node_id(), cluster_config(port++, {}));
        DistributedFulltextIndex distributed(cluster);  // the real BM25F backend
        ASSERT_TRUE(distributed.build_local(node, spec).ok()) << label;

        const IndexQueryResult oracle = bm25_oracle({table.get()}, spec);
        ASSERT_TRUE(oracle.status.ok());
        ASSERT_EQ(oracle.row_ids.size(), 5u);  // the five matching rows

        std::vector<std::string> deepest;
        for (const std::size_t depth : kDepths) {
            const auto result = distributed.query(spec, text_query("postgres", depth));
            expect_oracle_prefix(result, oracle, depth, label);
            // Consistent at every depth: a shallower answer is a prefix of a deeper.
            if (depth == 50) deepest = result.row_ids;
        }
        const auto top3 = distributed.query(spec, text_query("postgres", 3));
        ASSERT_GE(deepest.size(), 3u);
        EXPECT_EQ(top3.row_ids, std::vector<std::string>(deepest.begin(), deepest.begin() + 3))
            << label;
    }
}

TEST(DistributedFulltext, LimitedSearchOverSkewedNodesEqualsOneIndex) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    // Node A holds the weak match among misses, node B the strong matches: the
    // shape where one node holds most of the documents mentioning the term.
    const std::vector<nlohmann::json> corpus = skewed_corpus();
    std::vector<std::vector<nlohmann::json>> shards(2);
    for (std::size_t i = 0; i < corpus.size(); ++i) shards[i % 2].push_back(corpus[i]);

    constexpr std::uint16_t kBasePort = 48420;
    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    const std::vector<std::string> ids = {"skew-node-a", "skew-node-b"};
    std::vector<std::unique_ptr<FulltextNode>> nodes;
    for (std::size_t index = 0; index < shards.size(); ++index) {
        std::vector<std::string> seeds;
        if (index > 0) seeds.push_back("127.0.0.1:" + std::to_string(kBasePort));
        nodes.push_back(std::make_unique<FulltextNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds,
            shards[index], std::make_shared<OramaCoreFulltextIndex>(), spec));
    }
    for (auto& node : nodes) node->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    })) << "cluster did not converge";

    std::vector<Table*> tables;
    for (auto& node : nodes) tables.push_back(node->table.get());
    const IndexQueryResult oracle = bm25_oracle(tables, spec);
    ASSERT_TRUE(oracle.status.ok());
    ASSERT_EQ(oracle.row_ids.size(), 5u);

    // Every node, as coordinator, returns the oracle's top k at every depth.
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        for (const std::size_t depth : kDepths) {
            const auto result =
                nodes[index]->distributed.query(spec, text_query("postgres", depth), 5s);
            expect_oracle_prefix(result, oracle, depth, "coordinator " + ids[index]);
            EXPECT_EQ(result.nodes_answered, nodes.size());
        }
    }
}

// ----- the statistics round's pieces (#851) -----------------------------------

TEST(DistributedFulltextStatistics, MergeSumsEveryCount) {
    zetta::index::CorpusStatistics a;
    a.doc_count = 10;
    a.term_doc_freq = {{"postgr", 1}, {"vacuum", 0}};
    a.fields = {{"body", {50, 10}}, {"title", {10, 10}}};
    zetta::index::CorpusStatistics b;
    b.doc_count = 4;
    b.term_doc_freq = {{"postgr", 4}};
    b.fields = {{"body", {20, 3}}};
    a.merge(b);
    EXPECT_EQ(a.doc_count, 14u);
    EXPECT_EQ(a.term_doc_freq.at("postgr"), 5u);
    EXPECT_EQ(a.term_doc_freq.at("vacuum"), 0u);
    EXPECT_EQ(a.fields.at("body"), (zetta::index::FieldLengthStatistics{70, 13}));
    EXPECT_EQ(a.fields.at("title"), (zetta::index::FieldLengthStatistics{10, 10}));
}

TEST(DistributedFulltextWire, StatisticsTravelOnTheQueryAndTheStatsResponse) {
    zetta::index::CorpusStatistics statistics;
    statistics.doc_count = 20;
    statistics.term_doc_freq = {{"postgr", 5}};
    statistics.fields = {{"body", {100, 20}}, {"title", {20, 20}}};

    FulltextQueryRequest request;
    request.table = "articles";
    request.kind = IndexKind::Search;
    request.fields = {"title", "body"};
    request.term = "postgres";
    request.top_k = 2;
    request.statistics = statistics;
    std::string error;
    const auto decoded = zetta::index::decode_fulltext_query(
        zetta::index::encode_fulltext_query(request), &error);
    ASSERT_TRUE(decoded) << error;
    ASSERT_TRUE(decoded->statistics);
    EXPECT_EQ(*decoded->statistics, statistics);
    EXPECT_EQ(decoded->query().statistics, statistics);  // what the peer scores with

    // The stats request is the same message without statistics.
    request.statistics.reset();
    const auto plain = zetta::index::decode_fulltext_query(
        zetta::index::encode_fulltext_query(request), &error);
    ASSERT_TRUE(plain) << error;
    EXPECT_FALSE(plain->statistics);
    EXPECT_FALSE(plain->query().statistics);

    zetta::index::FulltextStatsResponse response;
    response.from = "node-b";
    response.statistics = statistics;
    response.sub_indexes = 2;
    const auto stats = zetta::index::decode_fulltext_stats(
        zetta::index::encode_fulltext_stats(response), &error);
    ASSERT_TRUE(stats) << error;
    EXPECT_EQ(stats->from, "node-b");
    EXPECT_EQ(stats->statistics, statistics);
    EXPECT_EQ(stats->sub_indexes, 2u);
}

TEST(DistributedFulltextWire, MalformedStatisticsAreRejectedAtTheBoundary) {
    std::string error;
    // A stats response must carry statistics.
    EXPECT_FALSE(zetta::index::decode_fulltext_stats(R"({"from":"n"})", &error));
    EXPECT_FALSE(zetta::index::decode_fulltext_stats("{not json", &error));
    // A negative count, a missing doc_count, a field missing doc_count.
    EXPECT_FALSE(zetta::index::decode_fulltext_stats(
        R"({"statistics":{"doc_count":-1,"term_doc_freq":{},"fields":{}}})", &error));
    EXPECT_FALSE(zetta::index::decode_fulltext_stats(
        R"({"statistics":{"term_doc_freq":{},"fields":{}}})", &error));
    EXPECT_FALSE(zetta::index::decode_fulltext_stats(
        R"({"statistics":{"doc_count":1,"term_doc_freq":{},"fields":{"body":{"total_len":5}}}})",
        &error));
    // A query request whose statistics carry a non-integer frequency.
    EXPECT_FALSE(zetta::index::decode_fulltext_query(
        R"({"table":"a","kind":"search","fields":["t"],"term":"q",)"
        R"("statistics":{"doc_count":1,"term_doc_freq":{"q":"many"},"fields":{}}})",
        &error));
    EXPECT_TRUE(zetta::index::decode_fulltext_stats(
        R"({"statistics":{"doc_count":0,"term_doc_freq":{},"fields":{}}})", &error))
        << error;
}

TEST(DistributedFulltextStatistics, ReferenceScorerSkipsTheStatisticsRound) {
    // A partition-independent scorer needs no statistics: no fulltext.stats round.
    TermFrequencyBackend reference;
    EXPECT_FALSE(reference.uses_corpus_statistics());
    if (OramaCoreFulltextIndex::engine_available()) {
        EXPECT_TRUE(OramaCoreFulltextIndex().uses_corpus_statistics());
    }
}

TEST(OramaCoreFulltextIndexOrder, CapCutsATieByRowIdNotInsertionOrder) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    // Three identical rows tie on score; they are inserted in descending row-id
    // order, so the engine's own (insertion-order) tie-break would keep "row-c".
    OramaCoreFulltextIndex backend;
    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    IndexPartition partition;
    partition.id = "p0";
    for (const char* id : {"row-c", "row-b", "row-a"}) {
        partition.documents.push_back(
            IndexDocument{id, article("Postgres", postgres_body(2))});
    }
    partition.documents.push_back(IndexDocument{"row-z", article("notes", "boil water pasta salt basil")});
    ASSERT_TRUE(backend.build(spec, std::span<const IndexPartition>(&partition, 1)).ok());

    const auto top1 = backend.query(spec, text_query("postgres", 1));
    ASSERT_TRUE(top1.status.ok());
    EXPECT_EQ(top1.row_ids, (std::vector<std::string>{"row-a"}));
    const auto top2 = backend.query(spec, text_query("postgres", 2));
    EXPECT_EQ(top2.row_ids, (std::vector<std::string>{"row-a", "row-b"}));
    const auto all = backend.query(spec, text_query("postgres", 0));
    EXPECT_EQ(all.row_ids, (std::vector<std::string>{"row-a", "row-b", "row-c"}));
}

TEST(OramaCoreFulltextIndexOrder, CollectsStatisticsAndScoresWithThem) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    // Two shards of the skewed corpus, built as separate indexes, plus the whole.
    const std::vector<nlohmann::json> corpus = skewed_corpus();
    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    IndexSpec spec_a = spec;
    spec_a.table = "articles-a";
    IndexSpec spec_b = spec;
    spec_b.table = "articles-b";
    IndexPartition a{"a", {}};
    IndexPartition b{"b", {}};
    IndexPartition whole{"all", {}};
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        IndexDocument document{"row-" + std::to_string(100 + i), corpus[i]};
        (i % 2 == 0 ? a : b).documents.push_back(document);
        whole.documents.push_back(document);
    }
    OramaCoreFulltextIndex backend;
    ASSERT_TRUE(backend.build(spec_a, std::span<const IndexPartition>(&a, 1)).ok());
    ASSERT_TRUE(backend.build(spec_b, std::span<const IndexPartition>(&b, 1)).ok());
    ASSERT_TRUE(backend.build(spec, std::span<const IndexPartition>(&whole, 1)).ok());

    IndexQuery query = text_query("postgres", 2);
    zetta::index::CorpusStatistics merged;
    ASSERT_TRUE(backend.collect_statistics(spec_a, query, merged).ok());
    ASSERT_TRUE(backend.collect_statistics(spec_b, query, merged).ok());
    zetta::index::CorpusStatistics consolidated;
    ASSERT_TRUE(backend.collect_statistics(spec, query, consolidated).ok());
    EXPECT_EQ(merged, consolidated);
    EXPECT_EQ(merged.doc_count, 20u);

    // Scored with the merged statistics, shard B's top 2 are the whole table's top 2.
    query.statistics = merged;
    const auto shard_b = backend.query(spec_b, query);
    const auto oracle = backend.query(spec, text_query("postgres", 2));
    ASSERT_TRUE(shard_b.status.ok());
    EXPECT_EQ(shard_b.row_ids, oracle.row_ids);
    EXPECT_EQ(shard_b.scores, oracle.scores);

    // Unknown index / bad field: the same statuses as query().
    zetta::index::CorpusStatistics unused;
    IndexSpec missing = spec;
    missing.table = "nope";
    EXPECT_EQ(backend.collect_statistics(missing, query, unused).code,
              zetta::index::IndexStatusCode::NotFound);
    IndexQuery bad_field = text_query("postgres");
    bad_field.field = "author";
    EXPECT_EQ(backend.collect_statistics(spec, bad_field, unused).code,
              zetta::index::IndexStatusCode::InvalidArgument);
}

TEST(OramaCoreFulltextIndexOrder, AFieldQueryMergesAndScoresThatFieldAlone) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    // "postgres" sits in the title of some rows and the body of others. A query
    // naming `body` searches the body's own index (lake#782), so its statistics,
    // gathered per shard and merged, must be the body's alone (#851), and a shard
    // scored with them must rank exactly as the whole table's body query.
    std::vector<nlohmann::json> corpus;
    for (int i = 0; i < 4; ++i) corpus.push_back(article("Postgres tuning", "boil water pasta salt"));
    for (int tf = 4; tf >= 1; --tf) corpus.push_back(article("notes", postgres_body(tf)));
    for (int i = 0; i < 4; ++i) corpus.push_back(article("notes", "prune roses mulch beds"));
    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    IndexSpec spec_a = spec;
    spec_a.table = "articles-a";
    IndexSpec spec_b = spec;
    spec_b.table = "articles-b";
    IndexPartition a{"a", {}};
    IndexPartition b{"b", {}};
    IndexPartition whole{"all", {}};
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        IndexDocument document{"row-" + std::to_string(100 + i), corpus[i]};
        (i % 2 == 0 ? a : b).documents.push_back(document);
        whole.documents.push_back(document);
    }
    OramaCoreFulltextIndex backend;
    ASSERT_TRUE(backend.build(spec_a, std::span<const IndexPartition>(&a, 1)).ok());
    ASSERT_TRUE(backend.build(spec_b, std::span<const IndexPartition>(&b, 1)).ok());
    ASSERT_TRUE(backend.build(spec, std::span<const IndexPartition>(&whole, 1)).ok());

    IndexQuery query = text_query("postgres", 2);
    query.field = "body";
    zetta::index::CorpusStatistics merged;
    ASSERT_TRUE(backend.collect_statistics(spec_a, query, merged).ok());
    ASSERT_TRUE(backend.collect_statistics(spec_b, query, merged).ok());
    zetta::index::CorpusStatistics consolidated;
    ASSERT_TRUE(backend.collect_statistics(spec, query, consolidated).ok());
    EXPECT_EQ(merged, consolidated);
    EXPECT_EQ(merged.doc_count, corpus.size());
    // Only the body's lengths: the title is not part of a body query's scoring.
    EXPECT_FALSE(merged.fields.contains("title"));
    EXPECT_TRUE(merged.fields.contains("body"));
    // Four rows carry "postgres" in the body; the title matches do not count.
    ASSERT_EQ(merged.term_doc_freq.size(), 1u);
    EXPECT_EQ(merged.term_doc_freq.begin()->second, 4u);

    IndexQuery oracle_query = text_query("postgres", 2);
    oracle_query.field = "body";
    const auto oracle = backend.query(spec, oracle_query);
    ASSERT_TRUE(oracle.status.ok());
    ASSERT_EQ(oracle.row_ids.size(), 2u);
    query.statistics = merged;
    for (const IndexSpec* shard : {&spec_a, &spec_b}) {
        const auto ranked = backend.query(*shard, query);
        ASSERT_TRUE(ranked.status.ok());
        for (std::size_t i = 0; i < ranked.row_ids.size(); ++i) {
            const auto at = std::find(oracle.row_ids.begin(), oracle.row_ids.end(),
                                      ranked.row_ids[i]);
            if (at == oracle.row_ids.end()) continue;  // below the global top 2
            EXPECT_EQ(ranked.scores[i], oracle.scores[at - oracle.row_ids.begin()])
                << ranked.row_ids[i];
        }
    }
}

TEST(OramaCoreFulltextIndexOrder, CapKeepsTheBestHitsInAnyInsertionOrder) {
    if (!OramaCoreFulltextIndex::engine_available()) {
        GTEST_SKIP() << "oramacore-cpp engine not compiled in (submodule absent)";
    }
    // Weakest-first insertion makes the engine meet the hits in ascending score
    // order; a capped query must still return the best ones (the engine's bounded
    // heap used to evict the best kept hit, dropping a top-k row silently).
    OramaCoreFulltextIndex backend;
    const IndexSpec spec = articles_spec(std::string(OramaCoreFulltextIndex::kName));
    IndexPartition partition;
    partition.id = "p0";
    for (int tf = 1; tf <= 5; ++tf) {
        partition.documents.push_back(
            IndexDocument{"row-" + std::to_string(tf), article("notes", postgres_body(tf))});
    }
    ASSERT_TRUE(backend.build(spec, std::span<const IndexPartition>(&partition, 1)).ok());
    const auto all = backend.query(spec, text_query("postgres", 0));
    ASSERT_EQ(all.row_ids,
              (std::vector<std::string>{"row-5", "row-4", "row-3", "row-2", "row-1"}));
    for (std::size_t k = 1; k <= 5; ++k) {
        const auto top = backend.query(spec, text_query("postgres", k));
        EXPECT_EQ(top.row_ids, std::vector<std::string>(all.row_ids.begin(),
                                                         all.row_ids.begin() + k))
            << "top " << k;
    }
}
