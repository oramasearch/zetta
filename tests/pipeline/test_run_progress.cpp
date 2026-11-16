// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/run_progress.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::pipeline::ExecutionResult;
using zetta::pipeline::project_run_progress;
using zetta::pipeline::RunLogEntry;
using zetta::pipeline::RunOutput;
using zetta::pipeline::RunProgress;
using zetta::pipeline::RunStatus;
using zetta::pipeline::RunValue;
using zetta::pipeline::StepRunProgress;
using zetta::pipeline::StepRunStatus;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// The full pipeline, one step per kind, chained connect -> ... -> act. Fixed
// ids so the assertions below can address steps directly.
ExecutionGraph full_pipeline() {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "ingest_events",
                   {{"source", "postgres"}, {"table", "events"}}, "s-connect");
    graph.add_node(StepKind::Clean, "dedupe", {}, "s-clean");
    graph.add_node(StepKind::Model, "churn_model", {}, "s-model");
    graph.add_node(StepKind::Index, "churn_index", {}, "s-index");
    graph.add_node(StepKind::Serve, "churn_api", {}, "s-serve");
    graph.add_node(StepKind::Act, "notify_at_risk", {}, "s-act");
    graph.add_edge("s-connect", "s-clean");
    graph.add_edge("s-clean", "s-model");
    graph.add_edge("s-model", "s-index");
    graph.add_edge("s-index", "s-serve");
    graph.add_edge("s-serve", "s-act");
    return graph;
}

const StepRunProgress& step_of(const RunProgress& progress,
                               const std::string& step_id) {
    for (const StepRunProgress& step : progress.steps) {
        if (step.step_id == step_id) return step;
    }
    ADD_FAILURE() << "no progress row for step " << step_id;
    static const StepRunProgress missing{};
    return missing;
}

}  // namespace

// A run the real executor drove to success projects every step Succeeded, with
// the materializing steps' outputs attributed to them. Running the projection
// against the executor (not hand-built logs) pins the two to the same grammar.
TEST(RunProgress, SucceededRunMarksEveryStepSucceeded) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    const ExecutionGraph graph = full_pipeline();
    const ExecutionResult result = node.run_graph(graph, "proj", "graph-1");
    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.run);

    const RunProgress progress = project_run_progress(graph, result.run->value());
    EXPECT_EQ(progress.run_status, RunStatus::Succeeded);
    EXPECT_TRUE(progress.failure_reason.empty());
    ASSERT_EQ(progress.steps.size(), 6u);
    for (const StepRunProgress& step : progress.steps) {
        EXPECT_EQ(step.status, StepRunStatus::Succeeded) << step.step_id;
    }

    // The default executor materializes dataset/dataset/model/index for the
    // first four kinds (serve/act produce nothing) — each lands on its step.
    EXPECT_EQ(step_of(progress, "s-connect").outputs.size(), 1u);
    EXPECT_EQ(step_of(progress, "s-connect").outputs[0].kind, "dataset");
    EXPECT_EQ(step_of(progress, "s-model").outputs.size(), 1u);
    EXPECT_EQ(step_of(progress, "s-model").outputs[0].kind, "model");
    EXPECT_EQ(step_of(progress, "s-index").outputs.size(), 1u);
    EXPECT_EQ(step_of(progress, "s-index").outputs[0].kind, "index");
    EXPECT_TRUE(step_of(progress, "s-serve").outputs.empty());
    EXPECT_TRUE(step_of(progress, "s-act").outputs.empty());
}

// A failing step (the executor's "fail" attribute) pins Failed + the reason on
// that step; earlier steps read Succeeded and downstream steps stay Pending
// (fail-fast: they never ran).
TEST(RunProgress, FailedStepIsPinnedWithItsReason) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    ExecutionGraph graph = full_pipeline();
    graph.find_node("s-model")->attributes["fail"] = "training data was empty";

    const ExecutionResult result = node.run_graph(graph, "proj", "graph-1");
    ASSERT_FALSE(result.ok);
    ASSERT_TRUE(result.run);
    EXPECT_EQ(result.failed_step, "s-model");

    const RunProgress progress = project_run_progress(graph, result.run->value());
    EXPECT_EQ(progress.run_status, RunStatus::Failed);
    EXPECT_EQ(step_of(progress, "s-connect").status, StepRunStatus::Succeeded);
    EXPECT_EQ(step_of(progress, "s-clean").status, StepRunStatus::Succeeded);
    const StepRunProgress& failed = step_of(progress, "s-model");
    EXPECT_EQ(failed.status, StepRunStatus::Failed);
    EXPECT_EQ(failed.reason, "training data was empty");
    EXPECT_EQ(progress.failure_reason, "training data was empty");
    EXPECT_EQ(step_of(progress, "s-index").status, StepRunStatus::Pending);
    EXPECT_EQ(step_of(progress, "s-serve").status, StepRunStatus::Pending);
    EXPECT_EQ(step_of(progress, "s-act").status, StepRunStatus::Pending);
}

