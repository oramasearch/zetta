#pragma once

#include <vector>

#include "zetta/pipeline/executor.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_registry.hpp"

// The generic StepExecutor that wires an IR `index` step to a real IndexBackend
// (#528) — the run-path slice that drives a deployed `index` step through the
// resolved backend, "actually building the index over the table's partitions and
// serving queries from it" (the slice docs/indexing/index-backend.md named as later work).
//
// DefaultStepExecutor (executor.hpp) emits a *logical* `index:<id>` placeholder
// for an `index` step — "no real index is materialized in R1". This executor
// makes the index real, behind the same StepExecutor seam and WITHOUT touching
// the orchestrator (GraphExecutor's walk is unchanged), the way ChurnStepExecutor
// makes the churn steps real. For an `index` step it:
//
//   1. extracts the IndexSpec from the step's attributes (index_spec_from_node —
//      the validated `{ table, kind, backend, fields }` boundary);
//   2. resolves the backend from the node's IndexRegistry (name + kind must match);
//   3. reads the source table's rows into IndexPartitions (chunked like the
//      storage engine, so a build can fan across cores/nodes later);
//   4. builds the index over those partitions through the backend;
//   5. records the durable Index entity on the node (`index:<uuid>` + index.json);
//   6. emits an `index` Run output referencing the REAL Index id.
//
// The built index then lives in the node's registry backend, keyed by
// IndexSpec::key(), so it is **queryable for the node's lifetime**:
//
//   node.index_registry().resolve(index->spec())->query(spec, query);
//
// Every other step kind delegates to DefaultStepExecutor, so this is a drop-in
// that can run a whole graph (a `connect -> ... -> index` graph) while making
// only the `index` step real.
//
// **No new JSON format.** The index step's attributes ride the already-IR-validated
// graph JSON (read through index_spec_from_node), and the recorded Index entity is
// validated at its own JSON-Schema boundary (parse_index_value) — so malformed
// data still fails fast there. See docs/indexing/index-step-executor.md.
namespace zetta::storage { class StorageEngine; }
namespace zetta { class Node; }

namespace zetta::index {

// a build can fan across cores/nodes later). Shared by IndexStepExecutor and any
// other executor that builds an index over a table's rows. An empty store yields no
// partitions (a valid empty build).
std::vector<IndexPartition> read_index_partitions(const zetta::storage::StorageEngine& store);

// A step's source table is named in its IndexSpec (`spec.table`), and a table in
// zetta lives in a database. By default the source table is resolved by name
// across the node's databases (first match); an `index` step may carry an optional
// `database` attribute to qualify it (`<database>.<table>`). This is the one place
// the step's raw attributes are read beyond index_spec_from_node.
class IndexStepExecutor final : public zetta::pipeline::StepExecutor {
public:
    // Build over the node's own IndexRegistry (the usual case — the registry the
    // node ships the reference backends in, plus anything the host added).
    explicit IndexStepExecutor(Node& node);

    // Build over an explicit registry (for isolation in tests, or a host-curated
    // backend set). The registry must outlive the executor.
    IndexStepExecutor(Node& node, IndexRegistry& registry);

    // Execute one step. An `index` step builds + records a real index (see above);
    // every other kind delegates to DefaultStepExecutor. Returns ok=false with a
    // reason on a missing/invalid spec, an unresolved backend, a missing source
    // table, a build failure, or an entity-record failure — the orchestrator then
    // fail-fasts the run.
    zetta::pipeline::StepOutcome run_step(const ir::Node& step) const override;

private:
    zetta::pipeline::StepOutcome run_index(const ir::Node& step) const;

    Node& node_;
    IndexRegistry& registry_;
    zetta::pipeline::DefaultStepExecutor default_;  // delegate for the non-index step kinds
};

}  // namespace zetta::index