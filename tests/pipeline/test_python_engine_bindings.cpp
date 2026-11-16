// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/cloud/burst_approval_gate.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/input/cluster_bindings.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/pipeline/run.hpp"

// #522 acceptance: the sim-Python cluster + ML verbs are *bound to the engine* — each
// call lowers to the correct IR node AND executes the right work end-to-end.
//
//   train(ds, target=…)               -> model step  -> native GBDT train (a real Model)
//   rank(model) / score(…)            -> index step  -> distributed batch scoring
//   explain(model)                    -> serve step  -> exact TreeSHAP (churn drivers)
//   cluster.add_nodes(n, provider=…)  -> act step    -> zetta::cloud::add_nodes (permanent-add)
//
// The three ML verbs need no new execution code: lowering them to model/index/serve
// makes the existing ChurnStepExecutor (via run_churn_solution_on_node) run them. The
// cluster control verb runs through the dedicated binding run_add_nodes_step.
namespace {

using zetta::cloud::AddNodesResult;
using zetta::usecases::churn::ChurnSolutionResult;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::ml::Model;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;
using zetta::cloud::Provisioner;
using zetta::pipeline::RunStatus;
using zetta::compute::CpuDevice;
using zetta::input::PythonFrontend;
using zetta::input::run_add_nodes_step;
using zetta::ir::StepKind;

// A single-node loopback node with its OWN long-lived coordinator over its cluster
// (node.attach_cluster, the #579 spine) — so the churn run goes through the one
// public runner, run_churn_solution_on_node (#637).
struct ComputeNode {
    explicit ComputeNode(std::uint16_t port)
        : node(node_config(temp_dir)),
          cluster("python-bindings-node", cluster_config(port)) {
        node.start();
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);
    }
    ~ComputeNode() { node.stop(); }  // tears the coordinator down while cluster alive
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

// A fake burst-pool Provisioner at the seam (the add_nodes control action programs
// against the abstract Provisioner&, so it never sees a control plane). It records
// teardown calls so the test can prove the permanent-add never tears a member down.
class FakeProvisioner : public Provisioner {
public:
    std::vector<ProvisionedNode> pool;
    int teardown_calls = 0;

    ProvisionResult provision(const ProvisionRequest& request) override {
        pool.clear();
        for (std::size_t index = 0; index < request.count; ++index) {
            ProvisionedNode node;
            node.node_id = "node-" + std::to_string(index);
            node.address = "10.1.0." + std::to_string(index) + ":7777";
            node.capability = request.spec;
            pool.push_back(node);
        }
        ProvisionResult result;
        result.nodes = pool;
        result.satisfied = pool.size() >= request.count;
        result.detail = result.satisfied ? "added" : "best-effort short";
        return result;
    }
    bool teardown(const std::string&) override {
        ++teardown_calls;
        return false;
    }
    std::vector<ProvisionedNode> provisioned() const override { return pool; }
};

}  // namespace

// =====================================================================
// The ML verbs (train / rank / explain) execute end-to-end through the real engine.
// =====================================================================

// A churn pipeline written as a sim-Python script lowers to a connect x2 -> clean ->
// model -> index -> serve -> act graph and runs through run_churn_solution_on_node
// exactly like the catalog blueprint — producing a real Model, the ranked at-risk
// list, and the churn drivers. This is the train/rank/explain half of the #522
// acceptance.
TEST(PythonEngineBindings, ChurnScriptTrainsRanksAndExplainsEndToEnd) {
    const auto lowered = PythonFrontend{}.compile(
        "events   = read_csv('events.csv')\n"
        "accounts = read_csv('accounts.csv')\n"
        "features = featurize(events, accounts)\n"
        "model    = train(features, target='churned')\n"
        "at_risk  = rank(model)\n"
        "drivers  = explain(model)\n"
        "playbook = act(at_risk, drivers)\n");
    ASSERT_TRUE(lowered.ok()) << lowered.error;

    // Each ML verb lowered to the stage whose engine op it names.
    EXPECT_EQ(lowered.graph.find_node("train")->kind, StepKind::Model);
    EXPECT_EQ(lowered.graph.find_node("rank")->kind, StepKind::Index);
    EXPECT_EQ(lowered.graph.find_node("explain")->kind, StepKind::Serve);

    ComputeNode host(24560);
    CpuDevice device;
    const zetta::usecases::churn::ChurnDataset demo = zetta::usecases::churn::demo_churn_dataset();
    ASSERT_FALSE(demo.accounts.empty());

    const ChurnSolutionResult result = zetta::usecases::churn::run_churn_solution_on_node(
        host.node, device, lowered.graph, demo);

    // The lowered graph executed end to end on the real engine.
    ASSERT_TRUE(result.ok()) << result.execution.error;
    ASSERT_TRUE(result.execution.run);
    EXPECT_EQ(result.execution.run->status(), RunStatus::Succeeded);

    // train -> a real registered Model.
    EXPECT_FALSE(result.model_id.empty());
    const std::shared_ptr<Model> registered = host.node.find_model(result.model_id);
    ASSERT_TRUE(registered) << "train step did not register a real Model";
    EXPECT_EQ(registered->kind(), "classifier");

    // rank -> the ranked at-risk list (one per account, highest risk first).
    ASSERT_FALSE(result.ranked.empty());
    EXPECT_EQ(result.ranked.size(), demo.accounts.size());
    for (std::size_t i = 1; i < result.ranked.size(); ++i) {
        EXPECT_GE(result.ranked[i - 1].risk, result.ranked[i].risk) << "rank " << i;
    }

    // explain -> the SHAP churn-driver ranking (failed payments dominates the demo).
    ASSERT_FALSE(result.drivers.empty());
    EXPECT_EQ(result.top_driver, "failed_payments");
}

