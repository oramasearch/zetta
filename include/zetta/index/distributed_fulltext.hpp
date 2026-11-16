// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/index/index_backend.hpp"
#include "zetta/cluster/signal.hpp"
#include "zetta/observability/trace.hpp"

// Per-partition distributed full-text search (ENT-39 / ENT-40, ADR-34, #529 — the
// engine half of TC-13 #503). Where the [index step executor](index_step_executor.hpp)
// (#528) builds **one consolidated** index over a whole table, this is the
// **approved per-partition topology**: each node builds a full-text sub-index over
// the partitions it physically OWNS (compute-near-data, the same shard source as
// zetta::ddf::load_local_partitions), and a query is answered by **scatter-gather +
// merge/rank** across those sub-indexes — never one big index.
//
// This is the standard sharded-search shape (per-shard scoring, then a
// coordinator merges the ranked hits by score), with a **global-statistics pass**
// first (#851, the dfs_query_then_fetch analogue) when the backend scores with
// corpus statistics (BM25F's IDF and average field lengths): the coordinator
// gathers every sub-index's statistics for the query (`fulltext.stats`), merges
// them, and sends the merge with the query, so every sub-index scores exactly as
// one consolidated index over the whole table would. Without it a sub-index
// ranked against its own statistics, and a per-shard top_k cutoff could silently
// drop a row that belonged in the global top k. The contract: the result —
// membership, order and scores — equals one consolidated index's at any top_k,
// under any distribution of rows, and every coordinator returns the same list.
// See docs/indexing/distributed-fulltext.md.
//
//   build_local(node, spec)  — enumerate this node's owned partitions of the table,
//                              read each body from local disk (no LOREQ, no
//                              full-dataset pull), and build ONE sub-index per
//                              partition into an owned IndexBackend.
//   query(spec, query)       — gather every node's corpus statistics for the query
//                              over the `fulltext.stats` RPC and merge them (when the
//                              backend uses them), then query THIS node's local
//                              sub-indexes and scatter the query, merged statistics
//                              attached, to every connected peer over the
//                              `fulltext.query` signal-plane RPC, gather their ranked
//                              hits, and merge everything into one global ranked list
//                              (identical on every node).
//
// **Wiring.** A DistributedFulltextIndex is the cluster's SignalHandler while it
// lives, chained over an optional delegate (the DistributedSorter pattern):
// `fulltext.query` and `fulltext.stats` are served here, everything else routes to
// the delegate. They ride the existing transport — no new wire verb — and the
// request/response payloads are **schema-validated at the boundary**
// (schemas/fulltext_query.schema.json / schemas/fulltext_hits.schema.json /
// schemas/fulltext_stats.schema.json, the same pboettch validator as the collective
// message), so a malformed message fails fast with a path-qualified reason.
//
// Non-throwing throughout (zetta-core's no-exceptions convention): every entry point
// returns a status alongside its result. See docs/indexing/distributed-fulltext.md.
namespace zetta::cluster { class Cluster; }
namespace zetta { class Node; }

namespace zetta::index {

// (higher = better). The unit the scatter-gather merges across partitions / nodes.
struct FulltextHit {
    std::string row_id;
    double score = 0.0;
    // The matched row's values for the requested fields (#1243), filled by the
    // node that owns the row's partition. Null when fields were not requested.
    nlohmann::json fields;

    bool operator==(const FulltextHit&) const = default;
};

// One node's part in a scatter-gather query — the per-node provenance the
// observability foundation wants visible (#641): which node answered, how many
// ranked hits it contributed (pre-merge, capped to the request's top_k), how long
// its answer took from the coordinator's side, and how many payload bytes its
// round trip moved (0 for the coordinator's own local contribution).
struct NodeContribution {
    std::string node_id;
    bool local = false;           // true for the coordinating node itself
    std::size_t hits = 0;         // ranked hits this node contributed (pre-merge)
    std::size_t sub_indexes = 0;  // per-partition sub-indexes the node queried
                                  //   (1 on the consolidated path; 0 = none owned)
    double latency_ms = 0.0;      // coordinator-observed time to answer
    std::uint64_t wire_bytes = 0; // request + response payload bytes (0 = local)

