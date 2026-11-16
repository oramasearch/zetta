// SPDX-License-Identifier: MIT

#include "zetta/agent/policy_gradient.hpp"

#include <cstddef>
#include <vector>

namespace zetta::agent {

void policy_gradient_step(SoftmaxPolicy& policy,
                          const std::vector<PolicySample>& batch,
                          double learning_rate) {
    if (batch.empty()) {
        return;
    }
    // The gradient is taken at the policy that generated the samples, so compute
    // the probabilities once, before applying the update.
    const std::vector<double> probabilities = policy.probabilities();
    std::vector<double>& logits = policy.logits();
    std::vector<double> gradient(logits.size(), 0.0);

    for (const PolicySample& sample : batch) {
        if (sample.action >= logits.size()) {
            continue;
        }
        for (std::size_t j = 0; j < logits.size(); ++j) {
            const double indicator = (j == sample.action) ? 1.0 : 0.0;
            gradient[j] += sample.advantage * (indicator - probabilities[j]);
        }
    }

    for (std::size_t j = 0; j < logits.size(); ++j) {
        logits[j] += learning_rate * gradient[j];
    }
}

}  // namespace zetta::agent
