#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cases.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/index/distributed_fulltext.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/index/oramacore_fulltext_index.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"

// TC-13 (#503) — fulltext index build + query correctness, as a GUI-runnable case.
// This is the in-app counterpart of tests/test_distributed_fulltext.cpp (the #529
// engine half): build a full-text index over a table's PARTITIONS via the index
// seam (ADR-34) on a real multi-node loopback cluster — each node builds one
// sub-index per partition it OWNS (compute-near-data, no full-dataset pull) — then
// query by scatter-gather and assert the ranked results match a single
// consolidated-index ORACLE on every node.
//
// "via the oramacore-cpp backend" when the private submodule is compiled in
// (ZETTA_WITH_ORAMACORE): the real BM25F OramaCoreFulltextIndex is the backend AND
// the oracle. On a CPU-only checkout that lacks the submodule the engine degrades
// to Unsupported, so the case falls back to a deterministic, partition-INDEPENDENT
// term-frequency reference scorer — proving the same build-over-partitions +
// scatter-gather + merge-vs-oracle plumbing end to end while staying green. The
// corpus is tf-dominated so the global ranking is unambiguous either way (exact for
// the global top-k; the same robustness the gtest relies on). See docs/testing/test-suite.md
// and docs/indexing/distributed-fulltext.md.
namespace zetta::testkit {
namespace {

// Same tunable knobs the sibling distributed cases use.
std::size_t node_count_option(const TestOptions& options) {
    return static_cast<std::size_t>(
        std::min<long>(6, std::max<long>(1, options.get_int("nodes", 3))));
}

// ----- a deterministic, partition-INDEPENDENT reference scorer ----------------
//
// A doc's score for a query is its term frequency — the count of query tokens
// occurring in the doc's indexed text — which depends ONLY on the doc + the query,
// never on corpus statistics. That is what makes "build per-partition then merge"
// equal to "build one consolidated index" EXACTLY, so it is a clean oracle for the
// distributed merge without the private BM25F engine (the same reference the
// distributed-fulltext gtest uses). Real BM25F is not partition-independent — its
// IDF depends on the corpus — which is why the real-engine path below leans on an
// unambiguous, tf-dominated corpus instead.
std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char ch : text) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            current.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

constexpr std::string_view kReferenceBackend = "tf_reference";

class ReferenceFulltextBackend final : public zetta::index::IndexBackend {
public:
    std::string name() const override { return std::string(kReferenceBackend); }
    zetta::index::IndexKind kind() const override { return zetta::index::IndexKind::Search; }