    bool operator==(const NodeContribution&) const = default;
};

// Merge per-source ranked hit lists into one globally-ranked list. Flattens every
// source, dedups by row id (a row id should appear in at most one partition, but a
// duplicate is collapsed to its highest score so it is never listed twice or
// double-counted), then orders **score descending, row id ascending** on a tie —
// deterministic + reproducible — and caps to `top_k` (0 = no cap). This is the pure
// core both the local (across a node's partitions) and global (across nodes) merges
// run through, so single-node and multi-node produce identical rankings.
std::vector<FulltextHit> merge_fulltext_hits(
    std::span<const std::vector<FulltextHit>> sources, std::size_t top_k = 0);

// --- the scatter-gather wire messages ---------------------------------------
//
// Exposed (like encode/decode_collective_message) so the schema boundary is
// directly testable; otherwise an internal protocol detail.

// The scatter request: which per-partition full-text index to query (its IndexSpec)
// plus the query itself. A peer matches it to the sub-indexes it built (by
// IndexSpec::key()) and answers from those.
struct FulltextQueryRequest {
    std::string tag;                  // optional correlation/round id
    std::string table;                // the index's source table
    IndexKind kind = IndexKind::Search;
    std::string backend;              // the backend selector (provenance/validation)
    std::vector<std::string> fields;  // the indexed fields (>= 1)
    std::string field;                // optional named field (one field to match)
    nlohmann::json term;              // the free-text query value (scalar)
    // Per-shard cap (0 = no cap). Safe to apply per shard because every shard
    // scores with the same merged `statistics` (#851): a row in the global top k
    // is in its own shard's top k.
    std::size_t top_k = 0;
    // The row fields each hit should carry back (#1243): the owning node
    // attaches them, so a row is never fetched from another node. nullopt = ids
    // and scores only.
    std::optional<std::vector<std::string>> return_fields;
    // The merged corpus statistics every shard scores with (#851). Set on the
    // `fulltext.query` scatter after the `fulltext.stats` round; absent on the
    // `fulltext.stats` request itself and for a backend that does not use them.
    std::optional<CorpusStatistics> statistics;

    // The database authorized by the coordinator. A peer must have built its
    // index from this same database (#1403).
    std::optional<std::string> database;

    // The IndexSpec this request names (table/kind/backend/fields) — what a peer
    // looks its sub-indexes up by.
    IndexSpec spec() const;
    // The IndexQuery this request names (field/term/top_k/statistics).
    IndexQuery query() const;
};

// Serialize a scatter request to its compact JSON form.
std::string encode_fulltext_query(const FulltextQueryRequest& request);

// Parse + validate a scatter request at the boundary (schemas/fulltext_query.schema.json,
// mirrored embedded in distributed_fulltext.cpp). Non-throwing — nullopt with a
// path-qualified reason in `error` (when non-null) on any failure.
std::optional<FulltextQueryRequest> decode_fulltext_query(
    std::string_view payload, std::string* error = nullptr);

// The FulltextQueryRequest JSON Schema (draft-07) — the code-owned source of
// truth decode_fulltext_query enforces; schemas/fulltext_query.schema.json is
// generated from it and drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& fulltext_query_schema();

// The gather response: this node's ranked hits for the request (already merged
// across its own partitions, capped to top_k).
struct FulltextQueryResponse {
    std::string from;               // the responding node id
    std::vector<FulltextHit> hits;  // ranked, capped to the request's top_k
    std::size_t sub_indexes = 0;    // per-partition sub-indexes the node queried
                                    //   (provenance for #643's "sub-indexes queried")
};

// Serialize a gather response to its compact JSON form.
std::string encode_fulltext_hits(const FulltextQueryResponse& response);

// Parse + validate a gather response at the boundary (schemas/fulltext_hits.schema.json,
// mirrored embedded in distributed_fulltext.cpp). Non-throwing — nullopt with a
// path-qualified reason in `error` (when non-null) on any failure.
std::optional<FulltextQueryResponse> decode_fulltext_hits(
    std::string_view payload, std::string* error = nullptr);

// The FulltextQueryResponse JSON Schema (draft-07) — the code-owned source of
// truth decode_fulltext_hits enforces; schemas/fulltext_hits.schema.json is
// generated from it and drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& fulltext_hits_schema();

// The `fulltext.stats` response: this node's corpus statistics for the request's
// query, summed across its own sub-indexes (#851). The request is a
// FulltextQueryRequest (the same schema; top_k / return_fields are ignored).
struct FulltextStatsResponse {
    std::string from;               // the responding node id
    CorpusStatistics statistics;    // summed across this node's sub-indexes
    std::size_t sub_indexes = 0;    // per-partition sub-indexes the node consulted
};

// Serialize a statistics response to its compact JSON form.
std::string encode_fulltext_stats(const FulltextStatsResponse& response);

// Parse + validate a statistics response at the boundary
// (schemas/fulltext_stats.schema.json). Non-throwing — nullopt with a
// path-qualified reason in `error` (when non-null) on any failure.
std::optional<FulltextStatsResponse> decode_fulltext_stats(
    std::string_view payload, std::string* error = nullptr);

// The FulltextStatsResponse JSON Schema (draft-07) — the code-owned source of
// truth decode_fulltext_stats enforces; schemas/fulltext_stats.schema.json is
// generated from it and drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& fulltext_stats_schema();

// --- the per-node distributed full-text index --------------------------------

class DistributedFulltextIndex : public zetta::cluster::SignalHandler {
public:
    // The RPC method a coordinator scatters a query to peers with.
    static constexpr std::string_view kQueryMethod = "fulltext.query";
    // The RPC method a coordinator gathers every peer's corpus statistics with
    // before scattering the query (#851).
    static constexpr std::string_view kStatsMethod = "fulltext.stats";

