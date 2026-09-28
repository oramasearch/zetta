#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/solutions/audit_ledger.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/solutions/roi_readout.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::solutions::ActionActorKind;
using zetta::solutions::ActionLedgerValue;
using zetta::solutions::ExperimentAllocation;
using zetta::solutions::ExperimentAssignmentRecord;
using zetta::solutions::ExperimentTier;
using zetta::solutions::ExperimentValue;
using zetta::solutions::OutcomeLedgerValue;
using zetta::solutions::OutcomeSource;
using zetta::solutions::RoiModel;
using zetta::solutions::RoiReadoutSpec;
using zetta::solutions::RoiReadoutStore;

ExperimentValue experiment_value() {
    ExperimentValue value;
    value.name = "renewal-effect";
    value.seed = 1108;
    value.allocations = {
        ExperimentAllocation{"holdout", ExperimentTier::Holdout, 0.5},
        ExperimentAllocation{"treatment", ExperimentTier::Treatment, 0.5},
    };
    return value;
}

ActionLedgerValue action_value(
    const ExperimentAssignmentRecord& assignment,
    std::string decision = "retain") {
    ActionLedgerValue value;
    value.subject = assignment.value.entity;
    value.score_id = "score-" + assignment.value.entity;
    value.score = 0.8;
    value.decision = std::move(decision);
    value.action = assignment.value.tier == ExperimentTier::Holdout
                       ? "no_contact"
                       : "retention_offer";
    value.actor_kind = ActionActorKind::Agent;
    value.actor = "retention-agent";
    value.model_version = "renewal-v3";
    value.policy_version = "retention-policy-v2";
    value.agent_version = "agent-v4";
    value.cell = assignment.value.allocation;
    value.cost = 1.0;
    value.currency = "USD";
    value.score_at = "2026-09-01T00:00:00.000Z";
    value.decision_at = "2026-09-01T00:00:01.000Z";
    value.action_at = "2026-09-01T00:00:02.000Z";
    return value;
}

void record_response(
    zetta::Node& node,
    const ExperimentAssignmentRecord& assignment,
    double response,
    std::string decision = "retain") {
    const auto action = node.record_action(
        action_value(assignment, std::move(decision)));
    ASSERT_TRUE(action);
    OutcomeLedgerValue outcome;
    outcome.action_id = action->id();
    outcome.outcome = "renewed";
    outcome.value = response;
    outcome.currency = "USD";
    outcome.source = OutcomeSource::Connector;
    outcome.occurred_at = "2026-09-30T00:00:00.000Z";
    ASSERT_TRUE(node.record_outcome(std::move(outcome)));
}

void create_valid_graph(zetta::Node& node) {
    const auto project = node.create_project("renewal-solution");
    ASSERT_TRUE(project);
    zetta::ir::ExecutionGraph graph;
    ASSERT_TRUE(graph.add_node(zetta::ir::StepKind::Connect, "source"));
    ASSERT_TRUE(project->create_graph("production", std::move(graph)));
}

RoiReadoutSpec readout_spec(const std::string& experiment_id) {
    RoiReadoutSpec spec;
    spec.solution = "renewal-risk";
    spec.project = "renewal-solution";
    spec.graph = "production";
    spec.experiment = experiment_id;
    spec.roi_model = RoiModel{"renewed", 100.0, 200.0, "USD"};
    return spec;
}

}  // namespace

TEST(RoiReadout, KnownSyntheticEffectFallsInsideItsConfidenceInterval) {
    zetta::test::TempDir directory;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    create_valid_graph(node);

    const auto experiment = node.experiment_manager().create(experiment_value());
    ASSERT_TRUE(experiment);
    std::uint64_t holdout_count = 0;
    std::uint64_t treatment_count = 0;
    std::uint64_t entity_index = 0;
    while (holdout_count < 40 || treatment_count < 40) {
        const auto assignment = node.experiment_manager().assign(
            experiment->id,
            "account-" + std::to_string(entity_index));
        ASSERT_TRUE(assignment);
        ++entity_index;
        if (assignment->value.tier == ExperimentTier::Holdout &&
            holdout_count < 40) {
            const double response = holdout_count % 2 == 0 ? 0.0 : 1.0;
            record_response(node, *assignment, response);
            ++holdout_count;
        }
        if (assignment->value.tier == ExperimentTier::Treatment &&
            treatment_count < 40) {
            const double response = treatment_count % 2 == 0 ? 0.2 : 1.2;
            record_response(node, *assignment, response);
            ++treatment_count;
        }
    }

    std::string error;
    RoiReadoutStore store{replication};
    const auto first = store.configure(
        readout_spec(experiment->id),
        "owner",
        &error);
    ASSERT_TRUE(first) << error;
    RoiReadoutSpec updated_spec = readout_spec(experiment->id);
    const auto second = store.configure(
        std::move(updated_spec),
        "owner",
        &error);
    ASSERT_TRUE(second) << error;
    EXPECT_EQ(second->version, 2u);

    zetta::solutions::MonitorSpec monitor_spec;
    monitor_spec.solution = "renewal-risk";
    monitor_spec.guardrails = {{"cost", 0.0, 100.0}};
    const auto monitor = node.create_monitor(std::move(monitor_spec), &error);
    ASSERT_TRUE(monitor) << error;
    ASSERT_TRUE(monitor->observe_guardrail("cost", 120.0).ok);

    const auto readout = zetta::solutions::generate_roi_readout(
        node,
        replication,
        "renewal-risk",
        &error);
    ASSERT_TRUE(readout) << error;
    EXPECT_EQ(readout->definition.version, 2u);
    EXPECT_TRUE(readout->correctness_report.passed());
    ASSERT_TRUE(readout->incrementality.claimable);
    ASSERT_EQ(readout->incrementality.cells.size(), 1u);
    const auto& estimate = readout->incrementality.cells.front().estimate;
    ASSERT_TRUE(estimate);
    EXPECT_NEAR(estimate->effect, 0.2, 1e-12);
    EXPECT_LE(estimate->confidence_interval_lower, 0.2);
    EXPECT_GE(estimate->confidence_interval_upper, 0.2);
    ASSERT_TRUE(readout->roi.claimable);
    EXPECT_NEAR(*readout->roi.incremental_outcomes, 8.0, 1e-12);
    EXPECT_NEAR(*readout->roi.gross_value, 800.0, 1e-9);
    EXPECT_NEAR(*readout->roi.net_value, 600.0, 1e-9);
    EXPECT_NEAR(*readout->roi.return_on_investment, 3.0, 1e-12);
    EXPECT_TRUE(readout->guardrails.at("available").get<bool>());
    EXPECT_EQ(readout->guardrails.at("active_breaches").size(), 1u);

    const nlohmann::json output = zetta::solutions::to_json(*readout);
    for (const auto& item : output.at("audit_extract")) {
        if (item.at("kind") == "outcome") {
            EXPECT_FALSE(item.contains("value"));
        }
    }
    node.stop();
}

