#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

using zetta::pipeline::DefaultStepExecutor;
using zetta::pipeline::DeploymentTarget;
using zetta::pipeline::ExecutionResult;
using zetta::pipeline::GraphExecutor;
using zetta::Node;
using zetta::NodeConfig;
using zetta::pipeline::RunOutput;
using zetta::pipeline::RunStatus;
using zetta::pipeline::StepExecutor;
using zetta::pipeline::StepOutcome;
using zetta::pipeline::topological_order;
using zetta::ir::EdgeKind;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// The full churn pipeline as an execution graph: one step of each kind, chained
// connect -> clean -> model -> index -> serve -> act. The connect step carries a
// source in its attribute bag so the run's lineage records where data came from.
ExecutionGraph churn_pipeline() {
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "ingest_events",
                                  {{"source", "postgres"}, {"table", "events"}});
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model");
    auto index = graph.add_node(StepKind::Index, "churn_index");
    auto serve = graph.add_node(StepKind::Serve, "churn_api");
    auto act = graph.add_node(StepKind::Act, "notify_at_risk");
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    graph.add_edge(model->id, index->id);
    graph.add_edge(index->id, serve->id);
    graph.add_edge(serve->id, act->id);
    return graph;
}

std::size_t count_outputs(const std::vector<RunOutput>& outputs,
                          const std::string& kind) {
    return static_cast<std::size_t>(std::count_if(
        outputs.begin(), outputs.end(),
        [&](const RunOutput& output) { return output.kind == kind; }));
}

bool has_log_containing(const std::shared_ptr<zetta::pipeline::Run>& run,
                        const std::string& needle) {
    for (const auto& entry : run->logs()) {
        if (entry.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// ---- topological ordering --------------------------------------------------

TEST(Executor, TopologicalOrderRespectsEdges) {
    const ExecutionGraph graph = churn_pipeline();
    const auto order = topological_order(graph);
    ASSERT_TRUE(order);
    ASSERT_EQ(order->size(), 6u);
    // The chain forces a unique order: connect first, act last.
    EXPECT_EQ((*order)[0]->kind, StepKind::Connect);
    EXPECT_EQ((*order)[1]->kind, StepKind::Clean);
    EXPECT_EQ((*order)[2]->kind, StepKind::Model);
    EXPECT_EQ((*order)[5]->kind, StepKind::Act);
}

TEST(Executor, TopologicalOrderIsDeterministicForUnconstrainedNodes) {
    // No edges: ready steps come out in graph insertion order.
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "a", {}, "a");
    graph.add_node(StepKind::Clean, "b", {}, "b");
    graph.add_node(StepKind::Model, "c", {}, "c");
    const auto order = topological_order(graph);
    ASSERT_TRUE(order);
    ASSERT_EQ(order->size(), 3u);
    EXPECT_EQ((*order)[0]->id, "a");
    EXPECT_EQ((*order)[1]->id, "b");
    EXPECT_EQ((*order)[2]->id, "c");
}

TEST(Executor, TopologicalOrderRejectsCycles) {
    ExecutionGraph two_cycle;
    two_cycle.add_node(StepKind::Connect, "a", {}, "a");
    two_cycle.add_node(StepKind::Clean, "b", {}, "b");
    two_cycle.add_edge("a", "b");
    two_cycle.add_edge("b", "a");
    EXPECT_FALSE(topological_order(two_cycle).has_value());

    // A self-edge is a cycle too.
    ExecutionGraph self_loop;
    self_loop.add_node(StepKind::Model, "m", {}, "m");
    self_loop.add_edge("m", "m");
    EXPECT_FALSE(topological_order(self_loop).has_value());
}

// ---- end-to-end execution (the acceptance criterion) -----------------------

TEST(Executor, DeployedGraphExecutesEndToEndAndProducesARun) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    const ExecutionGraph graph = churn_pipeline();

    // Deploy the graph, then execute it on the node (the issue's flow).
    auto deployment =
        node.deploy_graph(graph, DeploymentTarget::local(), "churn_pipeline");
    ASSERT_TRUE(deployment);

    const ExecutionResult result =
        node.run_graph(graph, "churn-project", "churn-graph-1");
    ASSERT_TRUE(result.run);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.failed_step.empty());

    const auto run = result.run;
    EXPECT_EQ(run->status(), RunStatus::Succeeded);
    EXPECT_EQ(run->project(), "churn-project");
    EXPECT_EQ(run->graph(), "churn-graph-1");

    // Outputs: a dataset per connect+clean, a model, an index (serve/act make
    // no artifact).
    EXPECT_EQ(run->outputs().size(), 4u);
    EXPECT_EQ(count_outputs(run->outputs(), "dataset"), 2u);
    EXPECT_EQ(count_outputs(run->outputs(), "model"), 1u);
    EXPECT_EQ(count_outputs(run->outputs(), "index"), 1u);

    // Lineage: every step ran, the source was recorded, freshness + full
    // confidence are stamped.
    EXPECT_EQ(run->lineage().steps.size(), 6u);
    ASSERT_EQ(run->lineage().sources.size(), 1u);
    EXPECT_EQ(run->lineage().sources[0], "postgres:events");
    EXPECT_FALSE(run->lineage().freshness.empty());
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 1.0);

    // Logs trace the run: creation, start, each step, success.
    EXPECT_TRUE(has_log_containing(run, "run started"));
    EXPECT_TRUE(has_log_containing(run, "model: churn_model"));
    EXPECT_TRUE(has_log_containing(run, "run succeeded"));

    // The run is owned + queryable on the node.
    EXPECT_EQ(node.run_count(), 1u);
    EXPECT_EQ(node.find_run(run->id()), run);

    node.stop();
}

