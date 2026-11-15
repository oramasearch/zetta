// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "zetta/agent/sequence_policy.hpp"
#include "zetta/agent/training.hpp"  // TrainingConfig, TrainingReport, UniformSource

// Trajectory-level RLVR training for a multi-turn SequencePolicy (#555;
// docs/agent/README.md). Each step samples a GROUP of trajectories (a full
// horizon-length action sequence each), turns their rewards into GRPO advantages,
// and applies each trajectory's advantage to EVERY turn's policy — trajectory-level
// credit assignment (REINFORCE over the whole sequence). The same shape as the
// single-action loop, now over sequences, so a real multi-turn task (ordered tool
// use) can be learned.
namespace zetta::agent {

// The reward a full trajectory (its horizon sampled action indices) earns.
using TrajectoryReward =
    std::function<double(const std::vector<std::size_t>& actions)>;

TrainingReport train_sequence_policy(SequencePolicy& policy,
                                     const TrajectoryReward& reward,
                                     const UniformSource& uniform,
                                     const TrainingConfig& config = {});

}  // namespace zetta::agent
