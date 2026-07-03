#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/solution_catalog.hpp"
#include "zetta/util/uuid.hpp"

// #601 acceptance: the coordinated cross-process SPMD launch. One node submits
// an interactive churn run over a REAL multi-node loopback cluster (each member
// wired exactly the way main wires a live process: ReplicationManager + Node +
// ReplicatedBlobStore + Cluster + attach_cluster); the runner fans a
// `run.launch` task out over the signal plane, every peer co-runs the same
// graph against its own disjoint entity shard on its own node-owning tick
// thread (the cross-process shape — no shared test thread drives all members),
// the collectives combine the partials, and:
//
//   * single-node == multi-node — the coordinated run's ranked at-risk list,
//     risks, and top driver match a single-member oracle run over the same
//     dataset + options;
//   * ONE logical Model registers (the collective root's) and, because the
//     members replicate, it resolves identically on EVERY member.
//
// Timing-bound like every multi-node suite: convergence loops poll with a
// patient deadline, never a fixed small count (tests/CLAUDE.md).
namespace {

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::compute::CpuDevice;
using zetta::ml::EntityScore;
using zetta::Node;
using zetta::NodeConfig;
using zetta::pipeline::Solution;
using zetta::pipeline::SolutionCatalog;
using zetta::usecases::churn::ChurnDataset;
using zetta::usecases::churn::ChurnSolutionOptions;
using zetta::usecases::churn::ChurnSolutionResult;

// One live cluster member, wired the way main() wires a real process: the
// replication actor id doubles as the cluster node id, the CRDT gossips over
// the cluster link, large bodies ride the blob store, and the node owns its
// long-lived compute plane (which #601 extends with the run.launch listener).
struct Member {
    Member(std::uint16_t port, const std::vector<std::string>& seeds)
        : replication(id),
          node(node_config(dir), &replication),
          blobs(dir.path(), replication),
          cluster(id, cluster_config(port, seeds), &replication, &blobs) {
        node.start();
        cluster.start();
        replication.set(
            "node:" + id,
            nlohmann::json{{"address", cluster.listen_addresses().front()}});
        node.attach_cluster(cluster, /*worker_count=*/2);
    }
    ~Member() { node.stop(); }  // tears the compute plane down while cluster lives

    static NodeConfig node_config(const zetta::test::TempDir& dir) {
        NodeConfig config;
        config.data_dir = dir.path().string();
        config.preflight = false;  // fresh scratch dir; nothing to check
        return config;
    }
    static ClusterConfig cluster_config(std::uint16_t port,
                                        const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 4;
        config.seeds = seeds;
        return config;
    }

    zetta::test::TempDir dir;  // declared first: outlives everything below
    std::string id = zetta::util::new_uuid();
    ReplicationManager replication;
    Node node;
    zetta::data::ReplicatedBlobStore blobs;
    Cluster cluster;
};

// Bring up `count` members sequentially (each seeded with the actually-bound
// addresses of the earlier ones) and wait for membership to converge.
std::vector<std::unique_ptr<Member>> bring_up(std::uint16_t base_port,
                                              std::size_t count) {
    std::vector<std::unique_ptr<Member>> members;
    std::vector<std::string> seeds;
    for (std::size_t index = 0; index < count; ++index) {
        members.push_back(std::make_unique<Member>(
            static_cast<std::uint16_t>(base_port + index * 4), seeds));
        seeds.push_back(members.back()->cluster.listen_address());
    }
    const std::size_t expected = count - 1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = true;
        for (const auto& member : members) {
            if (member->cluster.peer_count() != expected) all = false;
        }
        if (all) return members;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return members;  // caller asserts peer counts
}

bool converged(const std::vector<std::unique_ptr<Member>>& members) {
    for (const auto& member : members) {
        if (member->cluster.peer_count() != members.size() - 1) return false;
    }
    return true;
}

// Small-but-real training knobs so the multi-round SPMD combine is exercised
// without minutes of loopback rounds. Identical for the oracle and the
// coordinated run — the equivalence assertion needs the same program.
ChurnSolutionOptions run_options() {
    ChurnSolutionOptions options;
    options.params.num_trees = 10;
    options.params.max_depth = 3;
    options.params.max_bins = 16;
    options.params.learning_rate = 0.3;
    options.project = "coordinated-churn";
    options.graph = "churn-prediction";
    return options;
}

std::map<std::int64_t, double> risks_by_entity(
    const std::vector<EntityScore>& ranked) {
    std::map<std::int64_t, double> risks;
    for (const EntityScore& score : ranked) risks[score.entity] = score.risk;
    return risks;
}

TEST(ChurnCoordinatedLaunch, MultiNodeCoordinatedRunMatchesSingleNodeOracle) {
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr) << "built-in catalog has no churn-prediction";
    const zetta::ir::ExecutionGraph& graph = solution->graph;
    const ChurnDataset dataset = zetta::usecases::churn::demo_churn_dataset();
    const ChurnSolutionOptions options = run_options();

