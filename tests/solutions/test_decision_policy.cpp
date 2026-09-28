#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using Json = nlohmann::json;
using zetta::cluster::ReplicationManager;
using zetta::solutions::DecisionPolicyStore;

Json policy_config(std::string autonomy = "A2") {
    return {{"kill_switch", false},
            {"threshold", 0.5},
            {"tiers",
             {{{"name", "high"}, {"minimum_score", 0.8}, {"action", "call"}},
              {{"name", "medium"}, {"minimum_score", 0.5}, {"action", "email"}}}},
            {"budget", {{"limit", 100.0}}},
            {"contact_cap", 3},
            {"frequency_cap", {{"max_actions", 2}, {"window_seconds", 86400}}},
            {"eligibility_rules",
             {{{"field", "eligible"}, {"operator", "eq"}, {"value", true}}}},
            {"exclusion_rules",
             {{{"field", "country"}, {"operator", "eq"}, {"value", "blocked"}}}},
            {"autonomy", std::move(autonomy)},
            {"tunable_bounds",
             {{"threshold", {{"minimum", 0.4}, {"maximum", 0.9}}},
              {"budget.limit", {{"minimum", 50.0}, {"maximum", 200.0}}}}}};
}

Json entity(
    std::string id,
    double score = 0.9,
    int contacts = 0,
    int recent_actions = 0,
    std::string country = "allowed") {
    return {{"entity_id", std::move(id)},
            {"score", score},
            {"cost", 10.0},
            {"contacts", contacts},
            {"recent_actions", recent_actions},
            {"eligible", true},
            {"country", std::move(country)}};
}

}  // namespace

TEST(DecisionPolicy, EnforcesExclusionsAndContactAndFrequencyCaps) {
    ReplicationManager replication{zetta::util::new_uuid()};
    DecisionPolicyStore store{replication};
    std::string error;
    const auto policy = store.create("retention", policy_config(), "owner", &error);
    ASSERT_TRUE(policy) << error;

    const auto excluded = store.decide(
        policy->id,
        entity("excluded", 0.9, 0, 0, "blocked"),
        "agent",
        &error);
    ASSERT_TRUE(excluded) << error;
    EXPECT_EQ(excluded->status, "blocked");
    EXPECT_NE(excluded->reasons.front().find("exclusion"), std::string::npos);

    const auto contact_capped = store.decide(
        policy->id,
        entity("contact-capped", 0.9, 3),
        "agent",
        &error);
    ASSERT_TRUE(contact_capped) << error;
    EXPECT_EQ(contact_capped->status, "blocked");
    EXPECT_EQ(contact_capped->reasons.front(), "contact cap reached");

    const auto frequency_capped = store.decide(
        policy->id,
        entity("frequency-capped", 0.9, 0, 2),
        "agent",
        &error);
    ASSERT_TRUE(frequency_capped) << error;
    EXPECT_EQ(frequency_capped->status, "blocked");
    EXPECT_EQ(frequency_capped->reasons.front(), "frequency cap reached");
}

TEST(DecisionPolicy, A1WaitsForApprovalAndRecordsPolicyVersion) {
    zetta::test::TempDir directory;
    ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    DecisionPolicyStore store{replication, &node};
    std::string error;
    const auto policy = store.create("reviewed-retention", policy_config("A1"), "owner", &error);
    ASSERT_TRUE(policy) << error;

    const auto pending = store.decide(policy->id, entity("customer-1"), "agent", &error);
    ASSERT_TRUE(pending) << error;
    EXPECT_EQ(pending->policy_version, 1u);
    EXPECT_EQ(pending->status, "pending_approval");
    EXPECT_FALSE(pending->actionable);

    const auto approved = store.approve(pending->id, "reviewer", &error);
    ASSERT_TRUE(approved) << error;
    EXPECT_EQ(approved->status, "approved");
    EXPECT_TRUE(approved->actionable);
    EXPECT_EQ(approved->policy_version, 1u);
    EXPECT_EQ(approved->approved_by, "reviewer");

    const auto events = node.audit_events();
    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events.back()->value().target, pending->id);
    node.stop();
}

