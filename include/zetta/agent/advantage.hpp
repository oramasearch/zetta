// SPDX-License-Identifier: MIT

#pragma once

#include <vector>

// GRPO-style group-relative advantages (#555; docs/agent/README.md). Given the
// rewards of a GROUP of rollouts on the same task, the advantage of each is how
// much better than the group it did, normalized by the group's spread:
//   A_i = (r_i - mean) / (stddev + epsilon)
// This is the RLVR training signal — no learned value function, just the group as
// its own baseline. Rewards that all agree give a zero advantage (no signal), so a
// task the policy already solves (or always fails) does not push the weights.
namespace zetta::agent {

// The mean and (population) standard deviation of a reward group — the baseline
// GRPO normalizes against. Exposed so a training run can log the group's spread.
struct GroupStats {
    double mean = 0.0;
    double stddev = 0.0;
};

GroupStats group_stats(const std::vector<double>& rewards);

// Group-relative advantages: (r_i - mean) / (stddev + epsilon). Empty in -> empty
// out; a group with zero spread yields all-zero advantages.
std::vector<double> group_advantages(const std::vector<double>& rewards,
                                     double epsilon = 1e-8);

}  // namespace zetta::agent
