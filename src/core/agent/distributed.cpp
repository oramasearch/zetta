#include "zetta/agent/distributed.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace zetta::agent {

std::vector<double> identity_allreduce(const std::string& /*tag*/,
                                       const std::vector<double>& local,
                                       Reduce /*op*/) {
    return local;
}

std::vector<double> local_policy_gradient(
    const SoftmaxPolicy& policy, const std::vector<PolicySample>& batch) {
    const std::vector<double> probabilities = policy.probabilities();
    std::vector<double> gradient(probabilities.size(), 0.0);
    for (const PolicySample& sample : batch) {
        if (sample.action >= gradient.size()) {
            continue;
        }
        for (std::size_t j = 0; j < gradient.size(); ++j) {
            const double indicator = (j == sample.action) ? 1.0 : 0.0;
            gradient[j] += sample.advantage * (indicator - probabilities[j]);
        }
    }
    return gradient;
}

std::vector<double> distributed_group_advantages(
    const std::vector<double>& local_rewards, const AllReduce& allreduce,
    const std::string& tag, double epsilon) {
    // Local partials: sum, sum of squares, count. One allreduce-Sum makes them
    // global, so every node computes the same group mean and spread.
    double local_sum = 0.0;
    double local_sum_of_squares = 0.0;
    for (double reward : local_rewards) {
        local_sum += reward;
        local_sum_of_squares += reward * reward;
    }
    const std::vector<double> global = allreduce(
        tag,
        {local_sum, local_sum_of_squares,
         static_cast<double>(local_rewards.size())},
        Reduce::Sum);
    if (global.size() != 3) {
        return {};  // collective failure
    }

    const double count = global[2];
    std::vector<double> advantages(local_rewards.size(), 0.0);
    if (count <= 0.0) {
        return advantages;
    }
    const double mean = global[0] / count;
    const double variance = std::max(0.0, global[1] / count - mean * mean);
    const double stddev = std::sqrt(variance);
    for (std::size_t i = 0; i < local_rewards.size(); ++i) {
        advantages[i] = (local_rewards[i] - mean) / (stddev + epsilon);
    }
    return advantages;
}

bool distributed_policy_gradient_step(
    SoftmaxPolicy& policy, const std::vector<PolicySample>& local_batch,
    const AllReduce& allreduce, double learning_rate, const std::string& tag) {
    const std::vector<double> local_gradient =
        local_policy_gradient(policy, local_batch);
    const std::vector<double> global_gradient =
        allreduce(tag, local_gradient, Reduce::Sum);
    if (global_gradient.size() != policy.logits().size()) {
        return false;  // collective failure; leave the policy unchanged
    }
    std::vector<double>& logits = policy.logits();
    for (std::size_t j = 0; j < logits.size(); ++j) {
        logits[j] += learning_rate * global_gradient[j];
    }
    return true;
}

}  // namespace zetta::agent