TEST(DecisionPolicy, KillSwitchStopsAllActions) {
    ReplicationManager replication{zetta::util::new_uuid()};
    DecisionPolicyStore store{replication};
    Json config = policy_config("A3");
    config["kill_switch"] = true;
    std::string error;
    const auto policy = store.create("stopped", std::move(config), "owner", &error);
    ASSERT_TRUE(policy) << error;

    const auto decision = store.decide(policy->id, entity("customer-1"), "agent", &error);
    ASSERT_TRUE(decision) << error;
    EXPECT_EQ(decision->status, "blocked");
    EXPECT_FALSE(decision->actionable);
    EXPECT_EQ(decision->reasons.front(), "policy kill-switch is active");
}

TEST(DecisionPolicy, KillSwitchAlsoStopsPendingApproval) {
    ReplicationManager replication{zetta::util::new_uuid()};
    DecisionPolicyStore store{replication};
    std::string error;
    const auto policy = store.create("approval-stop", policy_config("A1"), "owner", &error);
    ASSERT_TRUE(policy) << error;
    const auto pending = store.decide(policy->id, entity("customer-1"), "agent", &error);
    ASSERT_TRUE(pending) << error;

    Json stopped = policy_config("A1");
    stopped["kill_switch"] = true;
    const auto updated = store.update(
        policy->id,
        std::move(stopped),
        std::nullopt,
        "owner",
        "emergency stop",
        &error);
    ASSERT_TRUE(updated) << error;

    const auto approved = store.approve(pending->id, "reviewer", &error);
    EXPECT_FALSE(approved);
    EXPECT_EQ(error, "policy kill-switch is active");
    EXPECT_EQ(store.get_decision(pending->id)->status, "pending_approval");
}

TEST(DecisionPolicy, EnforcesEligibilityThresholdTiersAndBudget) {
    ReplicationManager replication{zetta::util::new_uuid()};
    DecisionPolicyStore store{replication};
    Json config = policy_config();
    config["budget"]["limit"] = 50.0;
    std::string error;
    const auto policy = store.create("constraints", std::move(config), "owner", &error);
    ASSERT_TRUE(policy) << error;

    Json selected_entity = entity("selected");
    selected_entity["cost"] = 30.0;
    const auto selected = store.decide(
        policy->id,
        std::move(selected_entity),
        "agent",
        &error);
    ASSERT_TRUE(selected) << error;
    EXPECT_EQ(selected->tier, "high");
    EXPECT_EQ(selected->action, "call");
    EXPECT_TRUE(selected->actionable);

    Json ineligible = entity("ineligible");
    ineligible["eligible"] = false;
    const auto eligibility_block = store.decide(
        policy->id,
        std::move(ineligible),
        "agent",
        &error);
    ASSERT_TRUE(eligibility_block) << error;
    EXPECT_EQ(eligibility_block->status, "blocked");
    EXPECT_NE(eligibility_block->reasons.front().find("eligibility"), std::string::npos);

    const auto threshold_block = store.decide(
        policy->id,
        entity("low-score", 0.2),
        "agent",
        &error);
    ASSERT_TRUE(threshold_block) << error;
    EXPECT_EQ(threshold_block->reasons.front(), "score is below the policy threshold");

    Json over_budget = entity("over-budget");
    over_budget["cost"] = 30.0;
    const auto budget_block = store.decide(
        policy->id,
        std::move(over_budget),
        "agent",
        &error);
    ASSERT_TRUE(budget_block) << error;
    EXPECT_EQ(budget_block->reasons.front(), "budget exhausted");
}