// ---- the Failed path -------------------------------------------------------

TEST(Executor, FailingStepStopsTheRunAndIsDiagnosable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // Mark the model step as failing via its attribute bag; downstream steps
    // (index/serve/act) must not run.
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "ingest");
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model",
                                {{"fail", "model did not converge"}});
    auto index = graph.add_node(StepKind::Index, "churn_index");
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    graph.add_edge(model->id, index->id);

    const ExecutionResult result = node.run_graph(graph);
    ASSERT_TRUE(result.run);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failed_step, model->id);
    EXPECT_EQ(result.error, "model did not converge");

    const auto run = result.run;
    EXPECT_EQ(run->status(), RunStatus::Failed);
    // Only connect, clean, model ran (model failed); index was skipped.
    EXPECT_EQ(run->lineage().steps.size(), 3u);
    // Partial outputs: the two datasets produced before the failure.
    EXPECT_EQ(run->outputs().size(), 2u);
    EXPECT_DOUBLE_EQ(run->lineage().confidence, 0.0);
    EXPECT_TRUE(has_log_containing(run, "model did not converge"));

    node.stop();
}

// ---- guards: empty + cyclic graphs fail cleanly ----------------------------

TEST(Executor, EmptyGraphFailsWithoutRunning) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    const ExecutionResult result = node.run_graph(ExecutionGraph{});
    ASSERT_TRUE(result.run);  // the attempt is still recorded
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.run->status(), RunStatus::Failed);
    EXPECT_TRUE(result.run->lineage().steps.empty());
    EXPECT_TRUE(has_log_containing(result.run, "no steps"));

    node.stop();
}

TEST(Executor, CyclicGraphFailsBeforeExecuting) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "a", {}, "a");
    graph.add_node(StepKind::Clean, "b", {}, "b");
    graph.add_edge("a", "b");
    graph.add_edge("b", "a");

    const ExecutionResult result = node.run_graph(graph);
    ASSERT_TRUE(result.run);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.run->status(), RunStatus::Failed);
    EXPECT_TRUE(has_log_containing(result.run, "cycle"));

    node.stop();
}

// ---- the produced Run is schema-valid + survives restart -------------------

TEST(Executor, ProducedRunIsSchemaValidAndPersists) {
    TempDir tmp;
    std::string run_id;
    {
        Node node{config_for(tmp)};
        node.start();
        const ExecutionResult result = node.run_graph(churn_pipeline());
        ASSERT_TRUE(result.ok);
        run_id = result.run->id();

        // The produced value passes the Run JSON Schema (the storage boundary).
        std::string error;
        EXPECT_TRUE(zetta::pipeline::validate_run_value(result.run->metadata(), &error))
            << error;
        node.stop();
    }

    // A fresh node reloads the run purely from run.json — load_runs() routes it
    // through parse_run_value, so a malformed value would be dropped. Its
    // survival proves the executor wrote a schema-valid record.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    auto run = reloaded.find_run(run_id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), RunStatus::Succeeded);
    EXPECT_EQ(run->outputs().size(), 4u);
    reloaded.stop();
}

