#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "zetta/agent/softmax_policy.hpp"

// The RLVR training loop (#555; docs/agent/README.md). It repeats the reference
// step — sample a group of actions from the policy, turn their rewards into GRPO
// advantages, take a policy-gradient step — over many iterations, and reports the
// mean reward per step so improvement is visible (feat-12 step 5). The environment
// (what an action is worth) and the randomness are INJECTED, so the SAME loop
// trains the CPU reference policy here and, later, a real model behind AgentPolicy
// — only the reward function and the sampling source change.
namespace zetta::agent {

// The reward an action earns from the environment (task + sandbox + grader).
using RewardFunction = std::function<double(std::size_t action)>;

// A source of uniform draws in [0,1) for sampling — injected so training is
// deterministic in tests (seed a generator) and real in production.
using UniformSource = std::function<double()>;

struct TrainingConfig {
    std::size_t group_size = 8;  // rollouts sampled per step (the GRPO group)
    std::size_t steps = 100;     // gradient steps
    double learning_rate = 0.5;
};

struct TrainingReport {
    std::vector<double> mean_reward_per_step;  // the learning curve

    double first_reward() const {
        return mean_reward_per_step.empty() ? 0.0 : mean_reward_per_step.front();
    }
    double last_reward() const {
        return mean_reward_per_step.empty() ? 0.0 : mean_reward_per_step.back();
    }
    bool improved() const { return last_reward() > first_reward(); }
};

// Train `policy` in place: each step samples `group_size` actions via `uniform`,
// scores them with `reward`, forms GRPO advantages, and takes a gradient step.
// Returns the per-step mean reward — the learning curve.
TrainingReport train_softmax_policy(SoftmaxPolicy& policy,
                                    const RewardFunction& reward,
                                    const UniformSource& uniform,
                                    const TrainingConfig& config = {});

}  // namespace zetta::agent