TEST(DecisionPolicy, HoldoutSuppressesActionAndRecordsTheCell) {
    zetta::test::TempDir directory;
    ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());

    zetta::solutions::ExperimentValue experiment_value;
    experiment_value.name = "retention-holdout";
    experiment_value.seed = 1128;
    experiment_value.allocations = {
        {"control", zetta::solutions::ExperimentTier::Holdout, 0.5},
        {"treatment", zetta::solutions::ExperimentTier::Treatment, 0.5}};
    std::string error;
    const auto experiment =
        node.experiment_manager().create(std::move(experiment_value), &error);
    ASSERT_TRUE(experiment) << error;

    std::string holdout_entity;
    for (int index = 0; index < 100 && holdout_entity.empty(); ++index) {
        const std::string candidate = "customer-" + std::to_string(index);
        const auto assignment = node.experiment_manager().assign(
            experiment->id,
            candidate,
            false,
            {},
            {},
            &error);
        ASSERT_TRUE(assignment) << error;
        if (assignment->value.tier == zetta::solutions::ExperimentTier::Holdout) {
            holdout_entity = candidate;
        }
    }
    ASSERT_FALSE(holdout_entity.empty());

    DecisionPolicyStore store{replication, &node};
    const auto policy = store.create(
        "experiment-aware-retention",
        policy_config("A3"),
        "owner",
        &error);
    ASSERT_TRUE(policy) << error;
    Json scored_entity = entity(holdout_entity);
    scored_entity["experiment"] = experiment->id;
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::ApiContext context{node};
    const auto decided = registry.call(
        "decide",
        {{"policy_id", policy->id}, {"entity", std::move(scored_entity)}},
        context);
    ASSERT_TRUE(decided.ok) << decided.error.message;
    EXPECT_EQ(decided.value.at("experiment"), experiment->id);
    EXPECT_EQ(decided.value.at("cell"), "control");
    EXPECT_EQ(decided.value.at("status"), "blocked");
    EXPECT_FALSE(decided.value.at("actionable").get<bool>());
    EXPECT_EQ(decided.value.at("action"), "");

    const auto stored = store.get_decision(
        decided.value.at("id").get<std::string>());
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->cell, "control");
    EXPECT_TRUE(stored->action.empty());
    node.stop();
}

TEST(DecisionPolicy, UpdatesCreateVersionsAndAgentTuningStaysWithinBounds) {
    ReplicationManager replication{zetta::util::new_uuid()};
    DecisionPolicyStore store{replication};
    std::string error;
    const auto created = store.create("bounded", policy_config(), "owner", &error);
    ASSERT_TRUE(created) << error;

    const auto updated = store.update(
        created->id,
        std::nullopt,
        Json{{"threshold", 0.7}, {"budget.limit", 150.0}},
        "agent",
        "raise confidence",
        &error);
    ASSERT_TRUE(updated) << error;
    ASSERT_EQ(updated->versions.size(), 2u);
    EXPECT_EQ(updated->current()->version, 2u);
    EXPECT_EQ(updated->versions.front().config.at("threshold"), 0.5);
    EXPECT_EQ(updated->current()->config.at("threshold"), 0.7);

    const auto rejected = store.update(
        created->id,
        std::nullopt,
        Json{{"threshold", 0.95}},
        "agent",
        "outside bounds",
        &error);
    EXPECT_FALSE(rejected);
    EXPECT_NE(error.find("outside declared bounds"), std::string::npos);
    EXPECT_EQ(store.get(created->id)->versions.size(), 2u);
}

TEST(DecisionPolicy, BatchWritesVersionedDecisionTable) {
    zetta::test::TempDir directory;
    ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    auto database = node.create_database("scores");
    ASSERT_TRUE(database);
    auto table = database->create_table("customers");
    ASSERT_TRUE(table);
    table->add_row(entity("one", 0.95));
    table->add_row(entity("two", 0.2));

    DecisionPolicyStore store{replication, &node};
    std::string error;
    const auto policy = store.create("batch", policy_config(), "owner", &error);
    ASSERT_TRUE(policy) << error;
    zetta::solutions::BatchDecisionOptions options;
    options.database = "scores";
    options.table = "customers";
    options.output_table = "decisions";
    const auto result = zetta::solutions::decide_table(
        node,
        store,
        policy->id,
        options,
        "agent");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.table, "decision_results.decisions");
    EXPECT_EQ(result.rows, 2u);
    EXPECT_EQ(result.actionable, 1u);
    EXPECT_EQ(result.blocked, 1u);
    const auto output = node.find_database("decision_results")->find_table("decisions");
    ASSERT_TRUE(output);
    ASSERT_EQ(output->row_count(), 2u);
    EXPECT_EQ(output->store().at(0)->at("policy_version"), 1);
    node.stop();
}

