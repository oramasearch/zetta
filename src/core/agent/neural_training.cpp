#include "zetta/agent/neural_training.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "zetta/agent/softmax_policy.hpp"

namespace zetta::agent {

runtime::task<TrainingReport> train_linear_policy_distributed(
    LinearPolicy& policy, compute::ComputeDevice& device,
    const ContextSource& context, const ContextualRewardTask& reward,
    const UniformSource& uniform, const AllReduce& allreduce,
    const TrainingConfig& config) {
    TrainingReport report;
    report.mean_reward_per_step.reserve(config.steps);
    const std::size_t action_count = policy.action_count();
    const std::size_t feature_count = policy.feature_count();

    for (std::size_t step = 0; step < config.steps; ++step) {
        // Sample this node's local group: a context each, the forward pass W*x on
        // the device, then sample an action from the resulting distribution.
        std::vector<std::vector<float>> contexts(config.group_size);
        std::vector<std::size_t> actions(config.group_size, 0);
        std::vector<std::vector<double>> distributions(config.group_size);
        std::vector<runtime::io_task<double>> rollouts;
        rollouts.reserve(config.group_size);
        bool device_ok = true;
        for (std::size_t i = 0; i < config.group_size; ++i) {
            contexts[i] = context();
            const std::vector<float> logits = policy.logits(device, contexts[i]);
            if (logits.size() != action_count) {
                device_ok = false;
                break;
            }
            const SoftmaxPolicy distribution(
                std::vector<double>(logits.begin(), logits.end()));
            actions[i] = distribution.sample(uniform());
            distributions[i] = distribution.probabilities();
            rollouts.push_back(reward(contexts[i], actions[i]));
        }
        if (!device_ok) {
            break;
        }
        auto [error, rewards] = co_await runtime::when_all(std::move(rollouts));
        if (error) {
            break;
        }

        const std::string suffix = "-" + std::to_string(step);
        const std::vector<double> advantages =
            distributed_group_advantages(rewards, allreduce, "lin-adv" + suffix);
        if (advantages.size() != rewards.size()) {
            break;
        }

        // Accumulate the weight gradient: dW[k][f] += A*(1[k==a]-pi_k)*x[f].
        std::vector<double> gradient(action_count * feature_count, 0.0);
        for (std::size_t i = 0; i < config.group_size; ++i) {
            for (std::size_t k = 0; k < action_count; ++k) {
                const double indicator = (k == actions[i]) ? 1.0 : 0.0;
                const double signal =
                    advantages[i] * (indicator - distributions[i][k]);
                for (std::size_t f = 0; f < feature_count; ++f) {
                    gradient[k * feature_count + f] += signal * contexts[i][f];
                }
            }
        }
        const std::vector<double> global_gradient =
            allreduce("lin-grad" + suffix, gradient, Reduce::Sum);
        if (global_gradient.size() != gradient.size()) {
            break;
        }
        std::vector<float>& weights = policy.weights();
        for (std::size_t j = 0; j < weights.size(); ++j) {
            weights[j] += static_cast<float>(config.learning_rate * global_gradient[j]);
        }

        // The global mean reward this step.
        double local_sum = 0.0;
        for (double value : rewards) {
            local_sum += value;
        }
        const std::vector<double> mean_partials = allreduce(
            "lin-mean" + suffix,
            {local_sum, static_cast<double>(rewards.size())}, Reduce::Sum);
        const double mean = (mean_partials.size() == 2 && mean_partials[1] > 0.0)
                                ? mean_partials[0] / mean_partials[1]
                                : 0.0;
        report.mean_reward_per_step.push_back(mean);
    }

    co_return report;
}

}  // namespace zetta::agent
