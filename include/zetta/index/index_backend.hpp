// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/index/index_kind.hpp"

// The IndexBackend seam (ENT-39 / ENT-40, ADR-34) — the single interface every
// concrete index implementation targets. It is the same shape as zetta's other
// seams (StorageEngine, ComputeDevice, StepExecutor, Check): a pure-virtual base
// class whose concrete subclasses are the *backends*, so a consumer calls the
// interface and stays unchanged when a new backend appears. **New index type =
// a new backend, nothing else** — a vector / semantic backend drops in additively
// behind this interface without touching the registry or the IR.
//
// A backend does three things, the issue's exact verbs:
//   - **build** an index over a table's *partitions* (the chunked rows the storage
//     engine / DDF hands out, so a build can fan across the cluster later),
//   - **query** a built index, and
//   - **drop** it.
//
// Like ComputeDevice, every call returns a status rather than throwing —
// zetta-core compiles with -fexperimental-library and must never unwind across a
// seam. No SDL/CUDA/heavy type appears here; it is pure zetta-core.
namespace zetta::index {

// What an index is built over, independent of which backend builds it: the source
// `table`, the index `kind` (search / vector / feature), the `backend` name that
// should build it (matches IndexBackend::name()), and the `fields` it indexes.
// This is the seam's view of an index — the durable Index entity (index/index.hpp)
// carries the same four fields plus identity, and hands a backend its `spec()`.
struct IndexSpec {
    std::string table;                 // the source table the index is over
    IndexKind kind = IndexKind::Search;  // what the index accelerates
    std::string backend;               // the backend that should build it
    std::vector<std::string> fields;   // the fields it indexes (>= 1)

    // A stable key identifying *this* index within a backend that may hold many
    // (table + kind + fields, in field order). build()/query()/drop() use it so a
    // backend can store/look up the right index. Two specs differing only in
    // `backend` share a key — `backend` selects the implementation, not the index.
    std::string key() const;

    bool operator==(const IndexSpec&) const = default;
};

// Why a backend call succeeded or failed (mirrors compute::ComputeStatusCode, the
// established seam-result idiom). `Unsupported` is first-class: a backend that does
// not implement a requested kind/op returns it so a caller can route elsewhere.
enum class IndexStatusCode {
    Ok,               // the call completed
    Unsupported,      // this backend does not implement this kind / operation
    InvalidArgument,  // a spec/query/partition precondition was violated
    NotFound,         // the index has not been built on this backend
    BackendError,     // the backend reported a failure
};

// The lowercase canonical name of a status code ("ok", "unsupported", …).
constexpr std::string_view to_string(IndexStatusCode code) {
    switch (code) {
        case IndexStatusCode::Ok:              return "ok";
        case IndexStatusCode::Unsupported:     return "unsupported";
        case IndexStatusCode::InvalidArgument: return "invalid_argument";
        case IndexStatusCode::NotFound:        return "not_found";
        case IndexStatusCode::BackendError:    return "backend_error";
    }
    return "unknown";
}

// The outcome of a backend call: a code plus, on failure, a human-readable reason.
// A default-constructed status is Ok. Named constructors read clearer at call
// sites than aggregate init (the ComputeStatus pattern).
struct IndexStatus {
    IndexStatusCode code = IndexStatusCode::Ok;
    std::string message;

    bool ok() const noexcept { return code == IndexStatusCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    bool operator==(const IndexStatus&) const = default;

    static IndexStatus success() { return {}; }
    static IndexStatus unsupported(std::string reason) {
        return {IndexStatusCode::Unsupported, std::move(reason)};
    }
    static IndexStatus invalid_argument(std::string reason) {
        return {IndexStatusCode::InvalidArgument, std::move(reason)};
    }
    static IndexStatus not_found(std::string reason) {
        return {IndexStatusCode::NotFound, std::move(reason)};
    }
    static IndexStatus backend_error(std::string reason) {
        return {IndexStatusCode::BackendError, std::move(reason)};
    }
};

// One row to index: its stable id (the row's `_zid` — the same id on every node)
// and its document (the field values the backend reads). The doc is the row's
// data, exactly what a StorageEngine `at(i)` returns.
struct IndexDocument {
    std::string id;        // the row's stable id (its CRDT key)
    nlohmann::json doc;    // the row's field values
};

// A partition of a table's rows — the unit build() ingests. A table is stored in
// chunks (JsonlStore, 10k/chunk) and read into DDF chunks; an index builds
// per-partition so the work can fan across cores/nodes (a later slice). A
// partition carries an id (the chunk / shard id, for logging + idempotency) and
// its documents.
struct IndexPartition {
    std::string id;                        // the chunk / shard id
    std::vector<IndexDocument> documents;  // the rows in this partition
};

// One field's length totals across a corpus: the sum of the field's token counts
// over the documents that carry it, and how many documents carry it (the average
// field length BM25F normalizes by is total_len / doc_count).
struct FieldLengthStatistics {
    std::uint64_t total_len = 0;
    std::uint64_t doc_count = 0;

