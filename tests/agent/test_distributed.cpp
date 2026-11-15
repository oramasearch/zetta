// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "zetta/agent/advantage.hpp"
#include "zetta/agent/distributed.hpp"
#include "zetta/agent/policy_gradient.hpp"
#include "zetta/agent/softmax_policy.hpp"

namespace {

using zetta::agent::AllReduce;
using zetta::agent::distributed_group_advantages;
using zetta::agent::distributed_policy_gradient_step;
using zetta::agent::group_advantages;
using zetta::agent::identity_allreduce;
using zetta::agent::local_policy_gradient;
using zetta::agent::PolicySample;
using zetta::agent::Reduce;
using zetta::agent::SoftmaxPolicy;

// An allreduce that simulates one peer node: it adds this node's `local` to a fixed
// peer contribution, exactly what Collectives::allreduce(Sum) computes for 2 nodes.
AllReduce sum_with_peer(std::vector<double> peer) {
    return [peer = std::move(peer)](const std::string&,
                                    const std::vector<double>& local,
                                    Reduce) -> std::vector<double> {
        std::vector<double> global(local.size(), 0.0);
        for (std::size_t i = 0; i < local.size(); ++i) {
            global[i] = local[i] + (i < peer.size() ? peer[i] : 0.0);
        }
        return global;
    };
}

std::vector<double> reward_partials(const std::vector<double>& rewards) {
    double sum = 0.0;
    double sq = 0.0;
    for (double r : rewards) {
        sum += r;
        sq += r * r;
    }
    return {sum, sq, static_cast<double>(rewards.size())};
}

TEST(Distributed, IdentityAllreduceReturnsLocal) {
    const std::vector<double> local = {1.0, 2.0, 3.0};
    EXPECT_EQ(identity_allreduce("t", local, Reduce::Sum), local);
}

// The gradient is additive across shards — the property that makes an allreduce-Sum
// of local gradients equal the gradient of the whole batch.
TEST(Distributed, LocalGradientIsAdditiveAcrossShards) {
    const SoftmaxPolicy policy({0.2, -0.1, 0.4});
    const std::vector<PolicySample> shard_a = {{0, 1.0}, {2, -0.5}};
    const std::vector<PolicySample> shard_b = {{1, 0.3}, {0, -0.2}, {2, 0.7}};
    std::vector<PolicySample> combined = shard_a;
    combined.insert(combined.end(), shard_b.begin(), shard_b.end());

    const std::vector<double> grad_a = local_policy_gradient(policy, shard_a);
    const std::vector<double> grad_b = local_policy_gradient(policy, shard_b);
    const std::vector<double> grad_combined = local_policy_gradient(policy, combined);

    ASSERT_EQ(grad_combined.size(), 3u);
    for (std::size_t j = 0; j < grad_combined.size(); ++j) {
        EXPECT_NEAR(grad_a[j] + grad_b[j], grad_combined[j], 1e-12);
    }
}

TEST(Distributed, IdentityAdvantagesMatchGroupAdvantages) {
    const std::vector<double> rewards = {1.0, 0.0, 0.5, 0.75, 0.2};
    const std::vector<double> distributed =
        distributed_group_advantages(rewards, identity_allreduce);
    const std::vector<double> single = group_advantages(rewards);
    ASSERT_EQ(distributed.size(), single.size());
    for (std::size_t i = 0; i < single.size(); ++i) {
        EXPECT_NEAR(distributed[i], single[i], 1e-9);
    }
}

// Sharded across 2 nodes, the concatenated advantages equal the single-node result:
// the group statistics are global by construction.
TEST(Distributed, ShardedAdvantagesMatchCombined) {
    const std::vector<double> shard_a = {1.0, 0.0, 0.5};
    const std::vector<double> shard_b = {0.75, 0.2, 0.9};
    std::vector<double> combined = shard_a;
    combined.insert(combined.end(), shard_b.begin(), shard_b.end());

    const std::vector<double> adv_a = distributed_group_advantages(
        shard_a, sum_with_peer(reward_partials(shard_b)));
    const std::vector<double> adv_b = distributed_group_advantages(
        shard_b, sum_with_peer(reward_partials(shard_a)));
    const std::vector<double> reference = group_advantages(combined);

    ASSERT_EQ(adv_a.size() + adv_b.size(), reference.size());
    for (std::size_t i = 0; i < adv_a.size(); ++i) {
        EXPECT_NEAR(adv_a[i], reference[i], 1e-9);
    }
    for (std::size_t i = 0; i < adv_b.size(); ++i) {
        EXPECT_NEAR(adv_b[i], reference[shard_a.size() + i], 1e-9);
    }
}

// THE distributed invariant: a training STEP with the batch sharded across two
// nodes (+ a summing allreduce) produces the same policy as the whole batch on one
// node. Single-node == multi-node by construction.
TEST(Distributed, ShardedGradientStepMatchesCombinedStep) {
    const SoftmaxPolicy base({0.1, -0.2, 0.3});
    const std::vector<PolicySample> shard_a = {{0, 1.0}, {2, -0.5}, {1, 0.4}};
    const std::vector<PolicySample> shard_b = {{1, 0.3}, {0, -0.2}, {2, 0.7}};
    std::vector<PolicySample> combined = shard_a;
    combined.insert(combined.end(), shard_b.begin(), shard_b.end());

    // Whole batch on one node.
    SoftmaxPolicy single = base;
    ASSERT_TRUE(distributed_policy_gradient_step(single, combined,
                                                 identity_allreduce, 0.5));

    // Sharded across two nodes: node A's peer gradient is B's local gradient at the
    // shared (unchanged) policy.
    SoftmaxPolicy sharded = base;
    const std::vector<double> peer_gradient =
        local_policy_gradient(sharded, shard_b);
    ASSERT_TRUE(distributed_policy_gradient_step(
        sharded, shard_a, sum_with_peer(peer_gradient), 0.5));

    ASSERT_EQ(single.logits().size(), sharded.logits().size());
    for (std::size_t j = 0; j < single.logits().size(); ++j) {
        EXPECT_NEAR(single.logits()[j], sharded.logits()[j], 1e-12);
    }
}

TEST(Distributed, CollectiveFailureLeavesPolicyUnchanged) {
    SoftmaxPolicy policy({0.5, -0.5});
    const std::vector<double> before = policy.logits();
    const AllReduce failing = [](const std::string&, const std::vector<double>&,
                                 Reduce) { return std::vector<double>{}; };
    EXPECT_FALSE(distributed_policy_gradient_step(policy, {{0, 1.0}}, failing, 0.5));
    EXPECT_EQ(policy.logits(), before);
}

}  // namespace
