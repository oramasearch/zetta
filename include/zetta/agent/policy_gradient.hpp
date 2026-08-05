#pragma once

#include <cstddef>
#include <vector>

#include "zetta/agent/softmax_policy.hpp"

// The RLVR policy-gradient step (#555; docs/agent/README.md). Given a batch of
// (sampled action, its group-relative advantage), nudge the policy's logits so
// actions with positive advantage become more likely and those with negative
// advantage less likely — REINFORCE with the GRPO group baseline. The gradient of
// log pi(a) w.r.t. logit_j is (1[j==a] - pi_j), so the update is
//   logit_j += learning_rate * sum_samples advantage * (1[j==a] - pi_j).
// This is the single step the training loop repeats; run it enough times on a task
// and the reward rises — the whole point of the loop, proven here on the CPU
// reference policy before a real model is swapped in behind AgentPolicy.
namespace zetta::agent {

// One sampled action and the advantage it earned within its group.
struct PolicySample {
    std::size_t action = 0;
    double advantage = 0.0;
};

// One REINFORCE/GRPO ascent step on `policy`'s logits from `batch`, scaled by
// `learning_rate`. An empty batch is a no-op; an out-of-range action is skipped.
void policy_gradient_step(SoftmaxPolicy& policy,
                          const std::vector<PolicySample>& batch,
                          double learning_rate);

}  // namespace zetta::agent
