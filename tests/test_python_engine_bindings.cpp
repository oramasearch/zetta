#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/churn_solution.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/input/cluster_bindings.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"
#include "zetta/provisioner.hpp"
#include "zetta/run.hpp"

// #522 acceptance: the sim-Python cluster + ML verbs are *bound to the engine* — each
// call lowers to the correct IR node AND executes the right work end-to-end.
//
//   train(ds, target=…)               -> model step  -> native GBDT train (a real Model)
//   rank(model) / score(…)            -> index step  -> distributed batch scoring
//   explain(model)                    -> serve step  -> exact TreeSHAP (churn drivers)
//   cluster.add_nodes(n, provider=…)  -> act step    -> zetta::add_nodes (permanent-add)
//
// The three ML verbs need no new execution code: lowering them to model/index/serve
// makes the existing ChurnStepExecutor (via run_churn_solution) run them. The cluster
// control verb runs through the dedicated binding run_add_nodes_step.
namespace {

using zetta::AddNodesResult;
using zetta::ChurnSolutionResult;
using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::Model;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ProvisionedNode;
using zetta::ProvisionRequest;
using zetta::ProvisionResult;
using zetta::Provisioner;
using zetta::RunStatus;
using zetta::compute::CpuDevice;
using zetta::compute::JobCoordinator;
using zetta::input::PythonFrontend;
using zetta::input::run_add_nodes_step;
using zetta::ir::StepKind;

// A single-node loopback compute node (the harness the per-op job + churn-solution
// suites use): Node + a one-node Cluster + a started JobCoordinator over it.
struct ComputeNode {
    explicit ComputeNode(std::uint16_t port)
        : node(node_config(temp_dir)),
          cluster("python-bindings-node", cluster_config(port)),
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
// model -> index -> serve -> act graph and runs through run_churn_solution exactly
// like the catalog blueprint — producing a real Model, the ranked at-risk list, and
// the churn drivers. This is the train/rank/explain half of the #522 acceptance.
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
    const zetta::ChurnDataset demo = zetta::demo_churn_dataset();
    ASSERT_FALSE(demo.accounts.empty());

    const ChurnSolutionResult result = zetta::run_churn_solution(
        host.node, host.coordinator, device, lowered.graph, demo);

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
// the count and drives zetta::add_nodes over the injected provisioner.
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