    bool operator==(const FieldLengthStatistics&) const = default;
};

// The corpus statistics a ranked backend scores with (#851): the document count,
// the document frequency of each query term (keyed by the ANALYZED term the
// backend matches on, so a backend's own tokenizer decides the keys), and each
// field's length totals (keyed by field name). A sharded query collects one per
// shard (IndexBackend::collect_statistics), merges them, and queries every shard
// with the merge (IndexQuery::statistics), so each shard ranks exactly as one
// index over the whole corpus would and a per-shard top_k cutoff drops nothing
// that belongs in the global top k.
struct CorpusStatistics {
    std::uint64_t doc_count = 0;
    std::map<std::string, std::uint64_t> term_doc_freq;
    std::map<std::string, FieldLengthStatistics> fields;

    // Add another shard's statistics into this one. Every count sums: shards hold
    // disjoint rows.
    void merge(const CorpusStatistics& other) {
        doc_count += other.doc_count;
        for (const auto& [term, frequency] : other.term_doc_freq) {
            term_doc_freq[term] += frequency;
        }
        for (const auto& [field, lengths] : other.fields) {
            FieldLengthStatistics& mine = fields[field];
            mine.total_len += lengths.total_len;
            mine.doc_count += lengths.doc_count;
        }
    }

    bool operator==(const CorpusStatistics&) const = default;
};

// A query against a built index. A Search backend reads `field` + `term` (exact
// value match over an indexed field). A Vector / semantic backend reads `vector` +
// `top_k` behind the *same* seam (additive — a field a backend doesn't use is
// simply ignored); declaring them here keeps the seam stable as those backends
// land. `top_k` caps the result count (0 = no cap).
struct IndexQuery {
    std::string field;             // the indexed field to match (Search)
    nlohmann::json term;           // the value to match (Search)
    std::vector<float> vector;     // the query embedding (Vector / semantic)
    std::size_t top_k = 0;         // cap on results (0 = no cap)
    // The corpus statistics to score with instead of the index's own (#851): a
    // sharded query's merged view of every shard. nullopt = the index's own
    // statistics. A backend that does not use corpus statistics ignores it.
    std::optional<CorpusStatistics> statistics;
};

// What a query returned: a status plus the matching row ids in a deterministic
// order (so a query is reproducible). On a failing status `row_ids` is empty.
//
// `scores` is the per-hit relevance score, **higher = better**, aligned to
// `row_ids` (scores[i] is the score of row_ids[i]). A *ranked* backend
// (OramaCoreFulltextIndex's BM25F) fills it so a distributed scatter-gather can
// merge per-partition hits into one global ranking by score; an *exact-match*
// backend (InMemoryIndexBackend) has no ranking signal and leaves it **empty**.
// The invariant: `scores` is either empty (unranked) or exactly `row_ids.size()`
// long. A consumer that needs scores treats an empty vector as "no ranking"
// (every hit equally relevant).
struct IndexQueryResult {
    IndexStatus status;
    std::vector<std::string> row_ids;
    std::vector<double> scores;  // empty (unranked) or aligned to row_ids
};

// The backend seam: build an index over partitions, query it, drop it. Concrete
// backends (the in-memory reference Search backend today; vector / semantic
// backends later) subclass this. A backend may hold many built indexes, keyed by
// IndexSpec::key().
class IndexBackend {
public:
    virtual ~IndexBackend() = default;

    // This backend's stable name — what an IndexSpec::backend / Index::backend
    // names to select it in the registry (e.g. "memory_search").
    virtual std::string name() const = 0;

    // The index kind this backend implements. The registry resolves a spec to a
    // backend whose name matches *and* whose kind matches the spec's kind.
    virtual IndexKind kind() const = 0;

    // Build (or rebuild) the index described by `spec` over `partitions`. Replaces
    // any index previously built under spec.key(). Returns InvalidArgument on a
    // malformed spec (e.g. no fields), Unsupported if the spec's kind is not this
    // backend's kind.
    virtual IndexStatus build(const IndexSpec& spec,
                              std::span<const IndexPartition> partitions) = 0;

    // Query a built index. Returns NotFound (in the result's status) when no index
    // was built under spec.key(), InvalidArgument on a malformed query.
    virtual IndexQueryResult query(const IndexSpec& spec,
                                   const IndexQuery& query) const = 0;

    // Drop a built index. Idempotent: dropping an index that was never built (or
    // was already dropped) succeeds (Ok) — the post-condition is "not built here".
    virtual IndexStatus drop(const IndexSpec& spec) = 0;

    // Does this backend score with corpus statistics (document frequencies,
    // average field lengths)? If so, a sharded query must gather every shard's
    // statistics first and score each shard with the merge (#851), or a per-shard
    // top_k cutoff can drop a row that belongs in the global top k. False by
    // default: an exact-match or partition-independent scorer needs no such pass.
    virtual bool uses_corpus_statistics() const { return false; }

    // Add the index built under spec.key()'s corpus statistics for `query` into
    // `out` (so a caller can accumulate several sub-indexes). Only meaningful when
    // uses_corpus_statistics(); the default adds nothing and succeeds. Same
    // statuses as query() (NotFound, InvalidArgument).
    virtual IndexStatus collect_statistics(const IndexSpec& spec,
                                           const IndexQuery& query,
                                           CorpusStatistics& out) const {
        (void)spec;
        (void)query;
        (void)out;
        return IndexStatus::success();
    }
};

}  // namespace zetta::index