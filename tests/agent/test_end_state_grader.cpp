#include <gtest/gtest.h>

#include <utility>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/grader.hpp"

namespace {

using zetta::agent::EndState;
using zetta::agent::EndStateGrader;
using zetta::agent::Grade;

// A task's required end-state a correct agent must produce: two facts.
EndState make_required() {
    EndState required;
    required.set("ticket.status", "closed");
    required.set("ticket.assignee", "aria");
    return required;
}

TEST(EndStateGrader, ExactMatchScoresOneAndPasses) {
    const EndStateGrader grader(make_required());
    EndState observed;
    observed.set("ticket.status", "closed");
    observed.set("ticket.assignee", "aria");
    observed.set("ticket.note", "resolved");  // extra facts do not hurt

    const Grade grade = grader.grade(observed);
    EXPECT_DOUBLE_EQ(grade.score, 1.0);
    EXPECT_TRUE(grade.passed);
}

TEST(EndStateGrader, PartialMatchScoresFractionAndFails) {
    const EndStateGrader grader(make_required());
    EndState observed;
    observed.set("ticket.status", "closed");  // one of the two required
    observed.set("ticket.assignee", "someone-else");

    const Grade grade = grader.grade(observed);
    EXPECT_DOUBLE_EQ(grade.score, 0.5);
    EXPECT_FALSE(grade.passed);
}

// The negative control: a broken agent that produces nothing must score 0 and
// fail. This is what makes the reward mean something — a grader that cannot catch
// a wrong answer is measuring nothing (feat-12-base-agent test script, step 3).
TEST(EndStateGrader, EmptyObservedIsTheNegativeControl) {
    const EndStateGrader grader(make_required());
    const Grade grade = grader.grade(EndState{});
    EXPECT_DOUBLE_EQ(grade.score, 0.0);
    EXPECT_FALSE(grade.passed);
}

TEST(EndStateGrader, RightKeyWrongValueDoesNotCount) {
    EndState required;
    required.set("db.rows", 42);  // an integer fact
    const EndStateGrader grader(std::move(required));

    EndState observed;
    observed.set("db.rows", 41);
    const Grade grade = grader.grade(observed);
    EXPECT_DOUBLE_EQ(grade.score, 0.0);
    EXPECT_FALSE(grade.passed);
}

TEST(EndStateGrader, NoCriterionGradesNothing) {
    const EndStateGrader grader{EndState{}};
    EndState observed;
    observed.set("anything", "at all");
    const Grade grade = grader.grade(observed);
    EXPECT_DOUBLE_EQ(grade.score, 0.0);
    EXPECT_FALSE(grade.passed);
}

}  // namespace
