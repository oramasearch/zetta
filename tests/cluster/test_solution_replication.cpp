// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/action_integration.hpp"
#include "zetta/solutions/audit_ledger.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/solutions/roi_readout.hpp"
#include "zetta/solutions/snapshot.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using Json = nlohmann::json;
using zetta::solutions::ExperimentTier;

zetta::solutions::MonitorSpec monitor_spec(const std::string& solution) {
    zetta::solutions::MonitorSpec spec;
    spec.solution = solution;
    spec.guardrails = {{"complaints", 0.0, 0.1}};
    return spec;
}

zetta::solutions::ExperimentValue experiment_value(const std::string& name) {
    zetta::solutions::ExperimentValue value;
    value.name = name;
    value.seed = 1128;
    value.allocations = {{"holdout", ExperimentTier::Holdout, 0.1},
                         {"treatment", ExperimentTier::Treatment, 0.9}};
    return value;
}

std::string assignment_key(const zetta::solutions::ExperimentAssignmentRecord& record) {
    return "experiment_assignment:" + record.value.experiment + ":" + record.id;
}

zetta::solutions::ActionLedgerValue action_value() {
    zetta::solutions::ActionLedgerValue value;
    value.subject = "customer";
    value.score_id = "score";
    value.score = 0.8;
    value.decision = "decision";
    value.action = "no_contact";
    value.actor = "owner";
    value.model_version = "model-v1";
    value.policy_version = "policy-v1";
    value.agent_version = "retention-v1";
    value.cell = "holdout";
    value.currency = "USD";
    value.score_at = "2026-09-28T00:00:00Z";
    value.decision_at = value.score_at;
    value.action_at = value.score_at;
    return value;
}

// Match #1086's one-tick test, with enough unrelated keys to guarantee that
// none of these small changes falls back to a full snapshot (#1128).
class SolutionReplication : public ::testing::Test {
protected:
    zetta::test::TempDir source_directory;
    zetta::test::TempDir replica_directory;
    zetta::cluster::ReplicationManager source_replication{zetta::util::new_uuid()};
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node source{{source_directory.path().string(), false}, &source_replication};
    zetta::Node node{{replica_directory.path().string(), false}, &replication};
    std::uint64_t token = 0;

    void SetUp() override {
        ASSERT_TRUE(source.start());
        for (int index = 0; index < 128; ++index) {
            source_replication.set("fixture:seed-" + std::to_string(index),
                                   {{"value", index}});
        }
        replication.merge(source_replication.save());
        ASSERT_TRUE(node.start());
        node.tick();
        token = replication.changes_since(0).next_token;
    }

    void TearDown() override {
        node.stop();
        source.stop();
    }

    void merge_and_tick() {
        replication.merge(source_replication.save());
        const auto changes = replication.changes_since(token);
        ASSERT_FALSE(changes.full_resync);
        ASSERT_FALSE(changes.keys.empty());
        ASSERT_LE(changes.keys.size(),
                  std::max<std::size_t>(1, changes.snapshot->size() / 4));
        const std::string version = replication.version();
        node.tick();
        EXPECT_EQ(replication.version(), version) << "reconcile must not recapture";
        token = changes.next_token;
    }
};

TEST_F(SolutionReplication, MonitorChangesAreScopedAndSurviveRestart) {
    const auto changed = source.create_monitor(monitor_spec("changed"));
    const auto untouched = source.create_monitor(monitor_spec("untouched"));
    ASSERT_TRUE(changed);
    ASSERT_TRUE(untouched);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    ASSERT_TRUE(node.find_monitor(changed->id()));
    ASSERT_TRUE(node.find_monitor(untouched->id()));
    const Json untouched_before = node.find_monitor(untouched->id())->metadata();

    ASSERT_TRUE(changed->observe_guardrail("complaints", 0.2).ok);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    ASSERT_TRUE(node.find_monitor(changed->id()));
    EXPECT_EQ(node.find_monitor(changed->id())->metadata(), changed->metadata());
    EXPECT_EQ(node.find_monitor(untouched->id())->metadata(), untouched_before);

    source_replication.set("monitor:invalid", {{"solution", 42}});
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_FALSE(node.find_monitor("invalid"));
    EXPECT_EQ(node.monitors().size(), 2u);
    source_replication.remove("monitor:" + changed->id());
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_FALSE(node.find_monitor(changed->id()));
    EXPECT_TRUE(node.find_monitor(untouched->id()));

    node.stop();
    zetta::Node reloaded{{replica_directory.path().string(), false}, &replication};
    ASSERT_TRUE(reloaded.start());
    EXPECT_FALSE(reloaded.find_monitor(changed->id()));
    ASSERT_TRUE(reloaded.find_monitor(untouched->id()));
    EXPECT_EQ(reloaded.find_monitor(untouched->id())->metadata(), untouched_before);
    reloaded.stop();
}

