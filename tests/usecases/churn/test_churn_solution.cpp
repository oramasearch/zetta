// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <limits>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/pipeline/solution_catalog.hpp"

// #398 acceptance: the catalog churn SolutionCatalog blueprint deploys + runs on the
// REAL engine (featurize -> train -> explain -> rank -> playbook), producing a real
// trained Model + ranked drivers — not the stubbed DefaultStepExecutor. The wiring
// under test is zetta::usecases::churn::run_churn_solution_on_node (churn_solution.hpp) —
// the ONE public churn runner (#637), over the node's live JobCoordinator (the #579
// spine, #580) — driven by the catalog "churn-prediction" graph. Single-node here;
// the single-node == multi-node equivalence of the underlying jobs is proven in the
// per-op suites.
namespace {

namespace fs = std::filesystem;

using zetta::usecases::churn::ChurnSolutionOptions;
using zetta::usecases::churn::ChurnSolutionResult;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::pipeline::Deployment;
using zetta::pipeline::DeploymentKind;
using zetta::pipeline::DeploymentTarget;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::ml::Model;
using zetta::Node;
using zetta::NodeConfig;
using zetta::usecases::churn::RetentionAction;
using zetta::pipeline::RunOutput;
using zetta::pipeline::RunStatus;
using zetta::pipeline::Solution;
using zetta::pipeline::SolutionCatalog;
using zetta::compute::CpuDevice;
using zetta::ml::EntityScore;

// A single-node loopback node with its OWN long-lived coordinator over its cluster —
// the #579 spine, the way main wires a running node (node.attach_cluster after
// cluster.start()). Every churn run in this suite goes through the one public
// runner, run_churn_solution_on_node, over this node-owned coordinator (#637).
struct AttachedNode {
    explicit AttachedNode(std::uint16_t port)
        : node(node_config(temp_dir)),
          cluster("churn-attached-node", cluster_config(port)) {
        node.start();
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);  // brings up node.job_coordinator()
    }
    ~AttachedNode() { node.stop(); }  // tears the coordinator down while cluster alive
    static NodeConfig node_config(const zetta::test::TempDir& dir) {
        NodeConfig cfg;
        cfg.data_dir = dir.path().string();
        return cfg;
    }
    static ClusterConfig cluster_config(std::uint16_t port) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        return config;
    }
    zetta::test::TempDir temp_dir;  // declared first so it outlives `node`
    Node node;
    Cluster cluster;
};

// =====================================================================
// Deploy the catalog churn solution, then run it on the real engine.
// =====================================================================