TEST(DecisionPolicy, ReplicatesPoliciesAndDecisions) {
    ReplicationManager source{zetta::util::new_uuid()};
    ReplicationManager peer{zetta::util::new_uuid()};
    DecisionPolicyStore source_store{source};
    std::string error;
    const auto policy = source_store.create("replicated", policy_config(), "owner", &error);
    ASSERT_TRUE(policy) << error;
    const auto decision = source_store.decide(policy->id, entity("customer"), "agent", &error);
    ASSERT_TRUE(decision) << error;

    const std::vector<std::byte> state = source.save();
    peer.merge(state);
    DecisionPolicyStore peer_store{peer};
    ASSERT_TRUE(peer_store.get(policy->id));
    const auto peer_decision = peer_store.get_decision(decision->id);
    ASSERT_TRUE(peer_decision);
    EXPECT_EQ(peer_decision->policy_version, 1u);
}

TEST(DecisionPolicy, PersistsInTheNodeCatalogAcrossRestart) {
    zetta::test::TempDir directory;
    const std::string node_id = zetta::util::new_uuid();
    std::string policy_id;
    std::string decision_id;
    {
        ReplicationManager replication{node_id};
        zetta::Node node{{directory.path().string(), false}, &replication};
        ASSERT_TRUE(node.start());
        DecisionPolicyStore store{replication, &node};
        std::string error;
        const auto policy = store.create("durable", policy_config(), "owner", &error);
        ASSERT_TRUE(policy) << error;
        const auto decision = store.decide(policy->id, entity("customer"), "agent", &error);
        ASSERT_TRUE(decision) << error;
        policy_id = policy->id;
        decision_id = decision->id;
        node.tick();
        node.stop();
    }

    ReplicationManager replication{node_id};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    DecisionPolicyStore store{replication, &node};
    const auto policy = store.get(policy_id);
    const auto decision = store.get_decision(decision_id);
    ASSERT_TRUE(policy);
    ASSERT_TRUE(decision);
    EXPECT_EQ(policy->current()->version, 1u);
    EXPECT_EQ(decision->policy_version, 1u);
    node.stop();
}

TEST(DecisionPolicy, RequestTimeP99StaysBelowFiftyMilliseconds) {
    ReplicationManager replication{zetta::util::new_uuid()};
    DecisionPolicyStore store{replication};
    Json config = policy_config("A3");
    std::string error;
    const auto policy = store.create("latency", std::move(config), "owner", &error);
    ASSERT_TRUE(policy) << error;
    std::vector<double> milliseconds;
    for (int index = 0; index < 64; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const auto decision = store.decide(
            policy->id,
            entity("entity-" + std::to_string(index)),
            "agent",
            &error);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        ASSERT_TRUE(decision) << error;
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(elapsed).count());
    }
    std::ranges::sort(milliseconds);
    const std::size_t p99_index = (milliseconds.size() * 99u) / 100u;
    std::cout << "request-time decide p99_ms=" << milliseconds.at(p99_index)
              << " samples=" << milliseconds.size() << '\n';
    EXPECT_LT(milliseconds.at(p99_index), 50.0);
}

TEST(DecisionPolicyCapabilities, CreateDecideApproveJourney) {
    zetta::test::TempDir directory;
    ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::ApiContext context{node};

    const auto created = registry.call(
        "create_policy",
        {{"name", "journey"}, {"config", policy_config("A1")}},
        context);
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string policy_id = created.value.at("id").get<std::string>();
    const auto read = registry.call("get_policy", {{"policy_id", policy_id}}, context);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value.at("current_version"), 1);

    const auto decided = registry.call(
        "decide",
        {{"policy_id", policy_id}, {"entity", entity("customer")}},
        context);
    ASSERT_TRUE(decided.ok) << decided.error.message;
    EXPECT_EQ(decided.value.at("status"), "pending_approval");
    const auto approved = registry.call(
        "approve_decision",
        {{"decision_id", decided.value.at("id")}},
        context);
    ASSERT_TRUE(approved.ok) << approved.error.message;
    EXPECT_TRUE(approved.value.at("actionable").get<bool>());
    node.stop();
}
