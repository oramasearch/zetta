// When an agent should retrain, and what it was trained for.
//
// The property that matters: staleness is a COMPARISON, not a judgement. An agent is stale exactly
// when the world it was trained for differs from the world in force — different tools, different
// goals, or a different corpus — so the UI can say so plainly and a schedule can act on it.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "zetta/agent/goal.hpp"
#include "zetta/agent/training_policy.hpp"

namespace {

using zetta::agent::AgentVersion;
using zetta::agent::Goal;
using zetta::agent::RetrainCadence;
using zetta::agent::TrainingSpec;
using zetta::agent::environment_fingerprint;

const std::vector<std::string> kTools = {"connect", "clean", "model", "serve"};
const std::vector<Goal> kGoals = {{"Never fabricate a number.", 9.0, {"numbers_from_context"}}};

TEST(TrainingPolicy, AddingAToolChangesTheEnvironment) {
    const std::string before = environment_fingerprint(kTools, kGoals, "corpus-v9");
    std::vector<std::string> more = kTools;
    more.push_back("act");
    EXPECT_NE(environment_fingerprint(more, kGoals, "corpus-v9"), before)
        << "a tool the agent can now call is a different world to train for";
}

TEST(TrainingPolicy, ReweightingAGoalChangesTheEnvironment) {
    const std::string before = environment_fingerprint(kTools, kGoals, "corpus-v9");
    std::vector<Goal> reweighted = kGoals;
    reweighted[0].importance = 1.0;
    EXPECT_NE(environment_fingerprint(kTools, reweighted, "corpus-v9"), before)
        << "importance is the reward weight — changing it changes what training optimizes";
}

// Re-interpreting a goal onto different checks changes what the reward MEASURES, even when nobody
// touched the wording.
TEST(TrainingPolicy, RemappingAGoalsChecksChangesTheEnvironment) {
    const std::string before = environment_fingerprint(kTools, kGoals, "corpus-v9");
    std::vector<Goal> remapped = kGoals;
    remapped[0].checks = {"grounded"};
    EXPECT_NE(environment_fingerprint(kTools, remapped, "corpus-v9"), before);
}

TEST(TrainingPolicy, ADifferentCorpusChangesTheEnvironment) {
    EXPECT_NE(environment_fingerprint(kTools, kGoals, "corpus-v9"),
              environment_fingerprint(kTools, kGoals, "corpus-v10"));
}

// Reordering is not a change. A user dragging a tool up the list has altered nothing about what
// the agent can do, and retraining on that would burn a GPU for an identical checkpoint.
TEST(TrainingPolicy, ReorderingToolsOrGoalsIsNotAChange) {
    std::vector<std::string> shuffled = {"serve", "connect", "model", "clean"};
    EXPECT_EQ(environment_fingerprint(shuffled, kGoals, "corpus-v9"),
              environment_fingerprint(kTools, kGoals, "corpus-v9"));

    const std::vector<Goal> two = {{"A", 1.0, {"grounded"}}, {"B", 2.0, {"calibrated"}}};
    const std::vector<Goal> two_reversed = {{"B", 2.0, {"calibrated"}}, {"A", 1.0, {"grounded"}}};
    EXPECT_EQ(environment_fingerprint(kTools, two, "c"),
              environment_fingerprint(kTools, two_reversed, "c"));
}

// An agent that has never trained is UNTRAINED, not stale. Conflating them would have every fresh
// agent nagging to be retrained before it had been trained once.
TEST(TrainingPolicy, AnUntrainedAgentIsNotStale) {
    EXPECT_FALSE(zetta::agent::needs_retraining({}, environment_fingerprint(kTools, kGoals, "c")));
}

TEST(TrainingPolicy, AVersionTrainedForTheCurrentEnvironmentIsNotStale) {
    const std::string print = environment_fingerprint(kTools, kGoals, "corpus-v9");
    const std::vector<AgentVersion> versions = {{"/ck/v1", "2026-07-27T00:00:00Z", "job1", print,
                                                 "corpus-v9", 0.98}};
    EXPECT_FALSE(zetta::agent::needs_retraining(versions, print));

    std::vector<std::string> more = kTools;
    more.push_back("act");
    EXPECT_TRUE(zetta::agent::needs_retraining(
        versions, environment_fingerprint(more, kGoals, "corpus-v9")))
        << "adding a tool must make the deployed checkpoint stale";
}

TEST(TrainingPolicy, ManualNeverFiresOnItsOwn) {
    const std::string print = environment_fingerprint(kTools, kGoals, "c");
    TrainingSpec spec;  // Manual is the default: an agent never retrains behind your back.
    EXPECT_FALSE(zetta::agent::retrain_due(spec, {}, print, "2026-07-27T00:00:00Z"));
    EXPECT_FALSE(zetta::agent::retrain_due(
        spec, {{"/ck", "2020-01-01T00:00:00Z", "", "stale-print", "c", -1.0}}, print,
        "2026-07-27T00:00:00Z"));
}

// A schedule that fires when nothing changed burns a GPU reproducing the checkpoint it already has.
TEST(TrainingPolicy, OnChangeFiresOnlyWhenTheEnvironmentMoved) {
    const std::string print = environment_fingerprint(kTools, kGoals, "c");
    TrainingSpec spec;
    spec.cadence = RetrainCadence::OnChange;
    const std::vector<AgentVersion> current = {{"/ck", "2026-07-01T00:00:00Z", "", print, "c", -1.0}};
    EXPECT_FALSE(zetta::agent::retrain_due(spec, current, print, "2026-07-27T00:00:00Z"));

    const std::vector<AgentVersion> stale = {{"/ck", "2026-07-01T00:00:00Z", "", "other", "c", -1.0}};
    EXPECT_TRUE(zetta::agent::retrain_due(spec, stale, print, "2026-07-27T00:00:00Z"));
}

TEST(TrainingPolicy, DailyAndMonthlyFireOnTheCalendarBoundary) {
    const std::string print = environment_fingerprint(kTools, kGoals, "c");
    TrainingSpec daily;
    daily.cadence = RetrainCadence::Daily;
    const std::vector<AgentVersion> yesterday = {
        {"/ck", "2026-07-26T23:00:00Z", "", print, "c", -1.0}};
    EXPECT_TRUE(zetta::agent::retrain_due(daily, yesterday, print, "2026-07-27T01:00:00Z"));
    EXPECT_FALSE(zetta::agent::retrain_due(daily, yesterday, print, "2026-07-26T23:59:00Z"));

    TrainingSpec monthly;
    monthly.cadence = RetrainCadence::Monthly;
    EXPECT_TRUE(zetta::agent::retrain_due(monthly, yesterday, print, "2026-08-01T00:00:00Z"));
    EXPECT_FALSE(zetta::agent::retrain_due(monthly, yesterday, print, "2026-07-31T00:00:00Z"));
}

// A scheduled agent that has never trained trains, rather than waiting a full period first.
TEST(TrainingPolicy, AScheduledAgentWithNoVersionsIsDue) {
    TrainingSpec spec;
    spec.cadence = RetrainCadence::Weekly;
    EXPECT_TRUE(zetta::agent::retrain_due(spec, {}, "print", "2026-07-27T00:00:00Z"));
}

TEST(TrainingPolicy, RoundTripsThroughJson) {
    TrainingSpec spec{"corpus-v9", "/ck/base", 12000, RetrainCadence::Weekly};
    std::string error;
    const auto restored = zetta::agent::training_spec_from_json(zetta::agent::to_json(spec), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    EXPECT_EQ(*restored, spec);

    const AgentVersion version{"/ck/v1", "2026-07-27T00:00:00Z", "job1", "print", "corpus-v9", 0.98};
    const auto version_back =
        zetta::agent::agent_version_from_json(zetta::agent::to_json(version), &error);
    ASSERT_TRUE(version_back.has_value()) << error;
    EXPECT_EQ(*version_back, version);
}

TEST(TrainingPolicy, MalformedInputIsRejectedWithAReason) {
    std::string error;
    EXPECT_FALSE(zetta::agent::training_spec_from_json({{"cadence", "hourly"}}, &error).has_value());
    EXPECT_NE(error.find("cadence"), std::string::npos);
    EXPECT_FALSE(zetta::agent::training_spec_from_json({{"steps", -1}}, &error).has_value());
    EXPECT_FALSE(zetta::agent::agent_version_from_json({{"checkpoint", ""}}, &error).has_value());
}

}  // namespace
