#include "zetta/agent/advantage.hpp"

#include <cmath>
#include <cstddef>

namespace zetta::agent {

GroupStats group_stats(const std::vector<double>& rewards) {
    GroupStats stats;
    if (rewards.empty()) {
        return stats;
    }
    double sum = 0.0;
    for (double reward : rewards) {
        sum += reward;
    }
    stats.mean = sum / static_cast<double>(rewards.size());

    double squared_deviation = 0.0;
    for (double reward : rewards) {
        const double deviation = reward - stats.mean;
        squared_deviation += deviation * deviation;
    }
    stats.stddev = std::sqrt(squared_deviation / static_cast<double>(rewards.size()));
    return stats;
}

std::vector<double> group_advantages(const std::vector<double>& rewards,
                                     double epsilon) {
    const GroupStats stats = group_stats(rewards);
    std::vector<double> advantages(rewards.size(), 0.0);
    for (std::size_t i = 0; i < rewards.size(); ++i) {
        advantages[i] = (rewards[i] - stats.mean) / (stats.stddev + epsilon);
    }
    return advantages;
}

}  // namespace zetta::agent
