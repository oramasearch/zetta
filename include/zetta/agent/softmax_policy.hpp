// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

// A tiny parametric policy: a categorical distribution over a fixed set of actions,
// parameterized by one logit (a preference weight) per action. softmax(logits)
// gives the action probabilities; sampling is deterministic given a uniform draw so
// rollouts and tests are reproducible; log_prob feeds the policy-gradient update.
// This is the CPU reference policy the RLVR training loop is built and tested on
// (#555; docs/agent/README.md) — before a real, MLX-backed model plugs into the
// same AgentPolicy interface. It is deliberately trivial: the training machinery
// (advantages, the gradient step, the loop) is proven against it, then the model
// is swapped in without touching that machinery.
namespace zetta::agent {

class SoftmaxPolicy {
public:
    // `action_count` actions, all logits zero (a uniform distribution).
    explicit SoftmaxPolicy(std::size_t action_count)
        : logits_(action_count, 0.0) {}

    explicit SoftmaxPolicy(std::vector<double> logits)
        : logits_(std::move(logits)) {}

    std::size_t action_count() const noexcept { return logits_.size(); }

    const std::vector<double>& logits() const noexcept { return logits_; }
    std::vector<double>& logits() noexcept { return logits_; }  // for the update

    // The categorical probabilities: softmax(logits), summing to 1
    // (numerically stable — the max logit is subtracted first).
    std::vector<double> probabilities() const;

    // Sample an action index by inverse-CDF over the probabilities given a uniform
    // draw in [0,1). Deterministic in `uniform_draw`, so rollouts reproduce.
    std::size_t sample(double uniform_draw) const;

    // log pi(action) — the log-probability of a chosen action index. -inf if the
    // action is out of range.
    double log_prob(std::size_t action) const;

private:
    std::vector<double> logits_;
};

}  // namespace zetta::agent
