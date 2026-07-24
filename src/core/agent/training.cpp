#include "zetta/agent/training.hpp"

#include <cstddef>
#include <vector>

#include "zetta/agent/advantage.hpp"
#include "zetta/agent/policy_gradient.hpp"

namespace zetta::agent {

TrainingReport train_softmax_policy(SoftmaxPolicy& policy,
                                    const RewardFunction& reward,
                                    const UniformSource& uniform,
                                    const TrainingConfig& config) {
    TrainingReport report;
    report.mean_reward_per_step.reserve(config.steps);

    for (std::size_t step = 0; step < config.steps; ++step) {
        std::vector<double> rewards(config.group_size, 0.0);
        std::vector<PolicySample> batch(config.group_size);
        double reward_sum = 0.0;

        for (std::size_t i = 0; i < config.group_size; ++i) {
            const std::size_t action = policy.sample(uniform());
            const double earned = reward(action);
            batch[i].action = action;
            rewards[i] = earned;
            reward_sum += earned;
        }

        const std::vector<double> advantages = group_advantages(rewards);
        for (std::size_t i = 0; i < config.group_size; ++i) {
            batch[i].advantage = advantages[i];
        }
        policy_gradient_step(policy, batch, config.learning_rate);

        report.mean_reward_per_step.push_back(
            config.group_size > 0
                ? reward_sum / static_cast<double>(config.group_size)
                : 0.0);
    }

    return report;
}

}  // namespace zetta::agent
