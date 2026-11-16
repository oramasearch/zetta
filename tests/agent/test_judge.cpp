// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <utility>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/grader.hpp"
#include "zetta/agent/judge.hpp"
#include "zetta/agent/task.hpp"

namespace {

using zetta::agent::EndState;
using zetta::agent::Grade;
using zetta::agent::GradeSource;
using zetta::agent::Judge;
using zetta::agent::ResolvedGrade;
using zetta::agent::resolve_grade;
using zetta::agent::Task;

// A deterministic fake judge that always returns a configured grade — stands in for
// the real LLM judge so the fallback logic is tested with no model.
class FakeJudge final : public Judge {
public:
    explicit FakeJudge(Grade grade) : grade_(grade) {}
    Grade judge(const Task&, const EndState&) const override { return grade_; }

private:
    Grade grade_;
};

Task checkable_task() {
    EndState required;
    required.set("ticket.status", "closed");
    return Task{"close", "Close it.", EndState{}, {"close_ticket"},
                std::move(required)};
}

// A task with no required facts -> no deterministic criterion -> judge territory.
Task open_ended_task() {
    return Task{"summarize", "Write a good summary.", EndState{}, {"write"},
                EndState{}};
}

TEST(Judge, DeterministicWhenTaskHasCriterion) {
    const FakeJudge always_pass(Grade{1.0, true});  // would pass if consulted
    EndState observed;
    observed.set("ticket.status", "open");  // does NOT satisfy the criterion

    const ResolvedGrade resolved =
        resolve_grade(checkable_task(), observed, &always_pass);
    EXPECT_EQ(resolved.source, GradeSource::Deterministic);
    EXPECT_FALSE(resolved.grade.passed);  // deterministic grader wins, judge ignored
}

TEST(Judge, JudgeFiresWhenNoDeterministicCriterion) {
    const FakeJudge judge(Grade{0.75, true});
    const ResolvedGrade resolved =
        resolve_grade(open_ended_task(), EndState{}, &judge);
    EXPECT_EQ(resolved.source, GradeSource::Judged);
    EXPECT_DOUBLE_EQ(resolved.grade.score, 0.75);
    EXPECT_TRUE(resolved.grade.passed);
}

TEST(Judge, NoCriterionAndNoJudgeFailsClosed) {
    const ResolvedGrade resolved =
        resolve_grade(open_ended_task(), EndState{}, nullptr);
    EXPECT_EQ(resolved.source, GradeSource::Deterministic);
    EXPECT_DOUBLE_EQ(resolved.grade.score, 0.0);
    EXPECT_FALSE(resolved.grade.passed);  // never a silent pass
}

TEST(Judge, DeterministicPassStillWinsOverAFailingJudge) {
    const FakeJudge always_fail(Grade{0.0, false});  // must not be consulted
    EndState observed;
    observed.set("ticket.status", "closed");  // satisfies the criterion

    const ResolvedGrade resolved =
        resolve_grade(checkable_task(), observed, &always_fail);
    EXPECT_EQ(resolved.source, GradeSource::Deterministic);
    EXPECT_TRUE(resolved.grade.passed);
}

}  // namespace
