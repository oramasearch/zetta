// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/policy.hpp"
#include "zetta/agent/rollout.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/task.hpp"
#include "zetta/agent/tool_call.hpp"

namespace {

using zetta::agent::AgentPolicy;
using zetta::agent::ApplyStatus;
using zetta::agent::EndState;
using zetta::agent::PolicyDecision;
using zetta::agent::Rollout;
using zetta::agent::RolloutStep;
using zetta::agent::run_rollout;
using zetta::agent::Sandbox;
using zetta::agent::Task;
using zetta::agent::ToolCall;
using zetta::agent::ToolResult;

// A policy that plays a fixed script of decisions, then stops.
class ScriptedPolicy final : public AgentPolicy {
public:
    explicit ScriptedPolicy(std::vector<PolicyDecision> script)
        : script_(std::move(script)) {}
    PolicyDecision next(const RolloutStep&) override {
        if (index_ >= script_.size()) return PolicyDecision::done();
        return script_[index_++];
    }

private:
    std::vector<PolicyDecision> script_;
    std::size_t index_ = 0;
};

// A policy that repeats one call forever (never stops) — to test the turn budget.
class LoopingPolicy final : public AgentPolicy {
public:
    explicit LoopingPolicy(ToolCall call) : call_(std::move(call)) {}
    PolicyDecision next(const RolloutStep&) override {
        return PolicyDecision::act(call_);
    }

private:
    ToolCall call_;
};

Task make_ticket_task() {
    EndState initial;
    initial.set("ticket.status", "open");
    EndState required;
    required.set("ticket.status", "closed");
    required.set("ticket.assignee", "aria");
    return Task{"close-ticket", "Close the ticket and assign it to aria.",
                std::move(initial), {"assign_ticket", "close_ticket"},
                std::move(required)};
}

Sandbox make_ticket_sandbox() {
    EndState initial;
    initial.set("ticket.status", "open");
    Sandbox sandbox{std::move(initial)};
    sandbox.register_tool(
        "assign_ticket",
        [](EndState& world, const nlohmann::json& args) -> ToolResult {
            if (!args.contains("assignee")) {
                return ToolResult::failure("assign_ticket needs an assignee");
            }
            world.set("ticket.assignee", args.at("assignee"));
            return ToolResult::success();
        });
    sandbox.register_tool(
        "close_ticket",
        [](EndState& world, const nlohmann::json&) -> ToolResult {
            world.set("ticket.status", "closed");
            return ToolResult::success();
        });
    return sandbox;
}

ToolCall assign_aria() {
    return ToolCall{"assign_ticket", nlohmann::json{{"assignee", "aria"}}};
}
ToolCall close_ticket() {
    return ToolCall{"close_ticket", nlohmann::json::object()};
}

TEST(Rollout, ScriptedCorrectPolicyReachesTheGoal) {
    Task task = make_ticket_task();
    Sandbox sandbox = make_ticket_sandbox();
    ScriptedPolicy policy({PolicyDecision::act(assign_aria()),
                           PolicyDecision::act(close_ticket()),
                           PolicyDecision::done()});

    const Rollout rollout = run_rollout(policy, task, sandbox, /*max_turns=*/8);

    EXPECT_TRUE(rollout.stopped_by_policy);
    EXPECT_EQ(rollout.turns.size(), 2u);
    EXPECT_TRUE(task.grader().grade(rollout.final_state).passed);
}

TEST(Rollout, ScriptedWrongPolicyDoesNotReachTheGoal) {
    Task task = make_ticket_task();
    Sandbox sandbox = make_ticket_sandbox();
    // Never assigns -> the required ticket.assignee fact is missing.
    ScriptedPolicy policy(
        {PolicyDecision::act(close_ticket()), PolicyDecision::done()});

    const Rollout rollout = run_rollout(policy, task, sandbox, 8);

    EXPECT_FALSE(task.grader().grade(rollout.final_state).passed);
}

TEST(Rollout, TurnBudgetBoundsANonStoppingPolicy) {
    Task task = make_ticket_task();
    Sandbox sandbox = make_ticket_sandbox();
    LoopingPolicy policy(close_ticket());  // never returns done()

    const Rollout rollout = run_rollout(policy, task, sandbox, /*max_turns=*/3);

    EXPECT_FALSE(rollout.stopped_by_policy);  // budget exhausted, not a voluntary stop
    EXPECT_EQ(rollout.turns.size(), 3u);
}

TEST(Rollout, RecordsAFailedCallInTheTrajectory) {
    Task task = make_ticket_task();
    Sandbox sandbox = make_ticket_sandbox();
    // assign with no args -> the tool fails; the trajectory records it honestly.
    ScriptedPolicy policy(
        {PolicyDecision::act(ToolCall{"assign_ticket", nlohmann::json::object()}),
         PolicyDecision::done()});

    const Rollout rollout = run_rollout(policy, task, sandbox, 8);

    ASSERT_EQ(rollout.turns.size(), 1u);
    EXPECT_EQ(rollout.turns[0].outcome.status, ApplyStatus::ToolFailed);
}

}  // namespace