TEST(ChurnSolution, DeployCatalogChurnAndRunProducesRealModelAndDrivers) {
    // The blueprint a user picks in the Deploy panel.
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr) << "built-in catalog has no churn-prediction";
    const zetta::ir::ExecutionGraph& graph = solution->graph;

    // The committed synthetic churn fixture as this node's input (single-node =>
    // the whole dataset).
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::usecases::churn::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;

    AttachedNode host(24500);

    // (0) Deploy: the gate passes (RULE-1) and the placement is recorded — the
    // "deploy" half of the acceptance.
    ASSERT_TRUE(zetta::pipeline::default_gate().run(graph).passed())
        << "catalog churn blueprint must pass the correctness gate";
    const DeploymentTarget target{DeploymentKind::Local, {}, {}};
    const std::shared_ptr<Deployment> deployment =
        host.node.deploy_graph(graph, target, solution->name);
    ASSERT_TRUE(deployment) << "deploy_graph rejected the catalog churn blueprint";

    // (1) Run it on the real engine, over the node's live coordinator.
    CpuDevice device;
    ChurnSolutionOptions options;
    options.project = "churn-project";
    options.graph = "churn-graph-1";
    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        host.node, device, graph, loaded.dataset, options);

    // The run succeeded end to end and is schema-valid.
    ASSERT_TRUE(result.ok()) << result.execution.error;
    ASSERT_TRUE(result.execution.run);
    EXPECT_EQ(result.execution.run->status(), RunStatus::Succeeded);
    EXPECT_TRUE(result.execution.failed_step.empty());
    std::string schema_error;
    EXPECT_TRUE(zetta::pipeline::validate_run_value(result.execution.run->metadata(),
                                          &schema_error))
        << schema_error;

    // (2) A `model` output that resolves to a REAL registered Model.
    const std::vector<RunOutput>& outputs = result.execution.run->outputs();
    const auto model_output =
        std::find_if(outputs.begin(), outputs.end(),
                     [](const RunOutput& o) { return o.kind == "model"; });
    ASSERT_NE(model_output, outputs.end()) << "run has no model output";
    EXPECT_FALSE(result.model_id.empty());
    EXPECT_EQ(model_output->ref, result.model_id);
    const std::shared_ptr<Model> registered =
        host.node.find_model(result.model_id);
    ASSERT_TRUE(registered) << "model output ref does not resolve to a real Model";
    EXPECT_EQ(registered->kind(), "classifier");

    // (3) An `index` output — the ranked at-risk list (one per account, highest
    // risk first). This is the step the catalog blueprint was missing before #398.
    const auto index_output =
        std::find_if(outputs.begin(), outputs.end(),
                     [](const RunOutput& o) { return o.kind == "index"; });
    ASSERT_NE(index_output, outputs.end()) << "run has no index output";
    ASSERT_FALSE(result.ranked.empty());
    EXPECT_EQ(result.ranked.size(), loaded.dataset.accounts.size());
    for (std::size_t i = 1; i < result.ranked.size(); ++i) {
        EXPECT_GE(result.ranked[i - 1].risk, result.ranked[i].risk) << "rank " << i;
    }

    // (4) Ranked drivers — the SHAP population ranking; failed payments dominates
    // the churn of this fixture.
    ASSERT_FALSE(result.drivers.empty());
    EXPECT_EQ(result.drivers.front().first, "failed_payments");
    EXPECT_EQ(result.top_driver, "failed_payments");

    // (5) The retention playbook joins the at-risk list with each entity's driver.
    EXPECT_EQ(result.playbook.size(), result.ranked.size());
    ASSERT_FALSE(result.playbook.empty());
    EXPECT_EQ(result.playbook.front().entity, result.ranked.front().entity);
    EXPECT_TRUE(std::any_of(
        result.playbook.begin(), result.playbook.end(),
        [](const RetentionAction& a) { return !a.dominant_driver.empty(); }));

    // Each heavy step ran as a recorded, Succeeded ComputeJob (system.jobs).
    int featurize = 0, train = 0, score = 0, explain = 0;
    for (const auto& job : host.node.jobs()) {
        EXPECT_EQ(job->status(), JobStatus::Succeeded) << job->id();
        switch (job->kind()) {
            case JobKind::Featurize: ++featurize; break;
            case JobKind::Train:     ++train; break;
            case JobKind::Score:     ++score; break;
            case JobKind::Explain:   ++explain; break;
            case JobKind::Ddf:       break;
            case JobKind::Ingest:    break;  // no ingest step in this graph
        }
    }
    EXPECT_EQ(featurize, 1);
    EXPECT_EQ(train, 1);
    EXPECT_EQ(score, 1);
    EXPECT_EQ(explain, 1);
}

// The built-in demo dataset (no files) also runs end to end on the catalog graph,
// the way the headed Deploy panel's "Run on real engine" action drives it.
TEST(ChurnSolution, DemoDatasetRunsEndToEnd) {
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    AttachedNode host(24510);
    CpuDevice device;
    const zetta::usecases::churn::ChurnDataset demo = zetta::usecases::churn::demo_churn_dataset();
    ASSERT_FALSE(demo.accounts.empty());

    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        host.node, device, solution->graph, demo);

    ASSERT_TRUE(result.ok()) << result.execution.error;
    EXPECT_EQ(result.ranked.size(), demo.accounts.size());
    EXPECT_FALSE(result.model_id.empty());
    EXPECT_FALSE(result.drivers.empty());
    // The demo's only class-separating signal is payment failures.
    EXPECT_EQ(result.top_driver, "failed_payments");
}

// =====================================================================
// #958: the run emits a `report` output, and the facts behind it are readable
// back through the control surface.
// =====================================================================

