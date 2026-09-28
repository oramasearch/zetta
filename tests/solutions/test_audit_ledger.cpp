#include <gtest/gtest.h>

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/solutions/audit_ledger.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::solutions::ActionActorKind;
using zetta::solutions::ActionLedgerValue;
using zetta::solutions::LedgerEntryKind;
using zetta::solutions::OutcomeLedgerValue;
using zetta::solutions::OutcomeSource;
using zetta::test::TempDir;

NodeConfig config_for(const TempDir& directory) {
    return NodeConfig{directory.path().string(), /*preflight=*/false};
}

ActionLedgerValue action_value(std::string action = "email") {
    ActionLedgerValue value;
    value.subject = "account-42";
    value.score_id = "score-42";
    value.score = 0.91;
    value.decision = "retain";
    value.action = std::move(action);
    value.actor_kind = ActionActorKind::Agent;
    value.actor = "retention-agent";
    value.model_version = "churn-v7";
    value.policy_version = "retention-policy-v3";
    value.agent_version = "agent-v5";
    value.cell = "treatment-b";
    value.cost = 1.25;
    value.currency = "USD";
    value.score_at = "2026-09-28T10:00:00.000Z";
    value.decision_at = "2026-09-28T10:00:01.000Z";
    value.action_at = "2026-09-28T10:00:02.000Z";
    return value;
}

OutcomeLedgerValue outcome_value(const std::string& action_id) {
    OutcomeLedgerValue value;
    value.action_id = action_id;
    value.outcome = "renewed";
    value.value = 1200.0;
    value.currency = "USD";
    value.source = OutcomeSource::Connector;
    value.occurred_at = "2026-10-28T10:00:00.000Z";
    return value;
}

}  // namespace

TEST(AuditLedger, RecordsFullChainWithoutMutatingTheAction) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());

    const auto action = node.record_action(action_value("no_contact"));
    ASSERT_TRUE(action);
    EXPECT_EQ(action->kind(), LedgerEntryKind::Action);
    const nlohmann::json action_before = action->metadata();

    const auto outcome = node.record_outcome(outcome_value(action->id()));
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->kind(), LedgerEntryKind::Outcome);
    EXPECT_EQ(outcome->action_id(), action->id());

    ASSERT_EQ(node.ledger_entry_count(), 2u);
    EXPECT_EQ(action->metadata(), action_before);
    EXPECT_EQ(node.find_ledger_entry(action->id())->metadata(), action_before);
    EXPECT_EQ(node.find_ledger_entry(outcome->id())->action_id(), action->id());

    zetta::query::QueryService query{node};
    const auto rows = query.run("SELECT * FROM system.ledger;", "");
    ASSERT_TRUE(rows.ok()) << rows.error;
    ASSERT_EQ(rows.rows.size(), 2u);
    bool found_action = false;
    bool found_outcome = false;
    for (const auto& row : rows.rows) {
        if (row.at("kind") == "action") {
            found_action = row.at("action") == "no_contact";
        }
        if (row.at("kind") == "outcome") {
            found_outcome = row.at("action_id") == action->id();
        }
    }
    EXPECT_TRUE(found_action);
    EXPECT_TRUE(found_outcome);

    node.stop();
}

TEST(AuditLedger, RejectsAnOutcomeWithoutAnAction) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());

    EXPECT_FALSE(node.record_outcome(outcome_value("missing-action")));
    EXPECT_EQ(node.ledger_entry_count(), 0u);

    ActionLedgerValue invalid = action_value();
    invalid.subject.clear();
    EXPECT_FALSE(node.record_action(std::move(invalid)));
    EXPECT_EQ(node.ledger_entry_count(), 0u);

    node.stop();
}

TEST(AuditLedger, PersistsAcrossRestart) {
    TempDir directory;
    std::string action_id;
    {
        Node node{config_for(directory)};
        ASSERT_TRUE(node.start());
        const auto action = node.record_action(action_value());
        ASSERT_TRUE(action);
        action_id = action->id();
        OutcomeLedgerValue manual_outcome = outcome_value(action_id);
        manual_outcome.source = OutcomeSource::Manual;
        ASSERT_TRUE(node.record_outcome(std::move(manual_outcome)));
        node.stop();
    }

    Node reloaded{config_for(directory)};
    ASSERT_TRUE(reloaded.start());
    ASSERT_EQ(reloaded.ledger_entry_count(), 2u);
    ASSERT_TRUE(reloaded.find_ledger_entry(action_id));
    bool found_outcome = false;
    for (const auto& entry : reloaded.ledger_entries()) {
        if (entry->kind() == LedgerEntryKind::Outcome) {
            found_outcome = entry->action_id() == action_id &&
                            entry->metadata().at("source") == "manual";
        }
    }
    EXPECT_TRUE(found_outcome);
    reloaded.stop();
}

TEST(AuditLedger, ReplicatesActionAndOutcomeToAPeer) {
    TempDir directory_a;
    TempDir directory_b;
    ReplicationManager replication_a{zetta::util::new_uuid()};
    ReplicationManager replication_b{zetta::util::new_uuid()};
    Node node_a{config_for(directory_a), &replication_a};
    Node node_b{config_for(directory_b), &replication_b};
    ASSERT_TRUE(node_a.start());
    ASSERT_TRUE(node_b.start());

    const auto action = node_a.record_action(action_value());
    ASSERT_TRUE(action);
    ASSERT_TRUE(node_a.record_outcome(outcome_value(action->id())));

    replication_b.merge(replication_a.save());
    node_b.tick();

    ASSERT_EQ(node_b.ledger_entry_count(), 2u);
    ASSERT_TRUE(node_b.find_ledger_entry(action->id()));
    bool found_outcome = false;
    for (const auto& entry : node_b.ledger_entries()) {
        if (entry->kind() == LedgerEntryKind::Outcome) {
            found_outcome = entry->action_id() == action->id();
        }
    }
    EXPECT_TRUE(found_outcome);
    EXPECT_EQ(replication_b.snapshot(), replication_a.snapshot());

    node_a.stop();
    node_b.stop();
}

TEST(AuditLedger, SchemaRejectsMalformedEntries) {
    std::string error;
    const nlohmann::json valid = zetta::solutions::to_json(action_value());
    nlohmann::json with_stamp = valid;
    with_stamp["recorded_at"] = "2026-09-28T10:00:03.000Z";
    EXPECT_TRUE(zetta::solutions::validate_audit_ledger_entry(with_stamp, &error))
        << error;

    nlohmann::json missing_version = with_stamp;
    missing_version.erase("policy_version");
    EXPECT_FALSE(zetta::solutions::validate_audit_ledger_entry(
        missing_version, &error));

    nlohmann::json wrong_kind = with_stamp;
    wrong_kind["kind"] = "decision";
    EXPECT_FALSE(zetta::solutions::parse_audit_ledger_entry(wrong_kind, &error));
    EXPECT_FALSE(error.empty());
}