    // ---- The oracle: the same run on a single-member cluster (the roster is
    // just that node, so the coordinated path degenerates to the local run).
    ChurnSolutionResult oracle;
    {
        const auto solo = bring_up(24760, 1);
        CpuDevice device;
        oracle = zetta::usecases::churn::run_churn_solution_on_node(
            solo[0]->node, device, graph, dataset, options);
        ASSERT_TRUE(oracle.ok()) << oracle.execution.error;
        ASSERT_EQ(oracle.ranked.size(), dataset.accounts.size());
    }

    // ---- The coordinated run: three real members over loopback TCP.
    const auto members = bring_up(24768, 3);
    ASSERT_TRUE(converged(members)) << "3-node loopback cluster did not converge";

    // Each peer runs its own node loop — ticks drain the `run.launch` queue and
    // execute the co-run on that owner thread, exactly like a live process.
    std::atomic<bool> stop{false};
    const auto tick_loop = [&stop](Node& node) {
        while (!stop.load()) {
            node.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    std::thread peer_one(tick_loop, std::ref(members[1]->node));
    std::thread peer_two(tick_loop, std::ref(members[2]->node));

    // The initiator submits interactively from its own thread; the fan-out +
    // its local shard run happen inside the one public runner.
    CpuDevice device;
    const ChurnSolutionResult result =
        zetta::usecases::churn::run_churn_solution_on_node(
            members[0]->node, device, graph, dataset, options);
    stop.store(true);
    peer_one.join();
    peer_two.join();

    ASSERT_TRUE(result.ok())
        << "coordinated multi-node run failed: " << result.execution.error;

    // ---- Single-node == multi-node: the combined artifacts are global, not
    // one shard's. Every account appears exactly once (disjoint shards, union
    // == the whole — no double-counting through the allreduce).
    ASSERT_EQ(result.ranked.size(), dataset.accounts.size());
    const std::map<std::int64_t, double> oracle_risks =
        risks_by_entity(oracle.ranked);
    const std::map<std::int64_t, double> coordinated_risks =
        risks_by_entity(result.ranked);
    ASSERT_EQ(coordinated_risks.size(), oracle_risks.size());
    for (const auto& [entity, oracle_risk] : oracle_risks) {
        const auto found = coordinated_risks.find(entity);
        ASSERT_NE(found, coordinated_risks.end())
            << "entity " << entity << " missing from the coordinated ranking";
        EXPECT_NEAR(found->second, oracle_risk, 1e-3)
            << "entity " << entity << " risk diverged from the oracle";
    }
    EXPECT_EQ(result.top_driver, oracle.top_driver);
    EXPECT_EQ(result.drivers.size(), oracle.drivers.size());

    // ---- ONE logical Model registers (the collective root's) and replicates:
    // the SAME model id resolves on every member. Patient poll — gossip +
    // reconcile need real time on a loopback cluster.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string model_id;
    bool everywhere = false;
    while (std::chrono::steady_clock::now() < deadline && !everywhere) {
        for (const auto& member : members) member->node.tick();
        model_id.clear();
        for (const auto& member : members) {
            for (const std::shared_ptr<zetta::ml::Model>& model :
                 member->node.models()) {
                if (model->name() == options.model_name) {
                    model_id = model->id();
                    break;
                }
            }
            if (!model_id.empty()) break;
        }
        if (!model_id.empty()) {
            everywhere = true;
            for (const auto& member : members) {
                if (!member->node.find_model(model_id)) everywhere = false;
            }
        }
        if (!everywhere) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ASSERT_FALSE(model_id.empty())
        << "no member registered the coordinated run's model";
    EXPECT_TRUE(everywhere)
        << "model " << model_id << " did not replicate to every member";
    // Exactly one logical model: no member registered a duplicate of its own.
    for (const auto& member : members) {
        std::size_t churn_models = 0;
        for (const std::shared_ptr<zetta::ml::Model>& model :
             member->node.models()) {
            if (model->name() == options.model_name) ++churn_models;
        }
        EXPECT_EQ(churn_models, 1u)
            << "member registered duplicate churn models";
    }
}

TEST(ChurnCoordinatedLaunch, OptOutRunsThisNodeAloneOnSingleMemberCluster) {
    // coordinate = false on a single-member cluster is the pre-#601 behavior:
    // the whole dataset runs locally and succeeds (there are no peers whose
    // missing contributions could stall the collectives).
    const Solution* solution =
        SolutionCatalog::builtin().find("churn-prediction");
    ASSERT_NE(solution, nullptr);
    const auto solo = bring_up(24784, 1);

    ChurnSolutionOptions options = run_options();
    options.coordinate = false;
    CpuDevice device;
    const ChurnSolutionResult result =
        zetta::usecases::churn::run_churn_solution_on_node(
            solo[0]->node, device, solution->graph,
            zetta::usecases::churn::demo_churn_dataset(), options);
    ASSERT_TRUE(result.ok()) << result.execution.error;
    EXPECT_EQ(result.ranked.size(),
              zetta::usecases::churn::demo_churn_dataset().accounts.size());
}

}  // namespace
