#include "zetta/agent/softmax_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zetta::agent {

std::vector<double> SoftmaxPolicy::probabilities() const {
    std::vector<double> probs(logits_.size(), 0.0);
    if (logits_.empty()) {
        return probs;
    }
    const double max_logit = *std::max_element(logits_.begin(), logits_.end());
    double sum = 0.0;
    for (std::size_t i = 0; i < logits_.size(); ++i) {
        probs[i] = std::exp(logits_[i] - max_logit);
        sum += probs[i];
    }
    for (double& probability : probs) {
        probability /= sum;
    }
    return probs;
}

std::size_t SoftmaxPolicy::sample(double uniform_draw) const {
    const std::vector<double> probs = probabilities();
    double cumulative = 0.0;
    for (std::size_t i = 0; i < probs.size(); ++i) {
        cumulative += probs[i];
        if (uniform_draw < cumulative) {
            return i;
        }
    }
    // Rounding can leave uniform_draw just past the final cumulative sum; fall back
    // to the last action rather than run off the end.
    return probs.empty() ? 0 : probs.size() - 1;
}

double SoftmaxPolicy::log_prob(std::size_t action) const {
    const std::vector<double> probs = probabilities();
    if (action >= probs.size()) {
        return -std::numeric_limits<double>::infinity();
    }
    return std::log(probs[action]);
}

}  // namespace zetta::agent
