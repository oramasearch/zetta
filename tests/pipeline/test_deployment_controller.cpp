#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_controller.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::pipeline::DeploymentTarget;
using zetta::Node;
using zetta::NodeConfig;
using zetta::pipeline::ReconcileReport;
using zetta::cluster::ReplicationManager;
using zetta::pipeline::RunStatus;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A small connect→clean→model chain — the same shape the deployment tests use,
// enough for a deployment to record a non-trivial graph summary.
ExecutionGraph sample_graph() {
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "load_orders");
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model");
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    return graph;
}

// The runs the controller has tied to a given deployment.
std::vector<std::shared_ptr<zetta::pipeline::Run>> runs_for(Node& node,
                                                  const std::string& deployment) {
    std::vector<std::shared_ptr<zetta::pipeline::Run>> result;
    for (const auto& run : node.runs()) {
        if (run->deployment() == deployment) result.push_back(run);
    }
    return result;
}

// Tick both nodes until `done` holds (or we give up). Mirrors the deployment /
// data replication tests: capture -> cluster gossip -> peer merge -> reconcile.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

}  // namespace

// ---- desired → actual: a local deployment gets a run -----------------------

TEST(DeploymentController, ReconcileStartsRunForLocalDeployment) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto deployment =
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);
    EXPECT_EQ(node.run_count(), 0u);  // deploy records desired state only

    // First pass: desired > actual → start a running run for the deployment.
    const ReconcileReport report = node.deployment_controller().reconcile();
    EXPECT_EQ(report.local_deployments, 1u);
    EXPECT_EQ(report.satisfied, 0u);
    EXPECT_EQ(report.runs_started, 1u);
    ASSERT_EQ(report.healed.size(), 1u);
    EXPECT_EQ(report.healed[0], deployment->id());

    auto runs = runs_for(node, deployment->id());
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0]->status(), RunStatus::Running);
    EXPECT_EQ(runs[0]->deployment(), deployment->id());
    EXPECT_FALSE(runs[0]->logs().empty());  // the (re)start is logged

    node.stop();
}

// ---- steady state: a satisfied deployment is left alone --------------------

TEST(DeploymentController, SteadyStateStartsNothing) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto deployment =
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);

    node.deployment_controller().reconcile();  // starts the run
    ASSERT_EQ(runs_for(node, deployment->id()).size(), 1u);

    // Already-running deployment is satisfied — a second pass is a no-op scan.
    const ReconcileReport again = node.deployment_controller().reconcile();
    EXPECT_EQ(again.local_deployments, 1u);
    EXPECT_EQ(again.satisfied, 1u);
    EXPECT_EQ(again.runs_started, 0u);
    EXPECT_EQ(runs_for(node, deployment->id()).size(), 1u);

    // A *succeeded* run is terminal and likewise satisfies — not restarted.
    runs_for(node, deployment->id())[0]->set_status(RunStatus::Succeeded);
    const ReconcileReport after_success = node.deployment_controller().reconcile();
    EXPECT_EQ(after_success.satisfied, 1u);
    EXPECT_EQ(after_success.runs_started, 0u);
    EXPECT_EQ(runs_for(node, deployment->id()).size(), 1u);

    node.stop();
}

// ---- self-heal: kill the placed run, the controller restores it (TC-3) -----

TEST(DeploymentController, SelfHealsWhenRunIsKilled) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto deployment =
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);

    node.deployment_controller().reconcile();
    auto runs = runs_for(node, deployment->id());
    ASSERT_EQ(runs.size(), 1u);
    const std::string killed_id = runs[0]->id();

    // Kill the placed run — drop_run removes it from desired→actual coverage.
    ASSERT_TRUE(node.drop_run(killed_id));
    EXPECT_TRUE(runs_for(node, deployment->id()).empty());

    // Reconcile restores desired state: a fresh running run, different id.
    const ReconcileReport healed = node.deployment_controller().reconcile();
    EXPECT_EQ(healed.runs_started, 1u);
    ASSERT_EQ(healed.healed.size(), 1u);
    EXPECT_EQ(healed.healed[0], deployment->id());

    auto restored = runs_for(node, deployment->id());
    ASSERT_EQ(restored.size(), 1u);
    EXPECT_NE(restored[0]->id(), killed_id);
    EXPECT_EQ(restored[0]->status(), RunStatus::Running);

    node.stop();
}

// ---- self-heal: a failed run is replaced -----------------------------------

TEST(DeploymentController, SelfHealsWhenRunFails) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto deployment =
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);

    node.deployment_controller().reconcile();
    auto runs = runs_for(node, deployment->id());
    ASSERT_EQ(runs.size(), 1u);
    runs[0]->set_status(RunStatus::Failed);  // the placed run fails

    // A deployment whose only run failed is unsatisfied → restart (not drop).
    const ReconcileReport healed = node.deployment_controller().reconcile();
    EXPECT_EQ(healed.runs_started, 1u);

    // The failed run is kept (history); a new running run now satisfies it.
    auto all = runs_for(node, deployment->id());
    ASSERT_EQ(all.size(), 2u);
    int running = 0, failed = 0;
    for (const auto& run : all) {
        if (run->status() == RunStatus::Running) ++running;
        if (run->status() == RunStatus::Failed) ++failed;
    }
    EXPECT_EQ(running, 1);
    EXPECT_EQ(failed, 1);

    // Now satisfied again — no further churn.
    EXPECT_EQ(node.deployment_controller().reconcile().runs_started, 0u);

    node.stop();
}

// ---- the loop is driven automatically by Node::tick() ----------------------

TEST(DeploymentController, TickDrivesReconcileAndCounters) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto deployment =
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);

    // No manual reconcile — a single heartbeat tick starts the placed run.
    node.tick();
    EXPECT_EQ(runs_for(node, deployment->id()).size(), 1u);
    EXPECT_GE(node.deployment_controller().passes(), 1u);
    EXPECT_EQ(node.deployment_controller().total_runs_started(), 1u);

    // Kill it; the next tick self-heals without any explicit call.
    ASSERT_TRUE(node.drop_run(runs_for(node, deployment->id())[0]->id()));
    node.tick();
    EXPECT_EQ(runs_for(node, deployment->id()).size(), 1u);
    EXPECT_EQ(node.deployment_controller().total_runs_started(), 2u);

    node.stop();
}

// ---- R1 scope: a node reconciles only the deployments it placed itself ------

TEST(DeploymentController, DoesNotReconcilePeerPlacedDeployments) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43330;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43331;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43330"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Deploy on A only; B converges to the same deployment record.
    auto deployed =
        a.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployed);
    const bool converged = converge(a, b, [&] {
        auto seen = b.find_deployment(deployed->id());
        return seen && repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    // A placed it → A's controller started exactly one run for it.
    auto runs_on_a = runs_for(a, deployed->id());
    ASSERT_EQ(runs_on_a.size(), 1u);

    // B sees the deployment but did NOT place it (node_id == A) → B's control
    // plane never treats it as local and never starts a run for it. (A's run
    // record *does* replicate onto B — everything is replicated state — so the
    // assertion is on what B's *controller* did, not on what B can see.)
    const ReconcileReport on_b = b.deployment_controller().reconcile();
    EXPECT_EQ(on_b.local_deployments, 0u);
    EXPECT_EQ(on_b.runs_started, 0u);
    EXPECT_EQ(b.deployment_controller().total_runs_started(), 0u);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
