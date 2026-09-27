#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/run.hpp"

// The ExecutionGraph runner (an atomic slice of FEAT-5). Orchestrate execution of
// an ExecutionGraph step-by-step on a node, producing a Run with status, logs,
// outputs, and lineage. R1 runs the churn pipeline step-kinds locally.
//
// This is the piece that joins the two keystones it sits on: the ExecutionGraph
// IR (ir/execution_graph.hpp — what to run) and the Run record (run.hpp — the
// observable result). GraphExecutor walks a graph in dependency order and drives
// a Run created on the node through Pending -> Running -> Succeeded | Failed,
// accumulating a log line per step, the typed artifacts each step produces, and
// the lineage (sources / steps / freshness / confidence).
//
// Altitude: this slice is **orchestration**, not heavy compute. A step does not
// (yet) train a real model or build a real index; the DefaultStepExecutor derives
// a faithful, schema-valid Run from the graph's structure. Real per-step compute
// plugs in behind the StepExecutor seam without touching the orchestrator — the
// same shape as the gate's Check seam and the storage engine's StorageEngine.
namespace zetta { class Node; }

namespace zetta::pipeline {

// (datasets / models / indexes) and, for a source step, zero or more lineage
// sources it pulled from. A step fails by returning ok=false with a reason; the
// orchestrator then stops (fail-fast) and marks the run Failed.
struct StepOutcome {
    bool ok = true;
    std::string error;                  // why it failed (when !ok)
    std::vector<RunOutput> outputs;     // artifacts this step materialized
    std::vector<std::string> sources;   // source refs this step pulled from
    // What a person should know about how this step ran (logged as info on the run):
    // e.g. that a connect step was not read because the run trained on a named dataset.
    std::vector<std::string> notes;
};

// The pluggable unit that executes one graph step (mirrors the gate's Check
// seam). A StepExecutor is a pure function of the step it is handed; the default
// (DefaultStepExecutor) interprets the connect->clean->model->index->serve->act
// taxonomy. Concrete compute (real training, real indexing) implements this
// interface and is injected into a GraphExecutor without changing the walk.
class StepExecutor {
public:
    virtual ~StepExecutor() = default;

    // Execute `step` and report what it produced (or why it failed).
    virtual StepOutcome run_step(const ir::Node& step) const = 0;
};

// The default step executor for R1: interprets a step's StepKind to derive the
// artifact it materializes, with no real compute. The mapping (also in
// docs/pipeline/execution.md):
//
//   connect -> a `dataset` output + a lineage source (from the step's
//              `attributes`: "source"+"table", else "source"/"uri"/"path",
//              else the step name)
//   clean   -> a `dataset` output
//   model   -> a `model` output
//   index   -> an `index` output
//   serve   -> no artifact (it exposes existing ones)
//   act     -> no artifact (it acts on the world)
//
// An output's `kind` is always one of the Run schema's closed set, so a produced
// Run is schema-valid by construction. A step fails deterministically when its
// `attributes` carry a truthy `"fail"` (`true`, or a non-empty string used as the
// reason) — this drives the Failed path in tests and lets an author mark a step
// as known-broken without a custom StepExecutor.
class DefaultStepExecutor final : public StepExecutor {
public:
    StepOutcome run_step(const ir::Node& step) const override;
};

// The result of executing a whole graph: the Run it produced (always created, so
// the attempt is recorded even on failure), whether every step succeeded, and —
// on failure — the id of the step that failed and the reason. `run` is null only
// if the Run could not be created on the node.
struct ExecutionResult {
    std::shared_ptr<Run> run;
    bool ok = false;
    std::string failed_step;  // graph node id of the failing step (when !ok)
    std::string error;        // failure reason (when !ok)
};

// Orchestrate execution of an ExecutionGraph on a node (mirrors CorrectnessGate:
// a small runner holding the pluggable unit it runs). Holds a StepExecutor; the
// default is DefaultStepExecutor.
class GraphExecutor {
public:
    // Build an executor. A null step_executor falls back to DefaultStepExecutor.
    explicit GraphExecutor(
        std::shared_ptr<StepExecutor> step_executor = nullptr);

    // Execute `graph` on `node`, creating and driving a Run. The Run is labelled
    // with `project` / `graph_id` (both optional, free-string references, fixed at
    // creation). Steps run in dependency (topological) order; each emits a log
    // line; outputs + lineage aggregate as they go. The run ends:
    //   - Failed   if the graph is empty, has a cycle, a step failed
    //              (fail-fast: later steps do not run), or `cancel` was set;
    //   - Succeeded otherwise.
    // Both standalone and replicated nodes are supported — the Run's own mutators
    // capture into the CRDT (when replicated) and persist to disk either way.
    //
    // Threading (#624): every node-tree write (the Run creation + the Run's
    // mutators) is gated through Node::run_on_owner, so execute() may be driven
    // from a background-action worker while the owner loop keeps rendering /
    // ticking. Synchronous owner-thread callers are unchanged (the gate runs
    // inline).
    //
    // Cancellation (#624): `cancel` is an optional non-owning cooperative flag
    // (must outlive the call). It is checked BETWEEN steps — the in-flight step
    // completes — and a cancelled run ends Failed with a "run cancelled" log
    // line and `error` = "run cancelled by user".
    ExecutionResult execute(Node& node, const ir::ExecutionGraph& graph,
                            std::string project = {},
                            std::string graph_id = {},
                            const std::atomic<bool>* cancel = nullptr) const;

    // The step executor this orchestrator runs each step with.
    const StepExecutor& step_executor() const { return *step_executor_; }

private:
    std::shared_ptr<StepExecutor> step_executor_;
};

// Compute a dependency (topological) order over a graph's steps: every step
// appears after all steps with an edge into it (both DataFlow and Control edges
// constrain order). Ready steps are emitted in graph insertion order, so the
// result is deterministic. Returns std::nullopt when the graph cannot be ordered
// — i.e. it contains a cycle (a self-edge counts) — which the executor surfaces
// as a Failed run. Exposed for direct testing.
std::optional<std::vector<std::shared_ptr<ir::Node>>> topological_order(
    const ir::ExecutionGraph& graph);

}  // namespace zetta::pipeline