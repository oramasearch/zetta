// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <vector>

#include "zetta/agent/distributed.hpp"
#include "zetta/agent/linear_policy.hpp"
#include "zetta/agent/neural_training.hpp"
#include "zetta/agent/training.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/runtime.hpp"

namespace {

using zetta::agent::ContextSource;
using zetta::agent::ContextualRewardTask;
using zetta::agent::identity_allreduce;
using zetta::agent::LinearPolicy;
using zetta::agent::train_linear_policy_distributed;
using zetta::agent::TrainingConfig;
using zetta::agent::TrainingReport;
using zetta::compute::CpuDevice;

// A CONTEXTUAL bandit: context [1,0] rewards action 0; context [0,1] rewards action
// 1. A tabular policy cannot solve this (it has no context) — only a policy that
// maps the feature vector to an action can, which is why this exercises the neural
// weights. The linear policy starts at W = 0 (uniform) and must learn W.
TEST(NeuralTraining, LinearPolicyLearnsAContextualTaskOnTheDevice) {
    LinearPolicy policy(/*action_count=*/2, /*feature_count=*/2,
                        std::vector<float>(4, 0.0F));
    CpuDevice device;
    std::mt19937 rng(19);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    std::bernoulli_distribution coin(0.5);

    // Half the contexts are [1,0], half [0,1].
    ContextSource context = [&]() -> std::vector<float> {
        return coin(rng) ? std::vector<float>{1.0F, 0.0F}
                         : std::vector<float>{0.0F, 1.0F};
    };
    ContextualRewardTask reward =
        [](const std::vector<float>& ctx,
           std::size_t action) -> zetta::runtime::io_task<double> {
        const std::size_t correct = ctx[0] > 0.5F ? 0u : 1u;
        co_return {{}, action == correct ? 1.0 : 0.0};
    };

    const TrainingReport report =
        zetta::runtime::block_on(train_linear_policy_distributed(
            policy, device, context, reward, [&] { return dist(rng); },
            identity_allreduce, TrainingConfig{16, 250, 0.5}));

    EXPECT_TRUE(report.improved());
    EXPECT_GT(report.last_reward(), 0.8);

    // The trained weights map each context to its rewarding action.
    const std::vector<double> on_context_a =
        policy.probabilities(device, {1.0F, 0.0F});
    const std::vector<double> on_context_b =
        policy.probabilities(device, {0.0F, 1.0F});
    EXPECT_GT(on_context_a[0], 0.7);  // context [1,0] -> action 0
    EXPECT_GT(on_context_b[1], 0.7);  // context [0,1] -> action 1
}

}  // namespace