TEST_F(SolutionReplication, ExperimentsAndAssignmentsUpdateAndRemoveIndependently) {
    auto& manager = source.experiment_manager();
    const auto changed = manager.create(experiment_value("changed"));
    const auto untouched = manager.create(experiment_value("untouched"));
    ASSERT_TRUE(changed);
    ASSERT_TRUE(untouched);
    const auto first = manager.assign(changed->id, "first");
    const auto second = manager.assign(changed->id, "second");
    const auto other = manager.assign(untouched->id, "other");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(other);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    auto& replica_manager = node.experiment_manager();
    ASSERT_TRUE(replica_manager.report(changed->id));
    EXPECT_EQ(replica_manager.report(changed->id)->assignments, 2u);
    EXPECT_EQ(replica_manager.report(untouched->id)->assignments, 1u);

    Json assignment = zetta::solutions::to_json(first->value);
    assignment["exempt"] = true;
    assignment["exemption_reason"] = "manual service";
    assignment["exemption_actor"] = "owner";
    source_replication.set(assignment_key(*first), assignment);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(replica_manager.report(changed->id)->exemptions, 1u);
    EXPECT_EQ(replica_manager.report(changed->id)->assignments, 2u);
    EXPECT_EQ(replica_manager.report(untouched->id)->assignments, 1u);

    Json experiment = zetta::solutions::to_json(changed->value);
    experiment["name"] = "renamed";
    source_replication.set("experiment:" + changed->id, experiment);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    ASSERT_TRUE(replica_manager.find("renamed"));
    EXPECT_EQ(replica_manager.report(changed->id)->assignments, 2u);

    source_replication.remove(assignment_key(*first));
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(replica_manager.report(changed->id)->assignments, 1u);
    EXPECT_EQ(replica_manager.report(changed->id)->exemptions, 0u);
    source_replication.set(assignment_key(*second), {{"experiment", changed->id}});
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(replica_manager.report(changed->id)->assignments, 0u);
    EXPECT_EQ(replica_manager.report(untouched->id)->assignments, 1u);

    source_replication.remove("experiment:" + changed->id);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_FALSE(replica_manager.find(changed->id));
    EXPECT_EQ(replica_manager.experiments().size(), 1u);
    EXPECT_FALSE(std::filesystem::exists(replica_directory.path() / "experiments" / changed->id));
}

TEST_F(SolutionReplication, LateExperimentRevisitsChildrenAndParentRemovalCascades) {
    const auto experiment = source.experiment_manager().create(experiment_value("late"));
    ASSERT_TRUE(experiment);
    const auto assigned = source.experiment_manager().assign(experiment->id, "customer");
    ASSERT_TRUE(assigned);
    source_replication.remove("experiment:" + experiment->id);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_FALSE(node.experiment_manager().find(experiment->id));

    source_replication.set("experiment:" + experiment->id,
                           zetta::solutions::to_json(experiment->value));
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    ASSERT_TRUE(node.experiment_manager().report(experiment->id));
    EXPECT_EQ(node.experiment_manager().report(experiment->id)->assignments, 1u);
    source_replication.remove("experiment:" + experiment->id);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_FALSE(node.experiment_manager().find(experiment->id));

    source_replication.set("experiment:" + experiment->id,
                           zetta::solutions::to_json(experiment->value));
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(node.experiment_manager().report(experiment->id)->assignments, 1u);
    node.stop();
    zetta::Node reloaded{{replica_directory.path().string(), false}, &replication};
    ASSERT_TRUE(reloaded.start());
    ASSERT_TRUE(reloaded.experiment_manager().report(experiment->id));
    EXPECT_EQ(reloaded.experiment_manager().report(experiment->id)->assignments, 1u);
    reloaded.stop();
}

TEST_F(SolutionReplication, LedgerRemainsIdempotentAndAuditDoesNotEnterCatalog) {
    const auto action = source.record_action(action_value());
    ASSERT_TRUE(action);
    zetta::solutions::OutcomeLedgerValue outcome;
    outcome.action_id = action->id();
    outcome.outcome = "retained";
    outcome.value = 1.0;
    outcome.currency = "USD";
    outcome.occurred_at = "2026-09-28T00:00:00Z";
    const auto recorded = source.record_outcome(outcome);
    ASSERT_TRUE(recorded);
    source.record_audit_event({zetta::governance::AuditAction::DataAccess,
                               "owner", "ledger", true, "read", {}});
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    ASSERT_TRUE(node.find_ledger_entry(action->id()));
    ASSERT_TRUE(node.find_ledger_entry(recorded->id()));
    EXPECT_EQ(node.find_ledger_entry(recorded->id())->action_id(), action->id());
    EXPECT_EQ(node.ledger_entry_count(), 2u);
    EXPECT_EQ(node.audit_event_count(), 0u);
    EXPECT_EQ(source.audit_event_count(), 1u);

    const Json original = node.find_ledger_entry(action->id())->metadata();
    Json replacement = original;
    replacement["cost"] = 900.0;
    source_replication.set("ledger:" + action->id(), replacement);
    source_replication.set("ledger:invalid", {{"kind", "action"}});
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(node.find_ledger_entry(action->id())->metadata(), original);
    EXPECT_FALSE(node.find_ledger_entry("invalid"));
    source_replication.remove("ledger:" + recorded->id());
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_TRUE(node.find_ledger_entry(recorded->id()));
    node.tick();
    EXPECT_EQ(node.ledger_entry_count(), 2u);
    EXPECT_EQ(node.audit_event_count(), 0u);
}