    zetta::index::IndexStatus build(const zetta::index::IndexSpec& spec,
                      std::span<const zetta::index::IndexPartition> partitions) override {
        if (spec.kind != zetta::index::IndexKind::Search) {
            return zetta::index::IndexStatus::unsupported("tf_reference is search-only");
        }
        if (spec.fields.empty()) return zetta::index::IndexStatus::invalid_argument("no fields");
        Built built;
        for (const zetta::index::IndexPartition& partition : partitions) {
            for (const zetta::index::IndexDocument& document : partition.documents) {
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
        return zetta::index::IndexStatus::success();
    }

    zetta::index::IndexQueryResult query(const zetta::index::IndexSpec& spec,
                           const zetta::index::IndexQuery& query) const override {
        zetta::index::IndexQueryResult result;
        const auto found = indexes_.find(spec.key());
        if (found == indexes_.end()) {
            result.status = zetta::index::IndexStatus::not_found("no index for " + spec.key());
            return result;
        }
        const std::vector<std::string> query_tokens = tokenize(
            query.term.is_string() ? query.term.get<std::string>() : query.term.dump());
        std::vector<zetta::index::FulltextHit> hits;
        for (const Doc& doc : found->second.docs) {
            double score = 0.0;
            for (const std::string& token : query_tokens) {
                const auto count = doc.counts.find(token);
                if (count != doc.counts.end()) score += count->second;
            }
            if (score > 0.0) hits.push_back({doc.id, score});
        }
        std::stable_sort(hits.begin(), hits.end(),
                         [](const zetta::index::FulltextHit& a, const zetta::index::FulltextHit& b) {
                             if (a.score != b.score) return a.score > b.score;
                             return a.row_id < b.row_id;
                         });
        if (query.top_k > 0 && hits.size() > query.top_k) hits.resize(query.top_k);
        for (const zetta::index::FulltextHit& hit : hits) {
            result.row_ids.push_back(hit.row_id);
            result.scores.push_back(hit.score);
        }
        result.status = zetta::index::IndexStatus::success();
        return result;
    }

    zetta::index::IndexStatus drop(const zetta::index::IndexSpec& spec) override {
        indexes_.erase(spec.key());
        return zetta::index::IndexStatus::success();
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

// Make the chosen backend (real BM25F when the engine is compiled in, else the
// reference scorer) — used both per node and for the consolidated oracle so the
// rankings are directly comparable.
std::shared_ptr<zetta::index::IndexBackend> make_backend(bool real_engine) {
    if (real_engine) return std::make_shared<zetta::index::OramaCoreFulltextIndex>();
    return std::make_shared<ReferenceFulltextBackend>();
}

// The consolidated-index ORACLE: one index over EVERY row of every node's table,
// queried once — the single source of truth the per-partition distributed query
// must match.
std::vector<std::string> consolidated_oracle(
    const std::vector<std::shared_ptr<zetta::data::Table>>& tables, bool real_engine,
    const zetta::index::IndexSpec& spec, const zetta::index::IndexQuery& query) {
    const std::shared_ptr<zetta::index::IndexBackend> oracle = make_backend(real_engine);
    std::vector<zetta::index::IndexPartition> all;
    for (const std::shared_ptr<zetta::data::Table>& table : tables) {
        if (!table) continue;
        for (zetta::index::IndexPartition& partition : zetta::index::read_index_partitions(table->store())) {
            all.push_back(std::move(partition));
        }
    }
    oracle->build(spec, all);
    return oracle->query(spec, query).row_ids;
}

// An article row: a title + a body the index tokenizes.
nlohmann::json article(const std::string& title, const std::string& body) {
    return nlohmann::json{{"title", title}, {"body", body}};
}

// A corpus where "postgres" matches several rows with clearly separated term
// frequencies (5 / 3 / 2 / 1 / 0), so the global ranking is unambiguous
// (tf-dominated) — robust both for the reference scorer AND for real BM25F's
// per-shard IDF variation. `copies` repeats it (with distinct row ids assigned by
// the partitioned write) so the rows genuinely spread across several partitions per
// node; the relative tf separation — hence the ranking — is preserved.
std::vector<nlohmann::json> postgres_corpus(std::size_t copies) {
    const std::vector<nlohmann::json> base = {
        article("Postgres deep dive",
                "postgres postgres postgres postgres postgres tuning and scaling"),  // 5
        article("Postgres replication",
                "postgres postgres replication and postgres failover"),  // 3
        article("Scaling Postgres", "postgres postgres horizontal scaling"),  // 2
        article("Database indexes", "btree gin brin and postgres indexes"),  // 1
        article("Postgres backups", "postgres nightly backups explained"),  // 1
        article("Cooking pasta", "boil water then add salt and pasta"),  // 0
        article("Widget catalog", "a guide about widgets and gadgets"),  // 0
    };
    std::vector<nlohmann::json> corpus;
    corpus.reserve(base.size() * copies);
    for (std::size_t copy = 0; copy < copies; ++copy) {
        for (const nlohmann::json& row : base) corpus.push_back(row);
    }
    return corpus;
}

zetta::index::IndexSpec articles_spec(const std::string& backend) {
    return zetta::index::IndexSpec{"articles", zetta::index::IndexKind::Search, backend, {"title", "body"}};
}

zetta::index::IndexQuery text_query(const std::string& term, std::size_t top_k) {
    zetta::index::IndexQuery query;
    query.term = term;
    query.top_k = top_k;
    return query;
}

bool descending(const std::vector<double>& scores) {
    for (std::size_t i = 1; i < scores.size(); ++i) {
        if (scores[i] > scores[i - 1] + 1e-9) return false;
    }
    return true;
}

// Build a full-text index over a table's partitions and query it by scatter-gather
// across a real cluster, asserting the ranking matches a consolidated oracle (TC-13).
TestResult run_fulltext_index(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_count_option(options);
    const std::size_t copies = static_cast<std::size_t>(
        std::min<long>(64, std::max<long>(1, options.get_int("copies", 6))));
    const std::size_t partitions = static_cast<std::size_t>(
        std::min<long>(64, std::max<long>(1, options.get_int("partitions", 4))));
    const std::size_t top_k =
        static_cast<std::size_t>(std::max<long>(0, options.get_int("top_k", 0)));

    const bool real_engine = zetta::index::OramaCoreFulltextIndex::engine_available();
    const std::string backend_name =
        real_engine ? std::string(zetta::index::OramaCoreFulltextIndex::kName)
                    : std::string(kReferenceBackend);
    context.emit(real_engine
                     ? "using the real oramacore-cpp BM25F backend"
                     : "oramacore-cpp submodule absent — using the partition-"
                       "independent reference scorer");

    const std::vector<nlohmann::json> corpus = postgres_corpus(copies);

    // Bring up a real loopback cluster; each node holds a disjoint round-robin shard
    // of the corpus as its OWN owned partitions.
    context.emit("bringing up a " + std::to_string(nodes) + "-node loopback cluster");
    LoopbackCluster cluster(nodes);
    if (!cluster.connected()) return TestResult::fail("cluster did not converge");

    const zetta::index::IndexSpec spec = articles_spec(backend_name);

    context.emit("partitioning " + std::to_string(corpus.size()) +
                 " articles across " + std::to_string(nodes) +
                 " nodes (" + std::to_string(partitions) + " partitions each)");
    std::vector<std::shared_ptr<zetta::data::Table>> tables(nodes);
    std::uint64_t total_rows_written = 0;
    for (std::size_t i = 0; i < nodes; ++i) {
        Node& node = cluster.node(i);
        std::shared_ptr<zetta::data::Database> database = node.create_database("data");
        if (!database) {
            return TestResult::fail("create_database failed on node " + std::to_string(i));
        }
        std::shared_ptr<zetta::data::Table> table = database->create_table("articles");
        if (!table) {
            return TestResult::fail("create_table failed on node " + std::to_string(i));
        }
        table->set_partitioner(std::make_shared<zetta::data::RoundRobinPartitioner>(partitions));
        for (std::size_t r = i; r < corpus.size(); r += nodes) table->add_row(corpus[r]);
        const zetta::data::PartitionWriteResult written = write_partitioned(node, *database, *table);
        if (!written.ok()) {
            return TestResult::fail("write_partitioned failed on node " +
                                    std::to_string(i) + ": " + written.error);
        }
        total_rows_written += written.rows_written;
        tables[i] = table;
    }
    if (total_rows_written != corpus.size()) {
        return TestResult::fail("not every article was written across the shards");
    }

    // One DistributedFulltextIndex per node over the chosen backend; build the
    // per-partition sub-indexes near data. Declared after the cluster so they
    // unregister their signal handlers before it tears down.
    std::vector<std::unique_ptr<zetta::index::DistributedFulltextIndex>> indexes(nodes);
    std::size_t total_partitions_indexed = 0;
    std::uint64_t total_rows_indexed = 0;
    std::uint64_t total_local_bytes = 0;
    for (std::size_t i = 0; i < nodes; ++i) {
        indexes[i] = std::make_unique<zetta::index::DistributedFulltextIndex>(cluster.cluster(i),
                                                                make_backend(real_engine));
        const zetta::index::DistributedFulltextIndex::BuildResult build =
            indexes[i]->build_local(cluster.node(i), spec);
        if (!build.ok()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " build_local failed: " + build.status.message);
        }
        if (build.partitions_indexed == 0) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " indexed no owned partitions");
        }
        total_partitions_indexed += build.partitions_indexed;
        total_rows_indexed += build.rows_indexed;
        total_local_bytes += build.local_bytes_read;
    }
    if (total_rows_indexed != corpus.size()) {
        return TestResult::fail("indexed " + std::to_string(total_rows_indexed) +
                                " rows near data, expected " + std::to_string(corpus.size()));
    }
    if (total_local_bytes == 0) {
        return TestResult::fail("no partition bodies were read near data");
    }

    // The consolidated oracle over EVERY node's rows.
    const zetta::index::IndexQuery query = text_query("postgres", top_k);
    const std::vector<std::string> oracle =
        consolidated_oracle(tables, real_engine, spec, query);
    if (oracle.empty()) return TestResult::fail("the oracle matched no rows");

    // Every node, acting as the query coordinator, scatters to its peers and merges —
    // and each must produce the IDENTICAL global ranking, equal to the oracle.
    context.emit("scatter-gather querying \"postgres\" from every node");
    std::vector<zetta::index::DistributedFulltextIndex::QueryResult> results(nodes);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < nodes; ++i) {
        threads.emplace_back([&, i] {
            results[i] = indexes[i]->query(spec, query, std::chrono::seconds(8));
        });
    }
    for (auto& thread : threads) thread.join();