// A run still in flight (hand-built mid-run log, the shape a replicating peer
// sees arrive): the last step to start reads Running, earlier starts read
// Succeeded, unreached steps stay Pending.
TEST(RunProgress, InFlightRunMarksTheLastStartedStepRunning) {
    const ExecutionGraph graph = full_pipeline();

    RunValue run;
    run.status = RunStatus::Running;
    run.logs.push_back({"2026-07-02T00:00:00Z", "info", "run created: 6 step(s)"});
    run.logs.push_back({"2026-07-02T00:00:01Z", "info", "run started"});
    run.logs.push_back({"2026-07-02T00:00:01Z", "info", "connect: ingest_events"});
    run.logs.push_back({"2026-07-02T00:00:02Z", "info", "clean: dedupe"});

    const RunProgress progress = project_run_progress(graph, run);
    EXPECT_EQ(step_of(progress, "s-connect").status, StepRunStatus::Succeeded);
    EXPECT_EQ(step_of(progress, "s-clean").status, StepRunStatus::Running);
    EXPECT_EQ(step_of(progress, "s-model").status, StepRunStatus::Pending);
    EXPECT_EQ(step_of(progress, "s-act").status, StepRunStatus::Pending);
}

// A run with no step markers at all — pending, or a deployment-controller run
// that only logged its reconcile note — projects every step Pending. Same for
// a run of some other graph: nothing matches, nothing is invented.
TEST(RunProgress, UnrelatedOrPendingRunLeavesEveryStepPending) {
    const ExecutionGraph graph = full_pipeline();

    RunValue pending;
    EXPECT_EQ(project_run_progress(graph, pending).steps.size(), 6u);
    for (const StepRunProgress& step : project_run_progress(graph, pending).steps) {
        EXPECT_EQ(step.status, StepRunStatus::Pending);
    }

    RunValue other;
    other.status = RunStatus::Running;
    other.logs.push_back({"2026-07-02T00:00:00Z", "info", "connect: some_other_step"});
    for (const StepRunProgress& step : project_run_progress(graph, other).steps) {
        EXPECT_EQ(step.status, StepRunStatus::Pending);
    }
}

// A run that failed before any step could run (empty graph / cycle) still
// surfaces a pinned failure reason — the run's last error line.
TEST(RunProgress, RunFailedBeforeAnyStepFallsBackToTheLastErrorLine) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    const ExecutionGraph empty;
    const ExecutionResult result = node.run_graph(empty, "proj", "graph-1");
    ASSERT_TRUE(result.run);

    const RunProgress progress =
        project_run_progress(full_pipeline(), result.run->value());
    EXPECT_EQ(progress.run_status, RunStatus::Failed);
    EXPECT_EQ(progress.failure_reason,
              "execution graph has no steps; nothing to run");
}

// Output attribution prefers the real per-step ref ("<kind>:<step id>") and
// falls back to the step label the executors stamp into RunOutput::name — the
// shape the churn engine produces, where `ref` is a real registered Model id.
TEST(RunProgress, OutputsWithRealRefsAttributeByStepLabel) {
    const ExecutionGraph graph = full_pipeline();

    RunValue run;
    run.status = RunStatus::Succeeded;
    run.logs.push_back({"2026-07-02T00:00:00Z", "info", "model: churn_model"});
    run.outputs.push_back(RunOutput{"model", "churn_model",
                                    "mdl-1d9e75f2-real-model-id"});

    const RunProgress progress = project_run_progress(graph, run);
    const StepRunProgress& model = step_of(progress, "s-model");
    ASSERT_EQ(model.outputs.size(), 1u);
    EXPECT_EQ(model.outputs[0].ref, "mdl-1d9e75f2-real-model-id");
    EXPECT_EQ(model.status, StepRunStatus::Succeeded);
}
