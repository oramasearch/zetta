// Agent GOALS — the reward an agent is trained against, made data instead of code.
//
// The property under test is the one that keeps this RLVR rather than a learned preference: a goal
// resolves to DETERMINISTIC checks, a goal that resolves to none earns nothing and is reported,
// and a turn is never penalized on a goal it had no bearing on.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/agent_record.hpp"
#include "zetta/agent/goal.hpp"

namespace {

using zetta::agent::EndState;
using zetta::agent::Goal;

EndState facts(std::vector<std::pair<std::string, nlohmann::json>> entries) {
    EndState state;
    for (auto& [key, value] : entries) state.set(std::move(key), std::move(value));
    return state;
}

TEST(Goals, APlainLanguageGoalResolvesToVerifiableChecks) {
    const auto checks = zetta::agent::interpret_goal("Never state a number we cannot evidence.");
    ASSERT_FALSE(checks.empty()) << "this goal must be verifiable";
    EXPECT_NE(std::find(checks.begin(), checks.end(), "numbers_from_context"), checks.end());
}

// Every check a goal can resolve to must be one the response projection can actually decide.
// Otherwise training optimizes something nothing measures.
TEST(Goals, EveryCatalogueCheckIsNamedByTheInterpreter) {
    for (const auto& check : zetta::agent::goal_check_catalogue()) {
        EXPECT_FALSE(check.name.empty());
        EXPECT_FALSE(check.description.empty()) << check.name << " needs a readable meaning";
    }
}

// A goal nobody can verify must NOT quietly score. It is reported so the person who wrote it
// finds out, rather than believing the agent is being trained on it.
TEST(Goals, AnUnverifiableGoalEarnsNothingAndIsReported) {
    std::vector<Goal> goals = {{"Be delightful and on-brand.", 1.0, {}}};
    zetta::agent::interpret_goals(goals);
    EXPECT_TRUE(goals[0].checks.empty());
    EXPECT_EQ(zetta::agent::unmapped_goals(goals).size(), 1U);
    EXPECT_DOUBLE_EQ(zetta::agent::goal_reward(goals, facts({{"grounded", true}})), 0.0);
}

// Importance is the weight: the goal a customer cares about most moves the reward most.
TEST(Goals, ImportanceWeightsTheReward) {
    std::vector<Goal> goals = {
        {"Never state a number we cannot evidence.", 9.0, {"numbers_from_context"}},
        {"Nothing acts without approval.", 1.0, {"approval_gated"}}};

    // Fails the important goal, passes the minor one.
    const double mostly_wrong = zetta::agent::goal_reward(
        goals, facts({{"numbers_from_context", false}, {"approval_gated", true}}));
    // Passes the important goal, fails the minor one.
    const double mostly_right = zetta::agent::goal_reward(
        goals, facts({{"numbers_from_context", true}, {"approval_gated", false}}));

    EXPECT_LT(mostly_wrong, 0.2) << "failing the 9x goal must dominate";
    EXPECT_GT(mostly_right, 0.8) << "passing the 9x goal must dominate";
}

// A turn that never engaged a goal is not penalized on it. Counting an untouched goal as zero
// would push the agent to bring up approvals in a turn that asked what languages it supports.
TEST(Goals, AGoalTheTurnDidNotEngageDoesNotCountAgainstIt) {
    const std::vector<Goal> goals = {
        {"Never state a number we cannot evidence.", 1.0, {"numbers_from_context"}},
        {"Nothing acts without approval.", 1.0, {"approval_gated"}}};

    // Only the grounding goal is engaged, and it passed: full marks, not half.
    EXPECT_DOUBLE_EQ(zetta::agent::goal_reward(goals, facts({{"numbers_from_context", true}})),
                     1.0);
}

// Nothing measured is not the same as done badly.
TEST(Goals, NoGoalsEngagedScoresZeroRatherThanPassing) {
    const std::vector<Goal> goals = {{"Never fabricate.", 1.0, {"numbers_from_context"}}};
    EXPECT_DOUBLE_EQ(zetta::agent::goal_reward(goals, facts({{"kind", std::string("answer")}})),
                     0.0);
    EXPECT_DOUBLE_EQ(zetta::agent::goal_reward({}, facts({{"grounded", true}})), 0.0);
}

// A goal with several checks scores the fraction it satisfied, so partial credit is real.
TEST(Goals, AGoalScoresTheFractionOfItsChecksThatHold) {
    const std::vector<Goal> goals = {
        {"Do not leak the future.", 1.0, {"leakage_free", "point_in_time"}}};
    EXPECT_DOUBLE_EQ(
        zetta::agent::goal_reward(goals, facts({{"leakage_free", true}, {"point_in_time", false}})),
        0.5);
}

TEST(Goals, RoundTripsThroughJson) {
    const Goal goal{"Nothing acts without approval.", 0.75, {"approval_gated"}};
    std::string error;
    const auto restored = zetta::agent::goal_from_json(zetta::agent::to_json(goal), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    EXPECT_EQ(*restored, goal);
}

TEST(Goals, MalformedGoalsAreRejectedWithAReason) {
    std::string error;
    EXPECT_FALSE(zetta::agent::goal_from_json(nlohmann::json::array(), &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(zetta::agent::goal_from_json({{"statement", ""}}, &error).has_value());
    EXPECT_FALSE(
        zetta::agent::goal_from_json({{"statement", "x"}, {"importance", -1}}, &error).has_value());
}

// Goals ride on the agent record, so the reward is a property of the DEPLOYMENT rather than of the
// engine — a per-customer fork differs by its record, not by a build.
TEST(Goals, RideOnTheAgentRecordAndResolveOnTheWayIn) {
    const nlohmann::json document = {
        {"id", "a1"},
        {"name", "Lender agent"},
        {"model", "/checkpoints/base"},
        {"goals", nlohmann::json::array({
                      {{"statement", "Never state a number we cannot evidence."},
                       {"importance", 9}},
                      {{"statement", "Nothing acts without approval."}, {"importance", 3}},
                  })}};
    std::string error;
    const auto agent = zetta::agent::agent_from_json(document, &error);
    ASSERT_TRUE(agent.has_value()) << error;
    ASSERT_EQ(agent->goals.size(), 2U);
    EXPECT_DOUBLE_EQ(agent->goals[0].importance, 9.0);
    // Resolved on the way in: a stored record says what it actually trains on.
    EXPECT_FALSE(agent->goals[0].checks.empty());
    EXPECT_FALSE(agent->goals[1].checks.empty());

    // And it survives the round trip the CRDT stores it through.
    const auto restored = zetta::agent::agent_from_json(zetta::agent::to_json(*agent), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    EXPECT_EQ(restored->goals, agent->goals);
}

// An agent with no goals behaves exactly as every agent did before goals existed.
TEST(Goals, AnAgentWithoutGoalsIsUnchanged) {
    std::string error;
    const auto agent = zetta::agent::agent_from_json(
        {{"id", "a2"}, {"name", "Base"}, {"model", "/checkpoints/base"}}, &error);
    ASSERT_TRUE(agent.has_value()) << error;
    EXPECT_TRUE(agent->goals.empty());
}

}  // namespace
