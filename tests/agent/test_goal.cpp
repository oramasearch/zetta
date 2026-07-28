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
#include "zetta/agent/sandbox.hpp"

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

// THE CONNECTION #941 is about: a raw generation scored against the agent's own goals, which is
// what `grpo_step` needs as its CompletionReward. Training now moves the policy toward what this
// deployment wants rather than toward a rubric compiled into the engine.
TEST(Goals, AGenerationIsScoredAgainstTheAgentsOwnGoals) {
    const std::vector<Goal> goals = {
        {"Never state a number we cannot evidence.", 5.0, {"numbers_from_context"}}};
    const std::string grounding = "Project: Churn\nTable: accounts\nMeasure: AUC 0.880\n";

    const std::string honest =
        R"({"kind":"answer","message":"The model scores 0.880 AUC.","tool_calls":[]})";
    const std::string invented =
        R"({"kind":"answer","message":"The model scores 0.947 AUC.","tool_calls":[]})";

    EXPECT_GT(zetta::agent::goal_reward_for_generation(goals, honest, grounding), 0.9);
    EXPECT_LT(zetta::agent::goal_reward_for_generation(goals, invented, grounding), 0.1)
        << "a figure the context does not contain must not be rewarded";
}

// A generation the harness cannot execute is worth nothing, however well it reads. Rewarding
// fluent-but-unparseable text is exactly the failure verifiable reward exists to prevent.
TEST(Goals, AnUnparseableGenerationScoresZero) {
    const std::vector<Goal> goals = {{"Never fabricate.", 1.0, {"numbers_from_context"}}};
    EXPECT_DOUBLE_EQ(
        zetta::agent::goal_reward_for_generation(goals, "Sure! I can help with that.", "ctx"), 0.0);
}

// The verifier may see what the policy did not — the asymmetry the rollout harvest relies on. A
// product answer served with NO capability listing is still scored against one.
TEST(Goals, TheVerifierMaySeeGroundingThePolicyDidNot) {
    const std::vector<Goal> goals = {
        {"Only offer capabilities that exist.", 1.0, {"capability_exists"}}};
    const std::string listing = "Capabilities:\n  train a gradient-boosted model\n  deploy to AWS\n";
    const std::string real =
        R"({"kind":"answer","message":"Studio can train a gradient-boosted model.","tool_calls":[]})";
    const std::string invented =
        R"({"kind":"answer","message":"Studio can autotune your Kubernetes spend.","tool_calls":[]})";

    EXPECT_GT(zetta::agent::goal_reward_for_generation(goals, real, listing), 0.9);
    EXPECT_DOUBLE_EQ(zetta::agent::goal_reward_for_generation(goals, invented, listing), 0.0)
        << "inventing a feature must earn nothing";
}

// Importance still drives the gradient at the generation level, not just over raw facts.
TEST(Goals, ImportanceStillWeightsAGenerationsReward) {
    const std::vector<Goal> weighted = {
        {"Never state a number we cannot evidence.", 9.0, {"numbers_from_context"}},
        {"Nothing acts without approval.", 1.0, {"approval_gated"}}};
    const std::string grounding = "Measure: AUC 0.880\n";
    const std::string fabricates =
        R"({"kind":"action","message":"AUC is 0.947.","tool_calls":[{"tool":"serve",)"
        R"("arguments":{"name":"solution","approval_gated":true}}]})";
    EXPECT_LT(zetta::agent::goal_reward_for_generation(weighted, fabricates, grounding), 0.2)
        << "failing the 9x goal must dominate passing the 1x one";
}

// --- Bite 1: what actually RAN becomes gradeable facts (#941) -----------------------------------
//
// The reward has scored facts PROJECTED from a response — what it claimed. A projection can be
// satisfied by a response that would not survive execution, which is the failure verifiable reward
// exists to prevent. These facts come from the sandbox's own verdict on each call.
TEST(Goals, ExecutionFactsReportWhetherTheCallsActuallyRan) {
    const std::vector<zetta::agent::ToolOutcome> ran = {{"connect", "ok", ""},
                                                        {"model", "ok", ""}};
    const zetta::agent::EndState good = zetta::agent::execution_facts(ran);
    EXPECT_TRUE(good.matches("tools_ran", true));
    EXPECT_TRUE(good.matches("tools_failed", false));

    // One failure is a failure. A pipeline whose model step did not run is not a pipeline.
    const std::vector<zetta::agent::ToolOutcome> broke = {
        {"connect", "ok", ""}, {"model", "failed", "no such table"}};
    const zetta::agent::EndState bad = zetta::agent::execution_facts(broke);
    EXPECT_TRUE(bad.matches("tools_ran", false));
    EXPECT_TRUE(bad.matches("tools_failed", true));

    // Calling a tool that does not exist is its own failure, and worth distinguishing: it means
    // the model invented a capability rather than misusing a real one.
    const std::vector<zetta::agent::ToolOutcome> invented = {{"autotune", "unknown tool", ""}};
    const zetta::agent::EndState made_up = zetta::agent::execution_facts(invented);
    EXPECT_TRUE(made_up.matches("tools_ran", false));
    EXPECT_TRUE(made_up.matches("called_unknown_tool", true));
}

// A turn that called nothing has nothing to say about execution. It must not read as SUCCESS —
// an answer that ran no tools would otherwise score full marks on a goal about tools running.
TEST(Goals, ATurnThatCalledNothingProjectsNoExecutionFacts) {
    const zetta::agent::EndState none = zetta::agent::execution_facts({});
    EXPECT_FALSE(none.matches("tools_ran", true));
    EXPECT_FALSE(none.matches("tools_ran", false)) << "absent, not false — nothing was measured";
}

