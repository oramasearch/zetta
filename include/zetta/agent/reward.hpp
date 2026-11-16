// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#include "zetta/agent/rollout.hpp"
#include "zetta/agent/task.hpp"

// The RLVR reward over a whole rollout (#555; docs/agent/README.md). The
// deterministic end-state grader (pass/fail) is the anchor, but the training
// signal is a multi-term rubric in the shape blender-rl uses — right tool, right
// args, right end-state — combined by an ACTIVE-WEIGHT-RENORMALIZED weighted
// average: only the terms a rollout actually exercises count, and their weights
// renormalize so the reward stays in [0,1] no matter which terms apply. The
// per-term breakdown is returned so a training run can see *why* a rollout scored
// what it did (feat-12 step 5).
namespace zetta::agent {

// One rubric term's contribution: its base weight, the [0,1] score the rollout
// earned on it, and whether the rollout exercised it at all.
struct RubricTerm {
    std::string name;
    double weight = 0.0;  // base weight (>= 0); renormalized over active terms
    double score = 0.0;   // [0,1] earned on this term
    bool active = false;  // did the rollout exercise this term?
};

// Combine terms into a reward in [0,1]: the weighted average of the ACTIVE terms'
// scores, with the active weights renormalized to sum to 1. No active term (or all
// zero-weight) -> 0, because nothing was measured.
double rubric_reward(const std::vector<RubricTerm>& terms);

// The base weights of the three rubric terms (renormalized per rollout over the
// active ones). Defaults weight the end-state most; tool/arg discipline shape the
// rest.
struct RubricWeights {
    double end_state = 0.6;
    double right_tools = 0.2;
    double right_args = 0.2;
};

// The reward for one rollout, plus the breakdown that produced it.
struct RolloutReward {
    double reward = 0.0;            // renormalized rubric reward, [0,1]
    bool solved = false;           // final end-state fully passed the grader
    std::vector<RubricTerm> terms;  // end_state / right_tools / right_args
};

// Score a rollout against a task: build the three rubric terms from the final
// end-state (the grader) and the trajectory — which calls named a real tool ->
// right tools; which of those succeeded -> right args — then combine by
// rubric_reward. Terms the rollout did not exercise (e.g. it made no calls) are
// inactive and drop out of the renormalization.
RolloutReward reward_for_rollout(const Task& task, const Rollout& rollout,
                                 const RubricWeights& weights = {});

}  // namespace zetta::agent
