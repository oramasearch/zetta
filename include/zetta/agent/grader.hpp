#pragma once

#include <cstddef>
#include <utility>

#include "zetta/agent/end_state.hpp"

// The RLVR reward primitive (#555): score an agent rollout's observed end-state
// against a task's required facts, deterministically — no learned reward model and
// no LLM judgement (blender-rl's rubric shape). This is the piece #555 (base
// production) and #853 (the per-customer loop) both build on. See
// docs/agent/README.md.
namespace zetta::agent {

// The outcome of grading one rollout. `score` is the RLVR reward and is ALWAYS in
// [0,1] — a reward outside that range would corrupt training — and `passed` is
// whether the task's success criterion was fully met.
struct Grade {
    double score = 0.0;   // fraction of the task's required facts satisfied, [0,1]
    bool passed = false;  // every required fact satisfied (with at least one required)
};

// A deterministic grader: it holds the task's REQUIRED end-state (the facts a
// correct agent must produce) and scores an observed end-state by exact fact
// match. A task with no required facts grades nothing — score 0, not passed — so a
// criterion-free "task" cannot post a passing number (the negative-control
// discipline: a grader that catches nothing is measuring nothing). Where a task
// has no deterministic check, an LLM-judge fallback grader is a later slice behind
// this same Grade result.
class EndStateGrader {
public:
    explicit EndStateGrader(EndState required) : required_(std::move(required)) {}

    // Score `observed` against the required facts.
    Grade grade(const EndState& observed) const;

    const EndState& required() const noexcept { return required_; }

private:
    EndState required_;
};

}  // namespace zetta::agent
