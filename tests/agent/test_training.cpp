// SPDX-License-Identifier: MIT

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
#include "zetta/agent/softmax_policy.hpp"
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
using zetta::agent::SoftmaxPolicy;
using zetta::agent::Task;
using zetta::agent::ToolCall;
using zetta::agent::ToolResult;
using zetta::agent::train_softmax_policy;
using zetta::agent::TrainingConfig;
using zetta::agent::TrainingReport;

TEST(Training, LearningCurveRisesOnABandit) {
    const std::size_t arms = 4;
    const std::size_t target = 1;
    SoftmaxPolicy policy(arms);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    const TrainingReport report = train_softmax_policy(
        policy,
        [target](std::size_t action) { return action == target ? 1.0 : 0.0; },
        [&] { return dist(rng); }, TrainingConfig{8, 150, 0.5});

    EXPECT_TRUE(report.improved());
    EXPECT_GT(report.last_reward(), 0.8);
    EXPECT_GT(policy.probabilities()[target], 0.8);
}

// The full integration: train the policy against the REAL task pipeline. The three
// candidate actions map to tool calls; each is run as a single-turn rollout through
// the sandbox and scored by reward_for_rollout. close_ticket solves the task
// (reward ~1), assign leaves it open (partial), the unknown tool scores ~0 — so the
// loop must learn to pick close_ticket. This exercises policy -> action -> ToolCall
// -> sandbox -> rollout -> reward -> advantage -> gradient, end to end.
TEST(Training, LearnsToSolveARealTask) {
    Task task{"close-ticket", "Close the ticket.", [] {
                  EndState s;
                  s.set("ticket.status", "open");
                  return s;
              }(),
              {"close_ticket", "assign_ticket"}, [] {
                  EndState s;
                  s.set("ticket.status", "closed");
                  return s;
              }()};

    const std::vector<ToolCall> actions = {
        ToolCall{"close_ticket", nlohmann::json::object()},                    // solves
        ToolCall{"assign_ticket", nlohmann::json{{"assignee", "bob"}}},        // partial
        ToolCall{"delete_everything", nlohmann::json::object()},               // unknown -> ~0
    };

    auto make_sandbox = [] {
        EndState initial;
        initial.set("ticket.status", "open");
        Sandbox sandbox{std::move(initial)};
        sandbox.register_tool(
            "close_ticket",
            [](EndState& world, const nlohmann::json&) -> ToolResult {
                world.set("ticket.status", "closed");
                return ToolResult::success();
            });
        sandbox.register_tool(
            "assign_ticket",
            [](EndState& world, const nlohmann::json& args) -> ToolResult {
                world.set("ticket.assignee", args.at("assignee"));
                return ToolResult::success();
            });
        return sandbox;
    };

    // The environment: run the chosen action as a one-turn rollout, score it.
    auto reward = [&](std::size_t action) -> double {
        Sandbox sandbox = make_sandbox();
        const ApplyOutcome outcome = sandbox.apply(actions[action]);
        Rollout rollout;
        rollout.turns.push_back(RolloutTurn{actions[action], outcome});
        rollout.final_state = sandbox.state();
        return reward_for_rollout(task, rollout).reward;
    };

    SoftmaxPolicy policy(actions.size());
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    const TrainingReport report = train_softmax_policy(
        policy, reward, [&] { return dist(rng); }, TrainingConfig{8, 150, 0.5});

    EXPECT_TRUE(report.improved());
    EXPECT_GT(report.last_reward(), 0.8);
    // close_ticket (action 0) is the solving action and must dominate.
    const std::vector<double> probs = policy.probabilities();
    EXPECT_GT(probs[0], probs[1]);
    EXPECT_GT(probs[0], probs[2]);
    EXPECT_GT(probs[0], 0.7);
}

}  // namespace
