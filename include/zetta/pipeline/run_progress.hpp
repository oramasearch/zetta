// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/run.hpp"

// Per-step run progress, projected from a Run record onto an ExecutionGraph
// (#626 — the Graph Workspace live-run overlay's engine half).
//
// A Run makes itself observable through its logs: the GraphExecutor writes one
// line as each step starts ("<kind>: <label>") and one when a step fails
// ("<kind> step '<label>' failed: <reason>"), and its outputs carry the
// artifacts each step materialized (named by the step's label; the churn
// engine's model/index refs are real, resolvable entity ids). This header
// derives, from exactly that already-validated record, where a run currently
// is inside its graph — which steps have run, which is running now, which
// failed and why, and what each produced — so a renderer (the headed Graph
// Workspace, a CLI, an MCP read) can show the run moving through the graph
// without inventing any state of its own.
//
// The projection is a pure function of (graph, run value): no I/O, no caching,
// O(logs + steps + outputs) — cheap enough to recompute every frame (the #590
// bounded-work-per-frame constraint). It lives beside the executor whose log
// grammar it reads, so the two are maintained (and tested) together. It parses
// no JSON: the RunValue it consumes was schema-validated at its own boundary
// (parse_run_value), so this introduces no new format to validate.
namespace zetta::pipeline {

// Where one graph step is inside a run's lifecycle. The same shape as
// RunStatus, per step: Pending (not reached), Running (started, run still in
// flight, nothing after it has started), Succeeded, Failed.
enum class StepRunStatus {
    Pending,
    Running,
    Succeeded,
    Failed,
};

// The canonical lowercase name of a step-run status ("pending" / "running" /
// "succeeded" / "failed") — for labels, logs, and tests.
std::string_view to_string(StepRunStatus status);

// One graph step's progress inside a run.
struct StepRunProgress {
    std::string step_id;                // the graph node id this row describes
    StepRunStatus status = StepRunStatus::Pending;
    std::string reason;                 // why it failed (Failed only) — the
                                        // reason from the run log, verbatim
    std::vector<RunOutput> outputs;     // outputs attributed to this step
};

// A run projected onto a graph: one row per graph step (in graph order), plus
// the run-level status and — when the run failed — the failing reason (the
// failed step's reason, else the run's last error log line) so a renderer can
// pin it without re-deriving it.
struct RunProgress {
    RunStatus run_status = RunStatus::Pending;
    std::vector<StepRunProgress> steps;
    std::string failure_reason;         // "" unless the run failed
};

// Project `run` onto `graph`: derive each step's status from the run's log
// (the GraphExecutor grammar), attribute the run's outputs to the steps that
// produced them (by the step label the executors stamp into RunOutput::name,
// falling back to the "<kind>:<step id>" logical ref), and surface the failing
// reason. Steps the run's log never mentions stay Pending — including every
// step when `run` executed a different graph, so projecting an unrelated run
// is safe (it just shows nothing).
RunProgress project_run_progress(const ir::ExecutionGraph& graph,
                                 const RunValue& run);

}  // namespace zetta::pipeline
