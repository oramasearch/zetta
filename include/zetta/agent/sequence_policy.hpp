// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <vector>

#include "zetta/agent/softmax_policy.hpp"

// A fixed-horizon multi-turn policy: one SoftmaxPolicy per turn over a shared action
// space (#555; docs/agent/README.md). Real tasks take several tool calls, so the
// policy must emit a SEQUENCE of actions; this is the positional reference policy
// the multi-turn training loop is proven on, before a real (state-conditioned)
// model plugs in behind AgentPolicy. Each turn's distribution is independent —
// deliberately the simplest thing that can learn an ordered sequence.
namespace zetta::agent {

class SequencePolicy {
public:
    SequencePolicy(std::size_t horizon, std::size_t action_count)
        : turns_(horizon, SoftmaxPolicy(action_count)) {}

    std::size_t horizon() const noexcept { return turns_.size(); }
    std::size_t action_count() const noexcept {
        return turns_.empty() ? 0 : turns_.front().action_count();
    }

    // The per-turn policy (mutable overload for the training update).
    SoftmaxPolicy& at(std::size_t turn) { return turns_.at(turn); }
    const SoftmaxPolicy& at(std::size_t turn) const { return turns_.at(turn); }

    // Sample the action for `turn` from a uniform draw in [0,1).
    std::size_t sample(std::size_t turn, double uniform_draw) const {
        return turns_.at(turn).sample(uniform_draw);
    }

private:
    std::vector<SoftmaxPolicy> turns_;
};

}  // namespace zetta::agent
