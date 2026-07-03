#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/solution_catalog.hpp"

// #398 acceptance: the catalog churn SolutionCatalog blueprint deploys + runs on the
// REAL engine (featurize -> train -> explain -> rank -> playbook), producing a real
// trained Model + ranked drivers — not the stubbed DefaultStepExecutor. The wiring
// under test is zetta::usecases::churn::run_churn_solution (churn_solution.hpp), driven by the catalog
// "churn-prediction" graph. Single-node here; the single-node == multi-node
// equivalence of the underlying jobs is proven in the per-op suites.
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
using zetta::compute::JobCoordinator;
using zetta::ml::EntityScore;

// A single-node loopback compute node: Node + a one-node Cluster + a started
// JobCoordinator over it (the harness the per-op job suites use).
struct ComputeNode {
    explicit ComputeNode(std::uint16_t port)
        : node(node_config(temp_dir)),
          cluster("churn-solution-node", cluster_config(port)),
          coordinator(cluster, /*worker_count=*/2) {
        node.start();
        cluster.start();
        coordinator.start();
    }
    ~ComputeNode() { coordinator.stop(); }
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
    JobCoordinator coordinator;
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

    ComputeNode host(24500);

    // (0) Deploy: the gate passes (RULE-1) and the placement is recorded — the
    // "deploy" half of the acceptance.
    ASSERT_TRUE(zetta::pipeline::default_gate().run(graph).passed())
        << "catalog churn blueprint must pass the correctness gate";
    const DeploymentTarget target{DeploymentKind::Local, {}, {}};
    const std::shared_ptr<Deployment> deployment =
        host.node.deploy_graph(graph, target, solution->name);
    ASSERT_TRUE(deployment) << "deploy_graph rejected the catalog churn blueprint";

    // (1) Run it on the real engine.
    CpuDevice device;
    ChurnSolutionOptions options;
    options.project = "churn-project";
    options.graph = "churn-graph-1";
    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution(
        host.node, host.coordinator, device, graph, loaded.dataset, options);

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

    // Each heavy step ran as a recorded, Succeeded ComputeJob (system.zetta_jobs).
    int featurize = 0, train = 0, score = 0, explain = 0;
    for (const auto& job : host.node.jobs()) {
        EXPECT_EQ(job->status(), JobStatus::Succeeded) << job->id();
        switch (job->kind()) {
            case JobKind::Featurize: ++featurize; break;
            case JobKind::Train:     ++train; break;
            case JobKind::Score:     ++score; break;
            case JobKind::Explain:   ++explain; break;
            case JobKind::Ddf:       break;
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

    ComputeNode host(24510);
    CpuDevice device;
    const zetta::usecases::churn::ChurnDataset demo = zetta::usecases::churn::demo_churn_dataset();
    ASSERT_FALSE(demo.accounts.empty());

    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution(
        host.node, host.coordinator, device, solution->graph, demo);

    ASSERT_TRUE(result.ok()) << result.execution.error;
    EXPECT_EQ(result.ranked.size(), demo.accounts.size());
    EXPECT_FALSE(result.model_id.empty());
    EXPECT_FALSE(result.drivers.empty());
    // The demo's only class-separating signal is payment failures.
    EXPECT_EQ(result.top_driver, "failed_payments");
}

// =====================================================================
// #580: run over the node's LIVE JobCoordinator (the #579 spine), not an
// isolated loopback. A node that has been attach_cluster'd owns a coordinator;
// run_churn_solution_on_node resolves it and runs the real pipeline there.
// =====================================================================

// A node with its OWN long-lived coordinator over its cluster — the #579 spine,
// the way main wires a running node (node.attach_cluster after cluster.start()).
struct AttachedNode {
    explicit AttachedNode(std::uint16_t port)
        : node(ComputeNode::node_config(temp_dir)),
          cluster("churn-attached-node", ComputeNode::cluster_config(port)) {
        node.start();
        cluster.start();
        node.attach_cluster(cluster);  // brings up node.job_coordinator()
    }
    ~AttachedNode() { node.stop(); }  // tears the coordinator down while cluster alive
    zetta::test::TempDir temp_dir;    // declared first so it outlives `node`
    Node node;
    Cluster cluster;
};

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
    Node node(ComputeNode::node_config(tmp));
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

}  // namespace
