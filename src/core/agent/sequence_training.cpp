// SPDX-License-Identifier: MIT

#include "zetta/agent/sequence_training.hpp"

#include <cstddef>
#include <vector>

#include "zetta/agent/advantage.hpp"
#include "zetta/agent/policy_gradient.hpp"

namespace zetta::agent {

TrainingReport train_sequence_policy(SequencePolicy& policy,
                                     const TrajectoryReward& reward,
                                     const UniformSource& uniform,
                                     const TrainingConfig& config) {
    TrainingReport report;
    report.mean_reward_per_step.reserve(config.steps);
    const std::size_t horizon = policy.horizon();

    for (std::size_t step = 0; step < config.steps; ++step) {
        // Sample a group of full trajectories.
        std::vector<std::vector<std::size_t>> trajectories(
            config.group_size, std::vector<std::size_t>(horizon, 0));
        std::vector<double> rewards(config.group_size, 0.0);
        double reward_sum = 0.0;

        for (std::size_t member = 0; member < config.group_size; ++member) {
            for (std::size_t turn = 0; turn < horizon; ++turn) {
                trajectories[member][turn] = policy.sample(turn, uniform());
            }
            rewards[member] = reward(trajectories[member]);
            reward_sum += rewards[member];
        }

        const std::vector<double> advantages = group_advantages(rewards);

        // Trajectory-level credit: every turn's policy gets a batch of that turn's
        // sampled action paired with the whole trajectory's advantage.
        for (std::size_t turn = 0; turn < horizon; ++turn) {
            std::vector<PolicySample> batch(config.group_size);
            for (std::size_t member = 0; member < config.group_size; ++member) {
                batch[member].action = trajectories[member][turn];
                batch[member].advantage = advantages[member];
            }
            policy_gradient_step(policy.at(turn), batch, config.learning_rate);
        }

        report.mean_reward_per_step.push_back(
            config.group_size > 0
                ? reward_sum / static_cast<double>(config.group_size)
                : 0.0);
    }

    return report;
}

}  // namespace zetta::agent
