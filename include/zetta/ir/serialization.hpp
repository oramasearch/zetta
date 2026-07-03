#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"

// JSON (de)serialization for the ExecutionGraph IR (execution_graph.hpp).
//
// The canonical JSON form is the **tabular** half of the dual-form
// representation (ADR-10) — a graph as two arrays, nodes and edges:
//
//   {
//     "nodes": [ { "id": "…", "kind": "model", "name": "…",
//                  "attributes": { … } }, … ],
//     "edges": [ { "from": "…", "to": "…", "kind": "data_flow" }, … ]
//   }
//
// `kind` is the canonical lowercase name (to_string(StepKind) /
// to_string(EdgeKind)), never the enum's integer value, so the format is stable
// across enum reordering and is human-readable. `attributes` defaults to {} and
// an edge's `kind` defaults to "data_flow", matching the in-memory defaults.
//
// `to_json` then `from_json` round-trips a graph **unchanged** (same nodes,
// edges, attributes, and insertion order). Deserialization is the trust
// boundary: it validates the document against the schema above (see
// docs/pipeline/execution-graph.md) and the IR's own structural rules (unique node ids,
// no dangling or duplicate edges) and rejects anything that doesn't fit.
//
// `from_json` is **non-throwing** — it returns std::nullopt (with a
// human-readable reason in `error`) on malformed input, matching zetta's
// no-exceptions convention (`nlohmann::json::parse(..., allow_exceptions=false)`
// and skip-on-malformed everywhere else). A bad payload is rejected cleanly
// instead of corrupting state, and the validation lives in zetta-core where it
// can be reused by the persistence/replication paths without an exception ever
// crossing the library boundary.
namespace zetta::ir {

// Serialize a graph to its canonical JSON form. Never fails.
nlohmann::json to_json(const ExecutionGraph& graph);

// Parse a graph from its canonical JSON form. On success returns the graph; on
// malformed input returns std::nullopt and, when `error` is non-null, sets it to
// a human-readable reason (e.g. `nodes[2]: unknown step kind "frobnicate"`).
std::optional<ExecutionGraph> from_json(const nlohmann::json& document,
                                        std::string* error = nullptr);

}  // namespace zetta::ir