// A finished run has to be able to say what it FOUND, to someone who was not
// there when it ran. Before this the numbers lived in the executor's memory and a
// test measured its own; the report had no engine-side source.
TEST(ChurnSolution, RunEmitsAReportOutputAndItsFactsReadBack) {
    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    AttachedNode host(24520);
    CpuDevice device;
    const zetta::usecases::churn::ChurnDataset demo =
        zetta::usecases::churn::demo_churn_dataset();

    zetta::usecases::churn::ChurnSolutionOptions options;
    // Ask for a real holdout, so the report can lead with a figure measured on
    // rows the model never saw (#953) rather than the flattering one.
    options.params.holdout_fraction = 0.3;
    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        host.node, device, solution->graph, demo, options);
    ASSERT_TRUE(result.ok()) << result.execution.error;
    ASSERT_TRUE(result.execution.run);

    // The run names a report among its outputs, and its ref is the run itself.
    std::string report_ref;
    for (const zetta::pipeline::RunOutput& output : result.execution.run->outputs()) {
        if (output.kind == "report") report_ref = output.ref;
    }
    ASSERT_FALSE(report_ref.empty()) << "the run emitted no report output";
    EXPECT_EQ(report_ref, result.execution.run->id());

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::ApiContext context{host.node};
    const zetta::api::ApiResult read = registry.call(
        "get_run_report", {{"run_id", report_ref}, {"limit", 5}}, context);
    ASSERT_TRUE(read.ok) << read.error.message;
    const nlohmann::json& report = read.value;

    // What the model was built FROM. These survive the run only because they are
    // written onto the Model record; the feature table is long gone by now.
    ASSERT_TRUE(report.contains("dataset")) << report.dump(2);
    const nlohmann::json& dataset = report.at("dataset");
    EXPECT_EQ(dataset.value("accounts", 0.0), static_cast<double>(demo.accounts.size()));
    EXPECT_EQ(dataset.value("events", 0.0), static_cast<double>(demo.events.size()));
    const double rate = dataset.value("churn_rate", -1.0);
    EXPECT_GT(rate, 0.0);
    EXPECT_LT(rate, 1.0);
    // The accuracy a constant answer scores — emitted so a writer never has to
    // work it out, which is the computation the report rule forbids.
    EXPECT_DOUBLE_EQ(dataset.value("majority_class_accuracy", 0.0),
                     std::max(rate, 1.0 - rate));
    EXPECT_FALSE(dataset.at("features").empty());

    // Both figures, and the held-out one present because the run asked for it.
    const nlohmann::json& model = report.at("model");
    EXPECT_EQ(model.value("id", std::string{}), result.model_id);
    ASSERT_TRUE(model.contains("held_out_auc")) << model.dump(2);
    EXPECT_TRUE(model.contains("in_sample_auc"));
    EXPECT_GT(model.value("held_out_rows", 0.0), 0.0);

    // The drivers, and the ranked list capped at what was asked for, each row
    // carrying the evidence values a reader judges it by.
    EXPECT_FALSE(report.at("drivers").empty());
    const nlohmann::json& at_risk = report.at("at_risk");
    ASSERT_FALSE(at_risk.empty());
    EXPECT_LE(at_risk.size(), 5u);
    EXPECT_EQ(at_risk[0].value("rank", 0), 1);
    ASSERT_TRUE(at_risk[0].contains("evidence")) << at_risk[0].dump(2);
    EXPECT_FALSE(at_risk[0].at("evidence").empty());
    // The entity must survive as a 64-bit key (#1000) — see the dedicated test
    // below for why a plausible-looking `0` default silently truncates it.
    EXPECT_TRUE(at_risk[0].at("entity").is_number_integer()) << at_risk[0].dump(2);
}

// #1000: the ranked at-risk list is the artifact a customer is shown, and its
// entity ids were wrapped to 32 bits — so the report disagreed with `score` and
// with this node's own `churn_results.at_risk_*` table, and joining the list
// back to account records returned nothing.
//
// The cause is a one-token trap worth pinning directly: `json::value<T>`
// deduces T from the DEFAULT, so `row.value("account", 0)` asks for an `int`.
// Entity keys are `std::int64_t` the whole way through the ML path, and a
// derived key — the documented remedy for a string business id (#957) — is
// above 2^31 about half the time.
TEST(ChurnSolution, AnEntityKeyAboveTwoToThe31SurvivesTheReport) {
    // The exact value from the measured failure: it read back as -1949009714.
    constexpr std::int64_t kBigEntity = 2345957582;
    static_assert(kBigEntity > std::numeric_limits<std::int32_t>::max());

    const nlohmann::json row = {{"account", kBigEntity}, {"rank", 2},
                                {"risk", 0.8807439523346903}};

    // What the report builder must NOT do — kept as the guard, so anyone who
    // "simplifies" the default back to a bare literal fails here.
    EXPECT_NE(row.value("account", 0), kBigEntity)
        << "an int default no longer truncates; the guard below is now vacuous";
    EXPECT_EQ(row.value("account", 0), -1949009714);

    // What it does.
    EXPECT_EQ(row.value("account", std::int64_t{0}), kBigEntity);
}