// ---- the StepExecutor seam (custom per-step compute) -----------------------

namespace {

// A StepExecutor that fails the Nth step it is asked to run — proving the seam
// the orchestrator runs steps through is pluggable (real compute lands here).
class FailOnNthStep final : public StepExecutor {
public:
    explicit FailOnNthStep(int n) : nth_(n) {}
    StepOutcome run_step(const zetta::ir::Node&) const override {
        if (++seen_ == nth_) {
            return StepOutcome{false, "injected failure", {}, {}};
        }
        return StepOutcome{};
    }

private:
    int nth_;
    mutable int seen_ = 0;
};

}  // namespace

TEST(Executor, CustomStepExecutorIsHonoured) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    ExecutionGraph graph;
    auto a = graph.add_node(StepKind::Connect, "a", {}, "a");
    auto b = graph.add_node(StepKind::Clean, "b", {}, "b");
    auto c = graph.add_node(StepKind::Model, "c", {}, "c");
    graph.add_edge(a->id, b->id);
    graph.add_edge(b->id, c->id);

    GraphExecutor executor{std::make_shared<FailOnNthStep>(2)};
    const ExecutionResult result = executor.execute(node, graph);
    ASSERT_TRUE(result.run);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failed_step, "b");
    EXPECT_EQ(result.error, "injected failure");
    EXPECT_EQ(result.run->status(), RunStatus::Failed);

    node.stop();
}

// --- Run tracing (#640) --------------------------------------------------------

// Every run is a trace: the executor stamps the replicated run record with a
// trace id and records a run root span + one child span per step into the
// node's ring — so system.traces / get_trace answer "what happened" for a run.
TEST(Executor, RunRecordsATraceWithPerStepSpans) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    GraphExecutor executor;
    const ExecutionResult result =
        executor.execute(node, churn_pipeline(), "proj", "churn_v1");
    ASSERT_TRUE(result.ok) << result.error;

    const std::string trace_id = result.run->value().trace_id;
    ASSERT_EQ(trace_id.size(), 32u);

    const auto spans = node.span_recorder().trace(trace_id);
    std::string run_span_id;
    std::size_t step_spans = 0;
    for (const auto& span : spans) {
        if (span.name == "run:churn_v1") {
            run_span_id = span.span_id;
            EXPECT_EQ(span.attributes.at("plan").at("steps"), 6);
            EXPECT_TRUE(span.ok);
        }
    }
    ASSERT_FALSE(run_span_id.empty());
    for (const auto& span : spans) {
        if (span.name.starts_with("step:")) {
            ++step_spans;
            EXPECT_EQ(span.parent_span_id, run_span_id);
            EXPECT_TRUE(span.ok);
        }
    }
    EXPECT_EQ(step_spans, 6u);

    node.stop();
}

// ---- cooperative cancel (#624) ----------------------------------------------

TEST(Executor, CancelBetweenStepsEndsTheRunFailed) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    // The flag is set BEFORE execute(), so the first between-steps check sees
    // it: no step runs, the run ends Failed with the cancelled log line.
    std::atomic<bool> cancel{true};
    GraphExecutor executor;
    const ExecutionResult result =
        executor.execute(node, churn_pipeline(), "proj", "graph", &cancel);

    ASSERT_TRUE(result.run);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "run cancelled by user");
    EXPECT_TRUE(result.failed_step.empty()) << "no step failed — it was cancelled";
    EXPECT_EQ(result.run->status(), RunStatus::Failed);
    EXPECT_TRUE(result.run->outputs().empty()) << "no step ran";
    EXPECT_TRUE(has_log_containing(result.run, "run cancelled by user"));

    node.stop();
}

TEST(Executor, NullCancelExecutesEveryStep) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    GraphExecutor executor;
    const ExecutionResult result =
        executor.execute(node, churn_pipeline(), "proj", "graph", nullptr);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.run->status(), RunStatus::Succeeded);

    node.stop();
}