// =====================================================================
// The cluster control verb (add_nodes) executes the permanent-add control action.
// =====================================================================

// cluster.add_nodes(3, provider=…) lowers to an act step; run_add_nodes_step reads
// the count and drives zetta::cloud::add_nodes over the injected provisioner.
TEST(PythonEngineBindings, ClusterAddNodesDrivesThePermanentAddAction) {
    const auto lowered =
        PythonFrontend{}.compile("cluster.add_nodes(3, provider='azure')\n");
    ASSERT_TRUE(lowered.ok()) << lowered.error;
    const auto step = lowered.graph.find_node("add_nodes");
    ASSERT_TRUE(step);
    EXPECT_EQ(step->kind, StepKind::Act);

    FakeProvisioner provider;
    const AddNodesResult added = run_add_nodes_step(*step, provider);

    EXPECT_TRUE(added.satisfied);
    ASSERT_EQ(added.members.size(), 3u);
    EXPECT_EQ(provider.provisioned().size(), 3u);
    // Permanent-add: it provisions but never tears a member down.
    EXPECT_EQ(provider.teardown_calls, 0);
}

// The count can also come from a keyword argument (cluster.add_nodes(n=5)); a step
// with no usable count is a satisfied no-op, like add_nodes(provider, 0).
TEST(PythonEngineBindings, AddNodesCountFromKeywordAndNoOpWhenAbsent) {
    FakeProvisioner provider;

    const auto by_kw = PythonFrontend{}.compile("cluster.add_nodes(n=5)\n");
    ASSERT_TRUE(by_kw.ok()) << by_kw.error;
    const AddNodesResult five =
        run_add_nodes_step(*by_kw.graph.find_node("add_nodes"), provider);
    EXPECT_EQ(five.members.size(), 5u);

    const auto no_count = PythonFrontend{}.compile("cluster.add_nodes()\n");
    ASSERT_TRUE(no_count.ok()) << no_count.error;
    const AddNodesResult none =
        run_add_nodes_step(*no_count.graph.find_node("add_nodes"), provider);
    EXPECT_TRUE(none.satisfied);
    EXPECT_TRUE(none.members.empty());
}

// The burst cost cap is read from the cost_cap= keyword (#502 / TC-12). A script
// with no cap — or a zero cap, which BurstCap treats as "unlimited" — reads as
// "no cap named" so the ungated path runs.
TEST(PythonEngineBindings, AddNodesCostCapReadFromKeyword) {
    const auto capped = PythonFrontend{}.compile(
        "cluster.add_nodes(10, provider='azure', cost_cap=12)\n");
    ASSERT_TRUE(capped.ok()) << capped.error;
    const auto cap = zetta::input::add_nodes_cost_cap(*capped.graph.find_node("add_nodes"));
    ASSERT_TRUE(cap.has_value());
    EXPECT_DOUBLE_EQ(*cap, 12.0);

    const auto uncapped = PythonFrontend{}.compile("cluster.add_nodes(10)\n");
    ASSERT_TRUE(uncapped.ok()) << uncapped.error;
    EXPECT_FALSE(zetta::input::add_nodes_cost_cap(*uncapped.graph.find_node("add_nodes")));

    const auto zero = PythonFrontend{}.compile("cluster.add_nodes(10, cost_cap=0)\n");
    ASSERT_TRUE(zero.ok()) << zero.error;
    EXPECT_FALSE(zetta::input::add_nodes_cost_cap(*zero.graph.find_node("add_nodes")));
}

// The Node-aware execution composes the burst approval gate when the script names
// a cost_cap=: an over-cap request is blocked deny-by-default — the inner backend
// is never touched — and the decision lands in the node's audit trail; a
// within-cap request provisions through the gate and the members join.
TEST(PythonEngineBindings, GatedAddNodesEnforcesTheScriptCostCap) {
    ComputeNode host(24572);
    // A transparent unit-cost estimator: one relative compute unit per node, so a
    // request's cost is simply its count.
    const zetta::cloud::CostEstimator unit_cost = [](const ProvisionRequest& request) {
        return static_cast<double>(request.count);
    };

    // Over cap: 4 nodes at 1 unit each against a cap of 2 — blocked, audited.
    const auto over = PythonFrontend{}.compile(
        "cluster.add_nodes(4, provider='azure', cost_cap=2)\n");
    ASSERT_TRUE(over.ok()) << over.error;
    FakeProvisioner blocked_provider;
    const AddNodesResult blocked = run_add_nodes_step(
        *over.graph.find_node("add_nodes"), host.node, blocked_provider, {}, unit_cost);
    EXPECT_FALSE(blocked.satisfied);
    EXPECT_TRUE(blocked.members.empty());
    EXPECT_TRUE(blocked_provider.provisioned().empty())
        << "a blocked burst must never reach the inner backend";
    int provision_requested = 0;
    for (const auto& event : host.node.audit_events()) {
        if (event->action() == zetta::governance::AuditAction::ProvisionRequested) {
            ++provision_requested;
        }
    }
    EXPECT_EQ(provision_requested, 1) << "the over-cap decision was not audited";

    // Within cap: 3 nodes against a cap of 8 — the gate passes it through.
    const auto within = PythonFrontend{}.compile(
        "cluster.add_nodes(3, provider='azure', cost_cap=8)\n");
    ASSERT_TRUE(within.ok()) << within.error;
    FakeProvisioner provider;
    const AddNodesResult added = run_add_nodes_step(
        *within.graph.find_node("add_nodes"), host.node, provider, {}, unit_cost);
    EXPECT_TRUE(added.satisfied);
    EXPECT_EQ(added.members.size(), 3u);
}