    std::uint64_t total_wire_bytes = 0;
    for (std::size_t i = 0; i < nodes; ++i) {
        const zetta::index::DistributedFulltextIndex::QueryResult& result = results[i];
        if (!result.ok()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " query failed: " + result.status.message);
        }
        if (result.row_ids != oracle) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " ranking disagreed with the consolidated oracle");
        }
        if (result.scores.size() != result.row_ids.size()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " returned scores misaligned with row ids");
        }
        if (!descending(result.scores)) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " scores were not ranked descending");
        }
        if (result.nodes_answered != nodes) {
            return TestResult::fail("node " + std::to_string(i) + " heard from " +
                                    std::to_string(result.nodes_answered) + " of " +
                                    std::to_string(nodes) + " nodes");
        }
        total_wire_bytes += result.wire_bytes_moved;
    }
    // A multi-node query genuinely moves hits over the wire (scatter + gather); a
    // single-node cluster answers locally and moves nothing.
    if (nodes > 1 && total_wire_bytes == 0) {
        return TestResult::fail("a multi-node scatter-gather moved no bytes over the wire");
    }

    TestResult result = TestResult::pass(
        "the per-partition full-text index built over each node's owned partitions; "
        "the scatter-gather ranking matched the consolidated oracle on every node");
    result.metric("backend", real_engine ? "oramacore_fulltext (BM25F)"
                                          : "tf_reference (oramacore-cpp submodule absent)")
        .metric("nodes", std::to_string(nodes))
        .metric("articles", std::to_string(corpus.size()))
        .metric("partitions indexed", std::to_string(total_partitions_indexed))
        .metric("matched + ranked", std::to_string(oracle.size()))
        .metric("read near data (bytes)", std::to_string(total_local_bytes))
        .metric("moved over wire (bytes)", std::to_string(total_wire_bytes));
    return result;
}

}  // namespace

void register_fulltext_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "cluster.fulltext_index",
        .category = TestCategory::Distributed,
        .name = "Fulltext index build + query (TC-13)",
        .description =
            "Build a full-text index over a table's partitions via the index seam "
            "(ADR-34) on a real multi-node cluster — each node builds one sub-index "
            "per partition it owns (compute-near-data) — then query by scatter-gather "
            "and assert the ranked results match a single consolidated-index oracle on "
            "every node. Uses the oramacore-cpp BM25F backend when compiled in, else a "
            "partition-independent reference scorer (the same plumbing, always green).",
        .options = {TestOption::integer("nodes", "Nodes", 3, 1, 6),
                    TestOption::integer("copies", "Corpus copies", 6, 1, 64,
                                        "Repeats the article corpus so the rows spread "
                                        "across more partitions per node."),
                    TestOption::integer("partitions", "Partitions per node", 4, 1, 64),
                    TestOption::integer("top_k", "Top-k (0 = all)", 0, 0, 1000)},
        .run = run_fulltext_index,
    });
}

}  // namespace zetta::testkit
