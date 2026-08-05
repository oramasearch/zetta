#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include "zetta/agent/distributed.hpp"
#include "zetta/agent/distributed_training.hpp"
#include "zetta/agent/softmax_policy.hpp"
#include "zetta/agent/training.hpp"
#include "zetta/runtime.hpp"

namespace {

using zetta::agent::AllReduce;
using zetta::agent::identity_allreduce;
using zetta::agent::Reduce;
using zetta::agent::RewardTask;
using zetta::agent::SoftmaxPolicy;
using zetta::agent::train_softmax_policy_distributed;
using zetta::agent::TrainingConfig;
using zetta::agent::TrainingReport;

// The coroutine loop trains a bandit: rollouts fan out via when_all, the step
// allreduces (identity here = single node). Reward rises to the target arm.
TEST(DistributedTraining, TrainsABanditThroughCoroutinesAndAllreduce) {
    const std::size_t arms = 4;
    const std::size_t target = 2;
    SoftmaxPolicy policy(arms);
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    RewardTask reward =
        [target](std::size_t action) -> zetta::runtime::io_task<double> {
        co_return {{}, action == target ? 1.0 : 0.0};
    };

    const TrainingReport report =
        zetta::runtime::block_on(train_softmax_policy_distributed(
            policy, reward, [&] { return dist(rng); }, identity_allreduce,
            TrainingConfig{8, 150, 0.5}));

    EXPECT_TRUE(report.improved());
    EXPECT_GT(report.last_reward(), 0.8);
    EXPECT_GT(policy.probabilities()[target], 0.8);
}

// The reported reward is the global mean; the curve has one entry per step.
TEST(DistributedTraining, ReportsAMeanRewardPerStep) {
    SoftmaxPolicy policy(2);
    std::mt19937 rng(2);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    RewardTask reward =
        [](std::size_t action) -> zetta::runtime::io_task<double> {
        co_return {{}, action == 0 ? 1.0 : 0.0};
    };

    const TrainingReport report =
        zetta::runtime::block_on(train_softmax_policy_distributed(
            policy, reward, [&] { return dist(rng); }, identity_allreduce,
            TrainingConfig{8, 40, 0.5}));

    EXPECT_EQ(report.mean_reward_per_step.size(), 40u);
    for (double mean : report.mean_reward_per_step) {
        EXPECT_GE(mean, 0.0);
        EXPECT_LE(mean, 1.0);
    }
}

// A failing collective ends training early rather than training on partial data.
TEST(DistributedTraining, CollectiveFailureEndsTrainingEarly) {
    SoftmaxPolicy policy(3);
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    RewardTask reward =
        [](std::size_t) -> zetta::runtime::io_task<double> { co_return {{}, 1.0}; };
    const AllReduce failing = [](const std::string&, const std::vector<double>&,
                                 Reduce) { return std::vector<double>{}; };

    const TrainingReport report =
        zetta::runtime::block_on(train_softmax_policy_distributed(
            policy, reward, [&] { return dist(rng); }, failing,
            TrainingConfig{8, 50, 0.5}));

    EXPECT_TRUE(report.mean_reward_per_step.empty());  // stopped before recording
}

}  // namespace
