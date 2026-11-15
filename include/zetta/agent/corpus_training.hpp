// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "zetta/agent/training.hpp"  // RewardFunction, UniformSource, TrainingConfig

// Curriculum training over a task corpus, with the per-task observability feat-12
// step 5 asks for — you can see which games are attempted and how they score
// (#555; docs/agent/README.md). Each task is its own environment (its action count
// and reward); train_over_corpus trains a policy per task and reports each task's
// reward before and after, and whether it improved. The report is the surface a
// training run watches to know the base is getting better, and where it is not.
namespace zetta::agent {

// One task's training environment: a label, its action count, and its reward.
struct TaskEnvironment {
    std::string task_id;
    std::size_t action_count = 0;
    RewardFunction reward;
};

// One task's training outcome.
struct TaskTrainingResult {
    std::string task_id;
    double reward_before = 0.0;  // mean reward at the first step
    double reward_after = 0.0;   // mean reward at the last step
    bool improved = false;
};

// The corpus training report.
struct CorpusTrainingReport {
    std::vector<TaskTrainingResult> tasks;

    std::size_t improved_count() const;
    double mean_reward_after() const;  // 0.0 for an empty report
};

// Train one policy per task over the corpus, recording each task's before/after
// reward. `uniform` supplies the sampling randomness for every task's training.
CorpusTrainingReport train_over_corpus(const std::vector<TaskEnvironment>& corpus,
                                       const UniformSource& uniform,
                                       const TrainingConfig& config = {});

}  // namespace zetta::agent
