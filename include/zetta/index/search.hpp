#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/index/distributed_fulltext.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/observability/trace.hpp"

// The canonical distributed search entry point (#641) — the ONE query path every
// surface (GUI, MCP/REST, tests) calls; there is no second path. `Node::search`
// delegates here. Given an index id or a table name it:
//
//   1. **resolves** the registered Index entity (by id, else the most recently
//      created Search-kind index over the named table);
//   2. **routes** the query: a table with placed partitions on a clustered node
//      goes through the per-partition DistributedFulltextIndex (scatter-gather +
//      merge/rank across the whole cluster — the distributed-first default); a
//      table with no placed partitions degrades to the consolidated backend the
//      IndexRegistry resolves (the index-step-executor build);
//   3. returns the ranked hits **with per-node provenance** (which node answered,
//      per-node hit counts / latency / wire bytes) and, when a SpanRecorder is
//      given, records the whole search as ONE trace: a root `search` span with
//      `search:local` / `search:peer` child spans (observability.md).
//
// Non-throwing (the zetta-core boundary convention): the result carries an
// IndexStatus with a path-qualified reason. Main-thread only (it reads the Node
// tree and may build local sub-indexes). See docs/indexing/node-search.md.
namespace zetta { class Node; }

namespace zetta::index {

class Index;

// What the caller asked for, verbatim — the search's *intent*.
struct SearchRequest {
    std::string target;   // an Index id, or a table name with a Search index over it
    std::string query;    // the free-text query
    std::string field;    // optional: restrict to one indexed field
    std::size_t top_k = 0;  // cap on the global ranking (0 = no cap)
    // The row fields each hit carries back (#1243). nullopt = the index's own
    // fields; an empty list = ids and scores only. Each name must be one of the
    // index's fields — the values come from what the index holds, read by the
    // node that owns the row.
    std::optional<std::vector<std::string>> fields;
    std::chrono::milliseconds timeout =
        DistributedFulltextIndex::kDefaultTimeout;  // per-peer scatter budget

    // Supplied by the authenticated surface, never from request JSON (#1403).
    // Empty is reserved for trusted in-process callers, as with SQL Executor.
    std::string acting_id;
    bool acting_is_user = false;
};

// One ranked hit of the global ranking (score descending, row id ascending on a
// tie — deterministic on every node).
struct SearchHit {
    std::string row_id;
    double score = 0.0;
    // The matched row's values for the requested fields (#1243); an empty
    // object when none were requested.
    nlohmann::json fields = nlohmann::json::object();

    bool operator==(const SearchHit&) const = default;
};

// Which path the search took — the engine's *plan*, surfaced so a caller can see
// whether the whole cluster answered or the degraded single-node index did.
enum class SearchPath {
    Distributed,   // per-partition scatter-gather across the cluster (the default)
    Consolidated,  // the registry backend's single consolidated index (degraded:
                   //   the table has no placed partitions, or no cluster is attached)
};

// The lowercase canonical name of a search path ("distributed" / "consolidated").
constexpr std::string_view to_string(SearchPath path) {
    switch (path) {
        case SearchPath::Distributed:  return "distributed";
        case SearchPath::Consolidated: return "consolidated";
    }
    return "unknown";
}

// The outcome: the global ranking plus the provenance + observability the
// foundations require. On a failing status only `status` (and `trace_id`, when
// tracing) is meaningful.
struct SearchResult {
    IndexStatus status;
    std::string index_id;              // the resolved Index entity's id
    IndexSpec spec;                    // what was searched (table/kind/backend/fields)
    SearchPath path = SearchPath::Consolidated;
    std::vector<SearchHit> hits;       // the global ranking, capped to top_k
    // Per-node provenance: who answered. The distributed path lists the
    // coordinator first then every scattered peer; the consolidated path lists
    // exactly this node.
    std::vector<NodeContribution> nodes;
    std::uint64_t wire_bytes_moved = 0;  // scatter+gather payload bytes (0 = local)
    std::string trace_id;              // the trace this search recorded into
    double duration_ms = 0.0;          // whole-search wall time

    bool ok() const noexcept { return status.ok(); }
};

// Run one search over `node` (see the file comment for resolution + routing).
// `recorder` + `parent` opt the search into tracing: a root `search` span is
// recorded under `parent` (or a fresh root trace when `parent` is null), with
// per-contribution child spans. Both default null = no spans, same result.
// Prefer calling this through `Node::search` — the documented entry point.
SearchResult search(Node& node, const SearchRequest& request,
                    zetta::observability::SpanRecorder* recorder = nullptr,
                    const zetta::observability::TraceContext* parent = nullptr);

// THE target-resolution rule search() applies, exposed for callers that must
// identify the Index entity a target names before querying it (the governed
// serving surface's exposure check, expose_index — #648): an exact index id
// wins; otherwise the most recently created Search-kind index over the table
// named `target`. Empty handle when nothing matches.
std::shared_ptr<Index> resolve_search_target(Node& node,
                                             const std::string& target);

}  // namespace zetta::index
