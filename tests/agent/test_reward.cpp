#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/policy.hpp"
#include "zetta/agent/reward.hpp"
#include "zetta/agent/rollout.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/task.hpp"
#include "zetta/agent/tool_call.hpp"

namespace {

using zetta::agent::AgentPolicy;
using zetta::agent::EndState;
using zetta::agent::PolicyDecision;
using zetta::agent::Rollout;
using zetta::agent::RolloutReward;
using zetta::agent::RolloutStep;
using zetta::agent::reward_for_rollout;
using zetta::agent::RubricTerm;
using zetta::agent::rubric_reward;
using zetta::agent::run_rollout;
using zetta::agent::Sandbox;
using zetta::agent::Task;
using zetta::agent::ToolCall;
using zetta::agent::ToolResult;

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

const RubricTerm* find_term(const RolloutReward& reward, const std::string& name) {
    for (const RubricTerm& term : reward.terms) {
        if (term.name == name) return &term;
    }
    return nullptr;
}

// --- the renormalization math, tested directly -------------------------------

TEST(RubricReward, WeightedAverageOfActiveTerms) {
    EXPECT_DOUBLE_EQ(rubric_reward({RubricTerm{"a", 1.0, 1.0, true},
                                    RubricTerm{"b", 1.0, 0.0, true}}),
                     0.5);
}

TEST(RubricReward, InactiveTermsDropOutOfRenormalization) {
    // b carries most of the weight but is inactive -> reward is a's score alone.
    EXPECT_DOUBLE_EQ(rubric_reward({RubricTerm{"a", 1.0, 1.0, true},
                                    RubricTerm{"b", 3.0, 0.0, false}}),
                     1.0);
}

TEST(RubricReward, NoActiveTermScoresZero) {
    EXPECT_DOUBLE_EQ(rubric_reward({RubricTerm{"a", 1.0, 1.0, false}}), 0.0);
}

// --- reward over a real rollout ----------------------------------------------

TEST(Reward, GoodRolloutScoresHighAndSolved) {
    Task task = make_ticket_task();
    Sandbox sandbox = make_ticket_sandbox();
    ScriptedPolicy policy(
        {PolicyDecision::act(
             ToolCall{"assign_ticket", nlohmann::json{{"assignee", "aria"}}}),
         PolicyDecision::act(ToolCall{"close_ticket", nlohmann::json::object()}),
         PolicyDecision::done()});
    const Rollout rollout = run_rollout(policy, task, sandbox, 8);

    const RolloutReward reward = reward_for_rollout(task, rollout);
    EXPECT_TRUE(reward.solved);
    EXPECT_GT(reward.reward, 0.9);  // all three terms at 1.0
    EXPECT_DOUBLE_EQ(find_term(reward, "end_state")->score, 1.0);
    EXPECT_DOUBLE_EQ(find_term(reward, "right_tools")->score, 1.0);
    EXPECT_DOUBLE_EQ(find_term(reward, "right_args")->score, 1.0);
}

TEST(Reward, HallucinatedToolScoresLowAndUnsolved) {
    Task task = make_ticket_task();
    Sandbox sandbox = make_ticket_sandbox();
    ScriptedPolicy policy(
        {PolicyDecision::act(ToolCall{"delete_everything", nlohmann::json::object()}),
         PolicyDecision::done()});
    const Rollout rollout = run_rollout(policy, task, sandbox, 8);

    const RolloutReward reward = reward_for_rollout(task, rollout);
    EXPECT_FALSE(reward.solved);
    EXPECT_LT(reward.reward, 0.5);
    // the unknown tool tanks right_tools; right_args never became active (no known call)
    EXPECT_DOUBLE_EQ(find_term(reward, "right_tools")->score, 0.0);
    EXPECT_FALSE(find_term(reward, "right_args")->active);
}

TEST(Reward, NoCallsRenormalizesToEndStateAlone) {
    // A task already satisfied at the start: a policy that stops immediately makes
    // no calls, so right_tools / right_args are inactive and the reward is exactly
    // the end-state score.
    EndState satisfied;
    satisfied.set("ticket.status", "closed");
    Task task{"already-closed", "The ticket is already closed.", satisfied, {},
              satisfied};
    Sandbox sandbox{satisfied};
    ScriptedPolicy policy({PolicyDecision::done()});
    const Rollout rollout = run_rollout(policy, task, sandbox, 8);

    const RolloutReward reward = reward_for_rollout(task, rollout);
    EXPECT_TRUE(reward.solved);
    EXPECT_DOUBLE_EQ(reward.reward, 1.0);
    EXPECT_TRUE(find_term(reward, "end_state")->active);
    EXPECT_FALSE(find_term(reward, "right_tools")->active);
    EXPECT_FALSE(find_term(reward, "right_args")->active);
}

}  // namespace