TEST_F(SolutionReplication, PolicyAndDecisionStoresReadIncrementalChangesDirectly) {
    zetta::solutions::DecisionPolicyStore writer(source_replication);
    zetta::solutions::DecisionPolicyStore reader(replication);
    Json config = {{"autonomy", "A1"}, {"kill_switch", false}};
    const auto policy = writer.create("retention", config, "owner");
    ASSERT_TRUE(policy);
    const auto decision = writer.decide(policy->id,
        {{"entity_id", "customer"}, {"score", 0.9}, {"action", "task"}}, "owner");
    ASSERT_TRUE(decision);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    ASSERT_TRUE(reader.get(policy->id));
    ASSERT_TRUE(reader.get_decision(decision->id));
    EXPECT_EQ(reader.get_decision(decision->id)->status, "pending_approval");

    ASSERT_TRUE(writer.approve(decision->id, "owner"));
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_TRUE(reader.get_decision(decision->id)->actionable);
    config["kill_switch"] = true;
    ASSERT_TRUE(writer.update(policy->id, config, std::nullopt, "owner", "stop"));
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(reader.get(policy->id)->current()->version, 2u);
    EXPECT_TRUE(reader.delivery_blocker(*reader.get_decision(decision->id)));

    source_replication.remove("decision:" + decision->id);
    source_replication.remove("decision_policy:" + policy->id);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_FALSE(reader.get_decision(decision->id));
    EXPECT_FALSE(reader.get(policy->id));
}

TEST_F(SolutionReplication, RoiDefinitionsReadIncrementalVersionsDirectly) {
    zetta::solutions::RoiReadoutStore writer(source_replication);
    zetta::solutions::RoiReadoutStore reader(replication);
    zetta::solutions::RoiReadoutSpec spec;
    spec.solution = "retention";
    spec.project = "project";
    spec.graph = "graph";
    spec.experiment = "experiment";
    spec.roi_model = {"retained", 100.0, 200.0, "USD"};
    const auto first = writer.configure(spec, "owner");
    ASSERT_TRUE(first);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    ASSERT_TRUE(reader.get(spec.solution));
    EXPECT_EQ(reader.get(spec.solution)->id, first->id);

    spec.roi_model.investment = 300.0;
    const auto second = writer.configure(spec, "owner");
    ASSERT_TRUE(second);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(reader.get(spec.solution)->version, 2u);
    EXPECT_DOUBLE_EQ(reader.get(spec.solution)->spec.roi_model.investment, 300.0);
    source_replication.remove("roi_readout:" + second->id);
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    EXPECT_EQ(reader.get(spec.solution)->id, first->id);
}

TEST_F(SolutionReplication, ActionOutboxAndSnapshotUseIncrementalTableRows) {
    zetta::solutions::DecisionPolicyStore policies(source_replication);
    const auto policy = policies.create("tasks", {{"autonomy", "A2"}}, "owner");
    ASSERT_TRUE(policy);
    const auto decision = policies.decide(policy->id,
        {{"entity_id", "customer"}, {"score", 0.9}, {"action", "task"}}, "owner");
    ASSERT_TRUE(decision);
    zetta::solutions::ActionTaskSpec task;
    task.decision_id = decision->id;
    task.owner = "retention";
    task.sla_due_at = "2026-09-30T00:00:00Z";
    task.connector.endpoint = "https://crm.example.test/tasks";
    task.idempotency_key = decision->id;
    task.model_version = "model-v1";
    task.agent_version = "retention-v1";
    task.cell = "treatment";
    task.currency = "USD";
    zetta::solutions::ActionRuntime writer(source);
    const auto queued = writer.enqueue_task(task, "owner");
    ASSERT_TRUE(queued);

    const auto database = source.create_database("features");
    ASSERT_TRUE(database);
    const auto table = database->create_table("events");
    ASSERT_TRUE(table);
    table->add_row({{"customer", 1}, {"time", 10}, {"score", 0.8}});
    const zetta::solutions::SnapshotSpec snapshot{
        "features", "events", "frozen", "customer", "time", 20};
    ASSERT_TRUE(zetta::solutions::materialize_snapshot(source, snapshot).ok());
    ASSERT_NO_FATAL_FAILURE(merge_and_tick());
    zetta::solutions::ActionRuntime reader(node);
    ASSERT_TRUE(reader.find_task(queued->task_id));
    EXPECT_EQ(reader.find_task(queued->task_id)->status,
              zetta::solutions::ActionTaskStatus::Pending);
    const auto replica_database = node.find_database("features");
    ASSERT_TRUE(replica_database);
    const auto frozen = replica_database->find_table("frozen");
    ASSERT_TRUE(frozen);
    ASSERT_EQ(frozen->store().size(), 1u);
    EXPECT_EQ(frozen->store().at(0)->at("as_of"), 20);
}

}  // namespace