// A run that asked for no holdout must SAY so where the held-out figure would
// have been. Left blank, a report leads with the in-sample number and calls it
// quality — which is the failure #953 exists for.
TEST(ChurnSolution, ReportNamesTheFiguresItCannotSupply) {
    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    AttachedNode host(24530);
    CpuDevice device;
    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        host.node, device, solution->graph, zetta::usecases::churn::demo_churn_dataset());
    ASSERT_TRUE(result.ok()) << result.execution.error;

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::ApiContext context{host.node};
    const zetta::api::ApiResult read = registry.call(
        "get_run_report", {{"run_id", result.execution.run->id()}}, context);
    ASSERT_TRUE(read.ok) << read.error.message;

    EXPECT_FALSE(read.value.at("model").contains("held_out_auc"));
    ASSERT_TRUE(read.value.contains("missing")) << read.value.dump(2);
    bool named = false;
    for (const nlohmann::json& entry : read.value.at("missing")) {
        if (entry.value("field", std::string{}) == "held_out_auc") {
            named = true;
            EXPECT_FALSE(entry.value("why", std::string{}).empty());
        }
    }
    EXPECT_TRUE(named) << "the report did not say why it has no held-out figure";
}

// #1007: the report reads a table the run wrote — replicated state, which a peer
// or a customer's dataset shaped. A column of an unexpected type (a null rank, a
// string account id that was never derived to a key) made nlohmann's typed
// accessors THROW inside the read, and a throw on the owner loop is a request
// nobody answers. The report must come back, carrying the odd row as it is.
TEST(ChurnSolution, ReportToleratesAtRiskRowsOfAnUnexpectedShape) {
    const Solution* solution = SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    AttachedNode host(24535);
    CpuDevice device;
    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        host.node, device, solution->graph, zetta::usecases::churn::demo_churn_dataset());
    ASSERT_TRUE(result.ok()) << result.execution.error;

    // The at-risk table the run materialized, by the run's own `dataset` output.
    std::shared_ptr<zetta::data::Table> at_risk_table;
    for (const zetta::pipeline::RunOutput& output : result.execution.run->outputs()) {
        const std::size_t dot = output.ref.find('.');
        if (output.kind != "dataset" || dot == std::string::npos) continue;
        if (auto database = host.node.find_database(output.ref.substr(0, dot))) {
            at_risk_table = database->find_table(output.ref.substr(dot + 1));
        }
        if (at_risk_table) break;
    }
    ASSERT_TRUE(at_risk_table) << "the run materialized no at-risk table";
    at_risk_table->add_row(
        {{"rank", nullptr}, {"account", "acct-0042"}, {"risk", "high"}});

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::ApiContext context{host.node};
    const zetta::api::ApiResult read = registry.call(
        "get_run_report",
        {{"run_id", result.execution.run->id()}, {"limit", 1000}}, context);
    ASSERT_TRUE(read.ok) << read.error.code << ": " << read.error.message;

    bool carried = false;
    for (const nlohmann::json& entry : read.value.at("at_risk")) {
        if (entry.at("entity") == "acct-0042") {
            carried = true;
            EXPECT_EQ(entry.at("risk"), 0.0) << entry.dump(2);
        }
    }
    EXPECT_TRUE(carried) << "the odd row was dropped rather than reported as-is";
}

// =====================================================================
// #580: run over the node's LIVE JobCoordinator (the #579 spine), not an
// isolated loopback. A node that has been attach_cluster'd owns a coordinator;
// run_churn_solution_on_node resolves it and runs the real pipeline there.
// =====================================================================

TEST(ChurnSolution, RunOnNodeOverLiveCoordinatorProducesRealModel) {
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    AttachedNode host(24520);
    ASSERT_NE(host.node.job_coordinator(), nullptr)
        << "attach_cluster must bring up the live coordinator";

    CpuDevice device;
    const zetta::usecases::churn::ChurnDataset demo = zetta::usecases::churn::demo_churn_dataset();
    // No coordinator argument — it runs on the node's OWN live spine.
    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        host.node, device, solution->graph, demo);

    ASSERT_TRUE(result.ok()) << result.execution.error;
    ASSERT_TRUE(result.execution.run);
    EXPECT_EQ(result.execution.run->status(), RunStatus::Succeeded);
    EXPECT_FALSE(result.model_id.empty());
    EXPECT_EQ(result.ranked.size(), demo.accounts.size());
    EXPECT_EQ(result.top_driver, "failed_payments");

    // The Model + Run register on the LIVE node (visible in the Runs / Models
    // surfaces — the #580 acceptance), not some throwaway loopback node.
    const std::shared_ptr<Model> registered = host.node.find_model(result.model_id);
    ASSERT_TRUE(registered) << "model must register on the live node";
    EXPECT_EQ(registered->kind(), "classifier");
    EXPECT_FALSE(host.node.runs().empty());
}

