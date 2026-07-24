#include <gtest/gtest.h>

#include <utility>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/grader.hpp"
#include "zetta/agent/task.hpp"

namespace {

using zetta::agent::EndState;
using zetta::agent::Grade;
using zetta::agent::Task;

// A small enterprise tool-calling task: from an open ticket, drive it to closed
// and assigned. The required end-state is the readable, verifiable criterion.
Task make_close_ticket_task() {
    EndState initial;
    initial.set("ticket.status", "open");

    EndState required;
    required.set("ticket.status", "closed");
    required.set("ticket.assignee", "aria");

    return Task{"close-ticket", "Close the open ticket and assign it to aria.",
                std::move(initial), {"assign_ticket", "close_ticket"},
                std::move(required)};
}

TEST(Task, GraderCatchesBrokenEndState) {
    const Task task = make_close_ticket_task();

    EndState correct;
    correct.set("ticket.status", "closed");
    correct.set("ticket.assignee", "aria");
    EXPECT_TRUE(task.grader().grade(correct).passed);

    // A broken agent that left the ticket open must not pass.
    EndState broken;
    broken.set("ticket.status", "open");
    const Grade grade = task.grader().grade(broken);
    EXPECT_FALSE(grade.passed);
    EXPECT_LT(grade.score, 1.0);
}

TEST(Task, WithCriterionIsTrainable) {
    EXPECT_TRUE(make_close_ticket_task().is_trainable());
}

TEST(Task, WithoutVerifiableCriterionIsNotTrainable) {
    // No required facts -> nothing to verify -> cannot enter the curriculum.
    const Task task{"noop", "Do nothing checkable.", EndState{}, {}, EndState{}};
    EXPECT_FALSE(task.is_trainable());
}

TEST(Task, ExposesReadableCriterionAndStartState) {
    const Task task = make_close_ticket_task();
    EXPECT_EQ(task.id(), "close-ticket");
    EXPECT_FALSE(task.description().empty());
    // The start state and the goal are distinct: it starts open, must end closed.
    EXPECT_TRUE(task.initial_state().matches("ticket.status", "open"));
    EXPECT_TRUE(task.required().matches("ticket.status", "closed"));
    EXPECT_NE(task.initial_state(), task.required());
    EXPECT_EQ(task.tool_names().size(), 2u);
}

}  // namespace
