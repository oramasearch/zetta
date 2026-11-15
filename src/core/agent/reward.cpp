// SPDX-License-Identifier: MIT

#include "zetta/agent/reward.hpp"

#include <cstddef>

#include "zetta/agent/grader.hpp"
#include "zetta/agent/sandbox.hpp"  // ApplyStatus

namespace zetta::agent {

double rubric_reward(const std::vector<RubricTerm>& terms) {
    double weighted = 0.0;
    double total_weight = 0.0;
    for (const RubricTerm& term : terms) {
        if (!term.active || term.weight <= 0.0) {
            continue;
        }
        weighted += term.weight * term.score;
        total_weight += term.weight;
    }
    if (total_weight <= 0.0) {
        return 0.0;
    }
    return weighted / total_weight;
}

RolloutReward reward_for_rollout(const Task& task, const Rollout& rollout,
                                 const RubricWeights& weights) {
    // Right end-state: the deterministic grader's score over the final world.
    const Grade grade = task.grader().grade(rollout.final_state);

    // Trajectory tallies: how many calls named a real tool, and how many of those
    // succeeded.
    const std::size_t total_calls = rollout.turns.size();
    std::size_t unknown_calls = 0;
    std::size_t ok_calls = 0;
    for (const RolloutTurn& turn : rollout.turns) {
        if (turn.outcome.status == ApplyStatus::UnknownTool) {
            ++unknown_calls;
        } else if (turn.outcome.status == ApplyStatus::Ok) {
            ++ok_calls;
        }
    }
    const std::size_t known_calls = total_calls - unknown_calls;

    RolloutReward result;
    result.solved = grade.passed;
    result.terms = {
        RubricTerm{"end_state", weights.end_state, grade.score,
                   task.is_trainable()},
        RubricTerm{"right_tools", weights.right_tools,
                   total_calls > 0
                       ? static_cast<double>(known_calls) / total_calls
                       : 0.0,
                   total_calls > 0},
        RubricTerm{"right_args", weights.right_args,
                   known_calls > 0
                       ? static_cast<double>(ok_calls) / known_calls
                       : 0.0,
                   known_calls > 0},
    };
    result.reward = rubric_reward(result.terms);
    return result;
}

}  // namespace zetta::agent
