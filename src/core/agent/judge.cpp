// SPDX-License-Identifier: MIT

#include "zetta/agent/judge.hpp"

namespace zetta::agent {

ResolvedGrade resolve_grade(const Task& task, const EndState& observed,
                            const Judge* judge) {
    // A verifiable criterion means a deterministic check exists — it wins, always.
    if (task.is_trainable()) {
        return ResolvedGrade{task.grader().grade(observed),
                             GradeSource::Deterministic};
    }

    // No deterministic criterion: fall back to the judge if one is available.
    if (judge != nullptr) {
        return ResolvedGrade{judge->judge(task, observed), GradeSource::Judged};
    }

    // No criterion and no judge: fail closed. An ungradeable task must never post a
    // silent pass.
    return ResolvedGrade{Grade{.score = 0.0, .passed = false},
                         GradeSource::Deterministic};
}

}  // namespace zetta::agent
