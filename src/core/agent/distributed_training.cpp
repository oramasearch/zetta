// SPDX-License-Identifier: MIT

#include "zetta/agent/distributed_training.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "zetta/agent/policy_gradient.hpp"  // PolicySample

namespace zetta::agent {

runtime::task<TrainingReport> train_softmax_policy_distributed(
    SoftmaxPolicy& policy, const RewardTask& reward, const UniformSource& uniform,
    const AllReduce& allreduce, const TrainingConfig& config) {
    TrainingReport report;
    report.mean_reward_per_step.reserve(config.steps);

    for (std::size_t step = 0; step < config.steps; ++step) {
        // Sample this node's local group, then run the rollouts CONCURRENTLY: each
        // is an io_task, so a real tool call's I/O overlaps across the group.
        std::vector<std::size_t> actions(config.group_size, 0);
        std::vector<runtime::io_task<double>> rollouts;
        rollouts.reserve(config.group_size);
        for (std::size_t i = 0; i < config.group_size; ++i) {
            actions[i] = policy.sample(uniform());
            rollouts.push_back(reward(actions[i]));
        }
        auto [error, rewards] = co_await runtime::when_all(std::move(rollouts));
        if (error) {
            break;  // a rollout's I/O failed the join; stop with the curve so far
        }

        // Step-unique tags so consecutive collective rounds never alias on a real
        // cluster (every node issues the same tags in the same order, in lock-step).
        const std::string suffix = "-" + std::to_string(step);

        // Global GRPO advantages for this node's rewards (allreduce of the group
        // statistics), then the distributed gradient step (allreduce of the
        // gradient). Either collective failing ends training.
        const std::vector<double> advantages =
            distributed_group_advantages(rewards, allreduce, "grpo-adv" + suffix);
        if (advantages.size() != rewards.size()) {
            break;
        }
        std::vector<PolicySample> batch(config.group_size);
        for (std::size_t i = 0; i < config.group_size; ++i) {
            batch[i] = PolicySample{actions[i], advantages[i]};
        }
        if (!distributed_policy_gradient_step(policy, batch, allreduce,
                                              config.learning_rate,
                                              "grpo-grad" + suffix)) {
            break;
        }

        // The GLOBAL mean reward this step (allreduce the local sum + count).
        double local_sum = 0.0;
        for (double value : rewards) {
            local_sum += value;
        }
        const std::vector<double> global = allreduce(
            "reward-mean" + suffix,
            {local_sum, static_cast<double>(rewards.size())}, Reduce::Sum);
        const double mean =
            (global.size() == 2 && global[1] > 0.0) ? global[0] / global[1] : 0.0;
        report.mean_reward_per_step.push_back(mean);
    }

    co_return report;
}

}  // namespace zetta::agent