    // A generous default for the scatter RPC: a query waits this long for a peer to
    // answer before failing the round (so a missing partition can't silently skew
    // the ranking).
    static constexpr std::chrono::milliseconds kDefaultTimeout{5000};

    // The outcome of building this node's sub-indexes from its owned partitions.
    struct BuildResult {
        IndexStatus status;
        std::size_t partitions_indexed = 0;   // owned partitions built into sub-indexes
        std::uint64_t rows_indexed = 0;        // rows indexed across them
        std::uint64_t local_bytes_read = 0;    // partition-body bytes read near data
                                               //   (NOT moved over the wire)
        bool ok() const noexcept { return status.ok(); }
    };

    // The outcome of a distributed query — the GLOBAL ranked list, identical on
    // every node, plus scatter-gather instrumentation.
    struct QueryResult {
        IndexStatus status;
        std::vector<std::string> row_ids;  // global ranking (score desc, id asc)
        std::vector<double> scores;        // aligned to row_ids
        // Aligned to row_ids: each hit's requested field values (#1243), or
        // null when no fields were requested.
        std::vector<nlohmann::json> fields;
        std::size_t nodes_answered = 0;    // self + peers that contributed
        std::uint64_t wire_bytes_moved = 0;  // scatter+gather payload bytes this query
                                             //   moved (the hits, not the rows)
        // Per-node provenance: the coordinator's own contribution first, then one
        // entry per peer in scatter order (#641). Empty on a failing status.
        std::vector<NodeContribution> contributions;
        bool ok() const noexcept { return status.ok(); }
    };

    // Construct over a started (or startable) Cluster and an owned IndexBackend
    // (default a fresh OramaCoreFulltextIndex — the BM25F full-text backend). The
    // backend holds the per-partition sub-indexes. Registers this as the cluster's
    // signal handler, chained over `delegate` (which receives every non-fulltext
    // signal); restores `delegate` on destruction. `cluster` must outlive this.
    explicit DistributedFulltextIndex(zetta::cluster::Cluster& cluster,
                                      std::shared_ptr<IndexBackend> backend = nullptr,
                                      SignalHandler* delegate = nullptr);
    ~DistributedFulltextIndex() override;

    DistributedFulltextIndex(const DistributedFulltextIndex&) = delete;
    DistributedFulltextIndex& operator=(const DistributedFulltextIndex&) = delete;

    // The owned backend (for tests / observability).
    IndexBackend& backend() const { return *backend_; }

    // Build per-partition sub-indexes over this node's OWNED partitions of the table
    // named in `spec` (compute-near-data): enumerate every DatasetPartition whose
    // table matches and whose owner_nodes contains this node, read each body from the
    // local large-object store, parse its JsonLines rows, and build ONE sub-index per
    // partition into the backend. Replaces any sub-indexes previously built for this
    // spec. A node owning no partitions of the table builds nothing (ok, 0
    // partitions). A backend build failure (e.g. the engine is not compiled in)
    // fails the whole build cleanly. Call from the application thread.
    BuildResult build_local(Node& node, const IndexSpec& spec);

