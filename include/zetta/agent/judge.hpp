// SPDX-License-Identifier: MIT

#pragma once

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/grader.hpp"
#include "zetta/agent/task.hpp"

// The LLM-judge fallback (#555; docs/agent/README.md). Most tasks are graded
// deterministically (EndStateGrader over required facts) — a check that runs. But
// some tasks have no deterministic check; for those an LLM judge reads the task and
// the observed end-state and scores it. The judge is an INJECTED interface, so the
// fallback logic is tested with a deterministic fake and no model; a real judge (a
// local model by default, a frontier judge only on #555's Orama-infra runs)
// implements it later. The rule: the deterministic grader WINS wherever it applies;
// the judge fires ONLY where there is no deterministic criterion — and which path
// produced a grade is recorded (feat-12 step 4: a base whose score is mostly judged
// is a softer claim).
namespace zetta::agent {

// How a grade was produced.
enum class GradeSource {
    Deterministic,  // the task's required-facts grader
    Judged,         // the LLM-judge fallback
};

// The LLM judge: reads the task and an observed end-state, returns a Grade.
class Judge {
public:
    virtual ~Judge() = default;
    virtual Grade judge(const Task& task, const EndState& observed) const = 0;
};

// A grade plus how it was produced.
struct ResolvedGrade {
    Grade grade;
    GradeSource source = GradeSource::Deterministic;
};

// Resolve a task's grade for an observed end-state: use the deterministic grader
// when the task has a verifiable criterion, otherwise fall back to `judge`. `judge`
// may be null — then a task with no deterministic criterion cannot be graded and
// resolves to a failing Deterministic grade (score 0, not passed), never a silent
// pass. The deterministic grader always wins where it applies; the judge never
// overrides it.
ResolvedGrade resolve_grade(const Task& task, const EndState& observed,
                            const Judge* judge);

}  // namespace zetta::agent