// --- Bite 2: the reward counts what RAN, not only what was claimed (#941) ----------------------
//
// THE case that distinguishes an execution-backed reward from a projected one: two IDENTICAL
// actions — same message, same tool calls, same projected facts — where one's calls succeeded and
// the other's did not. A projection cannot tell them apart. The reward must.
TEST(Goals, IdenticalActionsScoreDifferentlyWhenTheCallsFail) {
    const std::vector<Goal> goals = {
        {"Every step of the Pipeline must actually run.", 5.0, {"tools_ran"}}};

    zetta::agent::StudioAction action;
    action.kind = zetta::agent::InteractionKind::Action;
    action.message = "I'll connect your source and train a model.";
    action.tool_calls.emplace_back("connect", nlohmann::json{{"name", "source"}});
    action.tool_calls.emplace_back("model", nlohmann::json{{"name", "model"}});

    const double ran = zetta::agent::goal_reward_for_execution(
        goals, action, /*grounding=*/"", {{"connect", "ok", ""}, {"model", "ok", ""}});
    const double failed = zetta::agent::goal_reward_for_execution(
        goals, action, /*grounding=*/"",
        {{"connect", "ok", ""}, {"model", "failed", "no such table"}});

    EXPECT_GT(ran, 0.9) << "calls that ran must be rewarded";
    EXPECT_LT(failed, 0.1) << "the SAME action whose calls failed must not be";
}

// Execution does not replace the projection — a goal about grounding still grades the response.
// Both sets of facts are in scope, so a reward can be about what it said AND what it did.
TEST(Goals, ExecutionAndResponseFactsAreBothInScope) {
    const std::vector<Goal> goals = {
        {"Never state a number we cannot evidence.", 1.0, {"numbers_from_context"}},
        {"Every step must actually run.", 1.0, {"tools_ran"}}};

    zetta::agent::StudioAction action;
    action.kind = zetta::agent::InteractionKind::Action;
    action.message = "Built it against 1200 rows.";
    action.tool_calls.emplace_back("connect", nlohmann::json::object());

    // Grounded figure AND a call that ran: both goals satisfied.
    const double both = zetta::agent::goal_reward_for_execution(
        goals, action, "Rows: 1200, columns: 9\n", {{"connect", "ok", ""}});
    EXPECT_GT(both, 0.9);

    // Same response, same figure — but the call failed. Half the goals now.
    const double half = zetta::agent::goal_reward_for_execution(
        goals, action, "Rows: 1200, columns: 9\n", {{"connect", "failed", "timeout"}});
    EXPECT_GT(half, 0.4);
    EXPECT_LT(half, 0.6) << "one of two equally-weighted goals held";
}

// --- Bite 3: a generation is RUN, and graded on what running it did (#941) ---------------------
//
// The reward RLVR takes is (prompt, completion) -> double. Until now that scored the parsed
// response. Here the emitted calls are APPLIED to a sandbox first, and the reward grades the
// outcome — so a rollout that names a tool the agent does not have earns less than one that runs.
//
// This is the whole point of verifiable reward: the environment answers, not the text.
TEST(Goals, AGenerationIsExecutedAndGradedOnTheOutcome) {
    const std::vector<Goal> goals = {
        {"Every step of the Pipeline must actually run.", 5.0, {"tools_ran"}}};

    // A sandbox with connect and model, and nothing else.
    zetta::agent::Sandbox sandbox;
    sandbox.register_tool("connect", [](zetta::agent::EndState& world, const nlohmann::json&) {
        world.set("connect", true);
        return zetta::agent::ToolResult::success();
    });
    sandbox.register_tool("model", [](zetta::agent::EndState& world, const nlohmann::json&) {
        world.set("model", true);
        return zetta::agent::ToolResult::success();
    });

    const std::string real =
        R"({"kind":"action","message":"Building it.","tool_calls":)"
        R"([{"tool":"connect","arguments":{}},{"tool":"model","arguments":{}}]})";
    const std::string invented =
        R"({"kind":"action","message":"Building it.","tool_calls":)"
        R"([{"tool":"connect","arguments":{}},{"tool":"autotune","arguments":{}}]})";

    const double ran = zetta::agent::goal_reward_for_rollout(goals, real, "", sandbox);
    const double made_up = zetta::agent::goal_reward_for_rollout(goals, invented, "", sandbox);

    EXPECT_GT(ran, 0.9) << "calls the sandbox could run must be rewarded";
    EXPECT_LT(made_up, 0.1)
        << "naming a tool the agent does not have must not be — the environment answered, not the text";
}

// A generation the harness cannot parse never reaches the sandbox and is worth nothing. Running
// half a malformed action would be worse than not running it.
TEST(Goals, AnUnparseableGenerationIsNeverExecuted) {
    const std::vector<Goal> goals = {{"Steps must run.", 1.0, {"tools_ran"}}};
    zetta::agent::Sandbox sandbox;
    bool touched = false;
    sandbox.register_tool("connect", [&touched](zetta::agent::EndState&, const nlohmann::json&) {
        touched = true;
        return zetta::agent::ToolResult::success();
    });

    EXPECT_DOUBLE_EQ(
        zetta::agent::goal_reward_for_rollout(goals, "Sure, I'll connect that!", "", sandbox), 0.0);
    EXPECT_FALSE(touched) << "nothing may run from a response the harness could not parse";
}

}  // namespace