TEST(RoiReadout, MissingHoldoutOutcomesRefusesIncrementalityClaim) {
    zetta::test::TempDir directory;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    create_valid_graph(node);

    const auto experiment = node.experiment_manager().create(experiment_value());
    ASSERT_TRUE(experiment);
    std::uint64_t treatment_count = 0;
    for (std::uint64_t index = 0; treatment_count < 12; ++index) {
        const auto assignment = node.experiment_manager().assign(
            experiment->id,
            "treatment-only-" + std::to_string(index));
        ASSERT_TRUE(assignment);
        if (assignment->value.tier != ExperimentTier::Treatment) {
            continue;
        }
        record_response(node, *assignment, 1.0);
        ++treatment_count;
    }

    std::string error;
    RoiReadoutStore store{replication};
    ASSERT_TRUE(store.configure(readout_spec(experiment->id), "owner", &error))
        << error;
    const auto readout = zetta::solutions::generate_roi_readout(
        node,
        replication,
        "renewal-risk",
        &error);
    ASSERT_TRUE(readout) << error;
    EXPECT_FALSE(readout->incrementality.claimable);
    EXPECT_NE(
        readout->incrementality.reason.find("no holdout outcomes"),
        std::string::npos);
    EXPECT_FALSE(readout->roi.claimable);
    EXPECT_FALSE(readout->roi.incremental_outcomes);
    node.stop();
}

TEST(RoiReadout, ReportsPolicyApprovalsAndExperimentOverrides) {
    zetta::test::TempDir directory;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    ASSERT_TRUE(node.start());
    create_valid_graph(node);
    const auto experiment = node.experiment_manager().create(experiment_value());
    ASSERT_TRUE(experiment);

    std::optional<ExperimentAssignmentRecord> treatment;
    std::optional<ExperimentAssignmentRecord> holdout;
    for (std::uint64_t index = 0; !treatment || !holdout; ++index) {
        const auto assignment = node.experiment_manager().assign(
            experiment->id,
            "operations-" + std::to_string(index));
        ASSERT_TRUE(assignment);
        if (assignment->value.tier == ExperimentTier::Treatment && !treatment) {
            treatment = assignment;
        }
        if (assignment->value.tier == ExperimentTier::Holdout && !holdout) {
            holdout = assignment;
        }
    }
    ASSERT_TRUE(node.experiment_manager().assign(
        experiment->id,
        holdout->value.entity,
        true,
        "required notice",
        "operator"));

    zetta::solutions::DecisionPolicyStore policy_store{replication, &node};
    std::string error;
    const auto policy = policy_store.create(
        "approval-policy",
        nlohmann::json{{"autonomy", "A1"}},
        "owner",
        &error);
    ASSERT_TRUE(policy) << error;
    const auto pending = policy_store.decide(
        policy->id,
        nlohmann::json{{"entity_id", treatment->value.entity},
                       {"score", 0.9},
                       {"action", "retention_offer"}},
        "agent",
        &error);
    ASSERT_TRUE(pending) << error;
    const auto approved = policy_store.approve(pending->id, "reviewer", &error);
    ASSERT_TRUE(approved) << error;
    record_response(node, *treatment, 1.0, approved->id);

    RoiReadoutStore store{replication};
    ASSERT_TRUE(store.configure(readout_spec(experiment->id), "owner", &error))
        << error;
    const auto readout = zetta::solutions::generate_roi_readout(
        node,
        replication,
        "renewal-risk",
        &error);
    ASSERT_TRUE(readout) << error;
    EXPECT_EQ(readout->agent_operations.actions, 1u);
    EXPECT_EQ(readout->agent_operations.autonomy_a1, 1u);
    EXPECT_EQ(readout->agent_operations.approvals, 1u);
    EXPECT_EQ(readout->agent_operations.overrides, 1u);
    node.stop();
}

TEST(RoiReadout, DefinitionBoundaryRejectsInventedOrInvalidEconomics) {
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    RoiReadoutStore store{replication};
    RoiReadoutSpec invalid = readout_spec("experiment-id");
    invalid.roi_model.currency.clear();
    std::string error;
    EXPECT_FALSE(store.configure(std::move(invalid), "owner", &error));
    EXPECT_NE(error.find("currency"), std::string::npos);
    EXPECT_TRUE(store.versions("renewal-risk").empty());
}
