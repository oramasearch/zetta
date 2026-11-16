// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "zetta/agent/collectives_allreduce.hpp"
#include "zetta/agent/distributed.hpp"
#include "zetta/agent/distributed_training.hpp"
#include "zetta/agent/softmax_policy.hpp"
#include "zetta/agent/training.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/collectives.hpp"
#include "zetta/runtime.hpp"

namespace {

using zetta::agent::collectives_allreduce;
using zetta::agent::Reduce;
using zetta::agent::RewardTask;
using zetta::agent::SoftmaxPolicy;
using zetta::agent::train_softmax_policy_distributed;
using zetta::agent::TrainingConfig;
using zetta::agent::TrainingReport;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::Collectives;

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

// One node of a loopback cluster: a Cluster plus its Collectives.
struct Node {
    Node(std::string id, std::uint16_t port, const std::vector<std::string>& seeds)
        : cluster(std::move(id), make_config(port, seeds)), collectives(cluster) {}

    static ClusterConfig make_config(std::uint16_t port,
                                     const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    Cluster cluster;
    Collectives collectives;
};

RewardTask bandit_reward(std::size_t target) {
    return [target](std::size_t action) -> zetta::runtime::io_task<double> {
        co_return {{}, action == target ? 1.0 : 0.0};
    };
}

// The whole point: two real cluster nodes train ONE policy in lock-step. Each node
// samples its own local rollouts (different RNG), the gradients are allreduced over
// the signal plane, and every node applies the same global update — so the two
// policies stay bit-identical and both learn the rewarding arm. Single-node ==
// multi-node, on a real cluster.
TEST(CollectivesAllreduce, TwoNodesTrainOnePolicyInLockStep) {
    const std::uint16_t base_port = 45360;
    Node node_a("node-a", base_port, {});  // node-a is the deterministic root
    Node node_b("node-b", base_port + 1,
                {"127.0.0.1:" + std::to_string(base_port)});
    node_a.cluster.start();
    node_b.cluster.start();
    ASSERT_TRUE(wait_for([&] {
        return node_a.cluster.peer_count() == 1 &&
               node_b.cluster.peer_count() == 1;
    }));

    const std::size_t arms = 4;
    const std::size_t target = 1;
    SoftmaxPolicy policy_a(arms);
    SoftmaxPolicy policy_b(arms);
    const TrainingConfig config{8, 25, 0.5};

    auto run = [&](SoftmaxPolicy& policy, Collectives& collectives,
                   unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        zetta::runtime::block_on(train_softmax_policy_distributed(
            policy, bandit_reward(target), [&] { return dist(rng); },
            collectives_allreduce(collectives), config));
    };

    // Both nodes train concurrently, rendezvousing at each allreduce.
    std::thread thread_a([&] { run(policy_a, node_a.collectives, 1); });
    std::thread thread_b([&] { run(policy_b, node_b.collectives, 2); });
    thread_a.join();
    thread_b.join();

    node_a.cluster.stop();
    node_b.cluster.stop();

    // Both nodes ended with the SAME policy (identical global updates each step)...
    EXPECT_EQ(policy_a.logits(), policy_b.logits());
    // ...and it learned the rewarding arm.
    EXPECT_GT(policy_a.probabilities()[target], 0.5);
    EXPECT_GT(policy_b.probabilities()[target], 0.5);
}

// The adapter forwards a round to the cluster collective: on a singleton cluster an
// allreduce-Sum of one participant returns that participant's own vector.
TEST(CollectivesAllreduce, AdapterForwardsOnASingletonCluster) {
    Cluster cluster{"solo-node", ClusterConfig{}};
    Collectives collectives{cluster};
    cluster.start();

    const auto allreduce = collectives_allreduce(collectives);
    const std::vector<double> result =
        allreduce("round", {2.0, 4.0, 8.0}, Reduce::Sum);
    EXPECT_EQ(result, (std::vector<double>{2.0, 4.0, 8.0}));

    cluster.stop();
}

}  // namespace