TEST(ChurnSolution, RunOnNodeWithoutClusterFailsClearly) {
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    // A standalone node — never attach_cluster'd — has no live coordinator.
    zetta::test::TempDir tmp;
    Node node(AttachedNode::node_config(tmp));
    node.start();
    ASSERT_EQ(node.job_coordinator(), nullptr);

    CpuDevice device;
    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        node, device, solution->graph, zetta::usecases::churn::demo_churn_dataset());

    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.execution.error.empty());
    EXPECT_FALSE(result.execution.run) << "nothing should run without a coordinator";
    EXPECT_TRUE(node.models().empty()) << "no Model should register on failure";
    node.stop();
}

TEST(ChurnSolution, IsChurnGraphRecognizesBackbone) {
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);
    // The catalog churn blueprint has the connect -> clean -> model backbone.
    EXPECT_TRUE(zetta::usecases::churn::is_churn_graph(solution->graph));

    // A graph missing the featurize (clean) step is not churn-shaped.
    zetta::ir::ExecutionGraph load_then_train;
    load_then_train.add_node(zetta::ir::StepKind::Connect, "load");
    load_then_train.add_node(zetta::ir::StepKind::Model, "train");
    EXPECT_FALSE(zetta::usecases::churn::is_churn_graph(load_then_train));

    EXPECT_FALSE(zetta::usecases::churn::is_churn_graph(zetta::ir::ExecutionGraph{}));
}

// =====================================================================
// #624: the async run — off the caller's thread, node writes gated
// =====================================================================

TEST(ChurnSolution, AsyncRunCompletesWhileTheOwnerLoopTicks) {
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    AttachedNode host(24530);
    ASSERT_NE(host.node.job_coordinator(), nullptr);

    ChurnSolutionOptions options;
    options.project = "async-churn";
    options.graph = "churn-prediction";
    const std::shared_ptr<zetta::usecases::churn::AsyncChurnRun> run =
        zetta::usecases::churn::AsyncChurnRun::start(
            host.node, solution->graph,
            zetta::usecases::churn::demo_churn_dataset(), options);

    // This thread is the node's owner: keep ticking — tick() is the drain
    // point for the worker's gated node writes — until the worker finishes.
    // Patient, like the multi-node convergence loops.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!run->done() && std::chrono::steady_clock::now() < deadline) {
        host.node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(run->done()) << "the async churn run must finish";

    const ChurnSolutionResult& result = run->result();
    ASSERT_TRUE(result.ok()) << result.execution.error;
    EXPECT_FALSE(result.model_id.empty());
    // The records the panels / MCP poll for live progress landed on the node.
    ASSERT_TRUE(result.execution.run);
    EXPECT_EQ(result.execution.run->status(), RunStatus::Succeeded);
    EXPECT_EQ(result.execution.run->project(), "async-churn");
    EXPECT_TRUE(host.node.find_model(result.model_id));
    EXPECT_FALSE(host.node.jobs().empty());
}

TEST(ChurnSolution, AsyncRunCancelEndsTheRunFailed) {
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);

    AttachedNode host(24540);
    ChurnSolutionOptions options;
    options.project = "async-churn-cancel";
    options.graph = "churn-prediction";
    const std::shared_ptr<zetta::usecases::churn::AsyncChurnRun> run =
        zetta::usecases::churn::AsyncChurnRun::start(
            host.node, solution->graph,
            zetta::usecases::churn::demo_churn_dataset(), options);

    // Cancel BEFORE the first tick: the worker's first gated node write has
    // not drained yet, so the flag is set before the first between-steps
    // check — the cancel path is deterministic, not a race.
    run->request_cancel();
    EXPECT_TRUE(run->cancel_requested());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!run->done() && std::chrono::steady_clock::now() < deadline) {
        host.node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(run->done());

    const ChurnSolutionResult& result = run->result();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.execution.error, "run cancelled by user");
    ASSERT_TRUE(result.execution.run);
    EXPECT_EQ(result.execution.run->status(), RunStatus::Failed);
    EXPECT_TRUE(host.node.models().empty())
        << "no step ran, so no Model registers";
}

}  // namespace