    // Query the distributed index. When the backend scores with corpus statistics
    // (and `query.statistics` is not already set), first gather this node's and
    // every connected peer's statistics for the query (the fulltext.stats RPC) and
    // merge them (#851). Then query THIS node's local sub-indexes, scatter the
    // query with the merged statistics to every peer (the fulltext.query RPC),
    // gather their ranked hits, and merge everything into one global ranked list
    // capped to `query.top_k` — equal, membership, order and scores, to one
    // consolidated index over the whole table. Returns the same global ranking on
    // every node. An unreachable / failing peer fails the query (a missing
    // partition would skew the ranking). Both rounds go to the same peers. Call
    // from the application thread (never the network thread). A single-node
    // cluster returns its local ranking (its own sub-indexes still share one
    // merged set of statistics).
    //
    // When `recorder` AND `parent` are given, the scatter-gather is traced: one
    // child span per contribution ("search:local" for this node's own sub-indexes,
    // "search:peer" per scattered peer) under `parent`, carrying node id, hit count,
    // and wire bytes — so a surface can show where the query's time went (#641) —
    // plus one "search:statistics" span over the statistics round when it runs.
    // Both null (the default) = no spans, unchanged behavior.
    //
    // `return_fields`, when non-null, asks every node to attach those fields'
    // values to its own hits (#1243): the node that owns a row's partition reads
    // it from what it indexed, so rows never cross the wire twice. Each name
    // must be one of `spec.fields`.
    QueryResult query(const IndexSpec& spec, const IndexQuery& query,
                      std::chrono::milliseconds timeout = kDefaultTimeout,
                      zetta::observability::SpanRecorder* recorder = nullptr,
                      const zetta::observability::TraceContext* parent = nullptr,
                      const std::vector<std::string>* return_fields = nullptr,
                      const std::string* database = nullptr) const;

    // Query only this node's local sub-indexes (no scatter) → its merged, ranked
    // hits, capped to `query.top_k`. Used by query() for the local contribution and
    // by on_request to answer a peer. `status`, when non-null, receives the first
    // failing sub-query status (Ok when this node holds no partitions of the index —
    // it simply contributes nothing). `sub_indexes`, when non-null, receives how
    // many per-partition sub-indexes were consulted (the provenance surfaces show).
    std::vector<FulltextHit> query_local(const IndexSpec& spec,
                                         const IndexQuery& query,
                                         IndexStatus* status = nullptr,
                                         std::size_t* sub_indexes = nullptr,
                                         const std::vector<std::string>* return_fields = nullptr,
                                         const std::string* database = nullptr) const;

    // Sum this node's local sub-indexes' corpus statistics for `query` (no
    // scatter) — this node's part of the statistics round, used by query() and by
    // on_request to answer a peer's fulltext.stats. `status` / `sub_indexes` as
    // for query_local.
    CorpusStatistics collect_local_statistics(const IndexSpec& spec,
                                              const IndexQuery& query,
                                              IndexStatus* status = nullptr,
                                              std::size_t* sub_indexes = nullptr) const;

    // The participants of a scatter: this node + every connected peer, sorted by
    // node id (so the set is the same on every node).
    std::vector<std::string> participants() const;

    // --- SignalHandler (runs on the cluster network thread) ------------------
    zetta::cluster::SignalResponse on_request(const zetta::cluster::Signal& request) override;
    void on_event(const zetta::cluster::Signal& event) override;
    void on_task(const zetta::cluster::Signal& task) override;

private:
    zetta::cluster::Cluster& cluster_;
    std::shared_ptr<IndexBackend> backend_;
    SignalHandler* delegate_ = nullptr;

    // Guards sub_specs_ AND serializes backend_ access (the engine's in-memory index
    // is read by both the application thread (query) and the network thread
    // (on_request); serializing keeps it safe without assuming the engine is
    // re-entrant). Held only for the cheap in-memory work, never across an RPC.
    mutable std::mutex mutex_;
    // base IndexSpec::key() -> the per-partition sub-index specs built into backend_.
    std::map<std::string, std::vector<IndexSpec>> sub_specs_;
    std::map<std::string, std::string> source_databases_;
    // base IndexSpec::key() -> row id -> the row's values for the index's own
    // fields, kept from the build (#1243). A hit's fields come from here, on the
    // node that owns the row: the network thread answering a peer cannot read
    // the node's partition bodies, and the index already holds these values.
    std::map<std::string, std::map<std::string, nlohmann::json>> rows_;
};

}  // namespace zetta::index