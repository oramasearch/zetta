#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/reward.hpp"
#include "zetta/agent/rollout.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/sequence_policy.hpp"
#include "zetta/agent/sequence_training.hpp"
#include "zetta/agent/task.hpp"
#include "zetta/agent/tool_call.hpp"
#include "zetta/agent/training.hpp"

namespace {

using zetta::agent::ApplyOutcome;
using zetta::agent::EndState;
using zetta::agent::Rollout;
using zetta::agent::RolloutTurn;
using zetta::agent::reward_for_rollout;
using zetta::agent::Sandbox;
using zetta::agent::SequencePolicy;
using zetta::agent::Task;
using zetta::agent::ToolCall;
using zetta::agent::ToolResult;
using zetta::agent::train_sequence_policy;
using zetta::agent::TrainingConfig;
using zetta::agent::TrainingReport;

// The policy must learn the exact ordered sequence [0, 1] out of a 2x2 space.
TEST(SequenceTraining, LearnsAnOrderedSequence) {
    SequencePolicy policy(/*horizon=*/2, /*action_count=*/2);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    const TrainingReport report = train_sequence_policy(
        policy,
        [](const std::vector<std::size_t>& actions) {
            return (actions[0] == 0 && actions[1] == 1) ? 1.0 : 0.0;
        },
        [&] { return dist(rng); }, TrainingConfig{16, 200, 0.5});

    EXPECT_TRUE(report.improved());
    EXPECT_GT(report.last_reward(), 0.8);
    EXPECT_GT(policy.at(0).probabilities()[0], 0.8);  // turn 0 -> action 0
    EXPECT_GT(policy.at(1).probabilities()[1], 0.8);  // turn 1 -> action 1
}

// Multi-turn integration: an order-enforcing sandbox — close_ticket fails on an
// unassigned ticket — so the unique solving trajectory is [assign, close]. The
// policy must learn to assign first, then close.
TEST(SequenceTraining, LearnsOrderedToolUseOnARealTask) {
    EndState required;
    required.set("ticket.status", "closed");
    required.set("ticket.assignee", "aria");
    Task task{"assign-then-close", "Assign the ticket to aria, then close it.",
              [] {
                  EndState s;
                  s.set("ticket.status", "open");
                  return s;
              }(),
              {"assign_ticket", "close_ticket"}, std::move(required)};

    const std::vector<ToolCall> actions = {
        ToolCall{"assign_ticket", nlohmann::json::object()},  // action 0
        ToolCall{"close_ticket", nlohmann::json::object()},   // action 1
    };

    auto make_sandbox = [] {
        EndState initial;
        initial.set("ticket.status", "open");
        Sandbox sandbox{std::move(initial)};
        sandbox.register_tool(
            "assign_ticket",
            [](EndState& world, const nlohmann::json&) -> ToolResult {
                world.set("ticket.assignee", "aria");
                return ToolResult::success();
            });
        sandbox.register_tool(
            "close_ticket",
            [](EndState& world, const nlohmann::json&) -> ToolResult {
                if (!world.has("ticket.assignee")) {
                    return ToolResult::failure("cannot close an unassigned ticket");
                }
                world.set("ticket.status", "closed");
                return ToolResult::success();
            });
        return sandbox;
    };

    auto reward = [&](const std::vector<std::size_t>& chosen) -> double {
        Sandbox sandbox = make_sandbox();
        Rollout rollout;
        for (std::size_t action : chosen) {
            const ApplyOutcome outcome = sandbox.apply(actions[action]);
            rollout.turns.push_back(RolloutTurn{actions[action], outcome});
        }
        rollout.final_state = sandbox.state();
        return reward_for_rollout(task, rollout).reward;
    };

    SequencePolicy policy(/*horizon=*/2, actions.size());
    std::mt19937 rng(21);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    const TrainingReport report = train_sequence_policy(
        policy, reward, [&] { return dist(rng); }, TrainingConfig{16, 200, 0.5});

    EXPECT_TRUE(report.improved());
    EXPECT_GT(report.last_reward(), 0.85);
    EXPECT_GT(policy.at(0).probabilities()[0], 0.7);  // assign first
    EXPECT_GT(policy.at(1).probabilities()[1], 0.7);  // then close
}

}  // namespace
