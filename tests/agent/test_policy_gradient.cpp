#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <vector>

#include "zetta/agent/advantage.hpp"
#include "zetta/agent/policy_gradient.hpp"
#include "zetta/agent/softmax_policy.hpp"

namespace {

using zetta::agent::group_advantages;
using zetta::agent::policy_gradient_step;
using zetta::agent::PolicySample;
using zetta::agent::SoftmaxPolicy;

TEST(PolicyGradient, PositiveAdvantageRaisesActionProbability) {
    SoftmaxPolicy policy(3);
    const double before = policy.probabilities()[1];
    policy_gradient_step(policy, {PolicySample{1, +1.0}}, 0.5);
    EXPECT_GT(policy.probabilities()[1], before);
}

TEST(PolicyGradient, NegativeAdvantageLowersActionProbability) {
    SoftmaxPolicy policy(3);
    const double before = policy.probabilities()[1];
    policy_gradient_step(policy, {PolicySample{1, -1.0}}, 0.5);
    EXPECT_LT(policy.probabilities()[1], before);
}

TEST(PolicyGradient, EmptyBatchIsANoop) {
    SoftmaxPolicy policy({0.5, -0.5});
    const std::vector<double> before = policy.logits();
    policy_gradient_step(policy, {}, 0.5);
    EXPECT_EQ(policy.logits(), before);
}

// The point of the whole loop: train a bandit and watch the reward rise. One arm
// is correct (reward 1); the policy starts uniform. Sample a group, turn rewards
// into GRPO advantages, take a gradient step, repeat — the correct arm's
// probability climbs to near-certainty. feat-12 "watch it improve", in miniature.
TEST(PolicyGradient, RewardRisesOverTrainingOnABandit) {
    const std::size_t arms = 4;
    const std::size_t target = 2;
    SoftmaxPolicy policy(arms);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    const double start_probability = policy.probabilities()[target];  // ~0.25

    const std::size_t group_size = 8;
    for (int iteration = 0; iteration < 200; ++iteration) {
        std::vector<double> rewards(group_size);
        std::vector<PolicySample> batch(group_size);
        for (std::size_t i = 0; i < group_size; ++i) {
            const std::size_t action = policy.sample(uniform(rng));
            batch[i].action = action;
            rewards[i] = (action == target) ? 1.0 : 0.0;
        }
        const std::vector<double> advantages = group_advantages(rewards);
        for (std::size_t i = 0; i < group_size; ++i) {
            batch[i].advantage = advantages[i];
        }
        policy_gradient_step(policy, batch, /*learning_rate=*/0.5);
    }

    const std::vector<double> trained = policy.probabilities();
    EXPECT_GT(trained[target], 0.9);              // reward-getting arm dominates
    EXPECT_GT(trained[target], start_probability);
}

}  // namespace
