#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

using zetta::pipeline::Check;
using zetta::pipeline::CheckResult;
using zetta::pipeline::CheckStatus;
using zetta::pipeline::CorrectnessGate;
using zetta::pipeline::make_check;
using zetta::pipeline::NonEmptyGraphCheck;
using zetta::pipeline::ValidationReport;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;

namespace {

// A graph with a single step — enough to pass the trivial NonEmptyGraphCheck.
ExecutionGraph one_step_graph() {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load_orders");
    return graph;
}

}  // namespace

// ---- the gate runner --------------------------------------------------------

TEST(CorrectnessGate, EmptyGatePassesAndAllowsDeploy) {
    const CorrectnessGate gate;  // no checks registered
    EXPECT_EQ(gate.check_count(), 0u);

    const ValidationReport report = gate.run(one_step_graph());
    EXPECT_TRUE(report.checks.empty());
    EXPECT_EQ(report.verdict(), CheckStatus::Pass);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);  // nothing failed → deploy allowed
}

TEST(CorrectnessGate, RunsRegisteredChecksInOrder) {
    CorrectnessGate gate;
    gate.add_check(make_check("first", [](const ExecutionGraph&) {
            return CheckResult{"first", CheckStatus::Pass, "ok"};
        }))
        .add_check(make_check("second", [](const ExecutionGraph&) {
            return CheckResult{"second", CheckStatus::Pass, "ok"};
        }));
    EXPECT_EQ(gate.check_count(), 2u);

    const ValidationReport report = gate.run(one_step_graph());
    ASSERT_EQ(report.checks.size(), 2u);
    EXPECT_EQ(report.checks[0].name, "first");
    EXPECT_EQ(report.checks[1].name, "second");
}

TEST(CorrectnessGate, IgnoresANullCheck) {
    CorrectnessGate gate;
    gate.add_check(nullptr);
    EXPECT_EQ(gate.check_count(), 0u);
}

// ---- RULE-1: a failing check blocks deploy with a clear reason --------------

TEST(CorrectnessGate, FailingCheckBlocksDeployWithAReason) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ExecutionGraph empty_graph;  // nothing to deploy

    const ValidationReport report = gate.run(empty_graph);

    // Only the non-empty check fails; the other checks have no matching steps.
    ASSERT_EQ(report.checks.size(), 78u);
    EXPECT_EQ(report.checks[0].name, "non_empty_graph");
    EXPECT_EQ(report.checks[0].status, CheckStatus::Fail);
    EXPECT_FALSE(report.checks[0].reason.empty());  // a clear reason, not blank
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);  // RULE-1: no deploy without a passing gate
}

TEST(CorrectnessGate, PassingGraphAllowsDeploy) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();

    const ValidationReport report = gate.run(one_step_graph());

    // All default checks pass an unannotated single-step graph.
    ASSERT_EQ(report.checks.size(), 78u);
    EXPECT_EQ(report.verdict(), CheckStatus::Pass);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
}

TEST(CorrectnessGate, DefaultGateShipsTheBaselineChecks) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    ASSERT_EQ(gate.check_count(), 78u);
    EXPECT_EQ(gate.checks()[0]->name(), "non_empty_graph");
    EXPECT_EQ(gate.checks()[1]->name(), "schema_type");
    EXPECT_EQ(gate.checks()[2]->name(), "data_leakage");
    EXPECT_EQ(gate.checks()[3]->name(), "snapshot_lineage");
    EXPECT_EQ(gate.checks()[4]->name(), "s1_point_in_time");
    EXPECT_EQ(gate.checks()[5]->name(), "s1_label_window");
    EXPECT_EQ(gate.checks()[6]->name(), "s1_temporal_validation");
    EXPECT_EQ(gate.checks()[7]->name(), "s1_group_split");
    EXPECT_EQ(gate.checks()[8]->name(), "s1_censoring");
    EXPECT_EQ(gate.checks()[9]->name(), "s1_calibration");
    EXPECT_EQ(gate.checks()[10]->name(), "s1_entity_resolution");
    EXPECT_EQ(gate.checks()[11]->name(), "s1_leakage_review");
    EXPECT_EQ(gate.checks()[12]->name(), "s1_level_composition");
    EXPECT_EQ(gate.checks()[13]->name(), "s2_point_in_time_conversion");
    EXPECT_EQ(gate.checks()[14]->name(), "s2_post_qualification_leakage");
    EXPECT_EQ(gate.checks()[15]->name(), "s2_capacity_routing");
    EXPECT_EQ(gate.checks()[16]->name(), "s3_short_tenure_censoring");
    EXPECT_EQ(gate.checks()[17]->name(), "s3_value_horizon");
    EXPECT_EQ(gate.checks()[18]->name(), "s3_entity_accounting");
    EXPECT_EQ(gate.checks()[19]->name(), "s4_regulatory_contact");
    EXPECT_EQ(gate.checks()[20]->name(), "s4_vulnerable_customer");
    EXPECT_EQ(gate.checks()[21]->name(), "s4_promise_to_pay_point_in_time");
    EXPECT_EQ(gate.checks()[22]->name(), "s12_no_automated_denial");
    EXPECT_EQ(gate.checks()[23]->name(), "s12_fairness_review");
    EXPECT_EQ(gate.checks()[24]->name(), "s12_document_confidence");
    EXPECT_EQ(gate.checks()[25]->name(), "s12_level_composition");
    EXPECT_EQ(gate.checks()[26]->name(), "s5_rolling_origin");
    EXPECT_EQ(gate.checks()[27]->name(), "s5_future_covariates");
    EXPECT_EQ(gate.checks()[28]->name(), "s5_hierarchy_consistency");
    EXPECT_EQ(gate.checks()[29]->name(), "s5_level_composition");
    EXPECT_EQ(gate.checks()[30]->name(), "s6_service_budget_constraints");
    EXPECT_EQ(gate.checks()[31]->name(), "s6_lead_time_realism");
    EXPECT_EQ(gate.checks()[32]->name(), "s6_order_approval");
    EXPECT_EQ(gate.checks()[33]->name(), "s6_level_composition");
    EXPECT_EQ(gate.checks()[34]->name(), "s7_causal_humility");
    EXPECT_EQ(gate.checks()[35]->name(), "s7_alert_budget");
    EXPECT_EQ(gate.checks()[36]->name(), "s7_seasonality_baseline");
    EXPECT_EQ(gate.checks()[37]->name(), "s8_censoring");
    EXPECT_EQ(gate.checks()[38]->name(), "s8_sensor_drift");
    EXPECT_EQ(gate.checks()[39]->name(), "s8_post_failure_maintenance_leakage");
    EXPECT_EQ(gate.checks()[40]->name(), "s9_consent_suppression");
    EXPECT_EQ(gate.checks()[41]->name(), "s9_contact_caps");
    EXPECT_EQ(gate.checks()[42]->name(), "s9_uplift_over_propensity");
    EXPECT_EQ(gate.checks()[43]->name(), "s10_label_delay");
    EXPECT_EQ(gate.checks()[44]->name(), "s10_declined_selection_bias");
    EXPECT_EQ(gate.checks()[45]->name(), "s10_request_time_feature_parity");
    EXPECT_EQ(gate.checks()[46]->name(), "s10_level_composition");
    EXPECT_EQ(gate.checks()[47]->name(), "s11_logged_propensity");
    EXPECT_EQ(gate.checks()[48]->name(), "s11_contact_consent");
    EXPECT_EQ(gate.checks()[49]->name(), "s11_propensity_interpretation");
    EXPECT_EQ(gate.checks()[50]->name(), "temporal_validation");
    EXPECT_EQ(gate.checks()[51]->name(), "entity_resolution_profile");
    EXPECT_EQ(gate.checks()[52]->name(), "segmentation");
    EXPECT_EQ(gate.checks()[53]->name(), "forecast_correctness");
    EXPECT_EQ(gate.checks()[54]->name(), "survival_censoring");
    EXPECT_EQ(gate.checks()[55]->name(), "causal_uplift");
    EXPECT_EQ(gate.checks()[56]->name(), "anomaly_correctness");
    EXPECT_EQ(gate.checks()[57]->name(), "next_best_action_correctness");
    EXPECT_EQ(gate.checks()[58]->name(), "rank_correctness");
    EXPECT_EQ(gate.checks()[59]->name(), "language_grounding");
    EXPECT_EQ(gate.checks()[60]->name(), "language_training_access");
    EXPECT_EQ(gate.checks()[61]->name(), "document_review");
    EXPECT_EQ(gate.checks()[62]->name(), "graph_edge_time");
    EXPECT_EQ(gate.checks()[63]->name(), "vision_source_split");
    EXPECT_EQ(gate.checks()[64]->name(), "speech_consent");
    EXPECT_EQ(gate.checks()[65]->name(), "speech_pii_redaction");
    EXPECT_EQ(gate.checks()[66]->name(), "speech_local_audio");
    EXPECT_EQ(gate.checks()[67]->name(), "bandit_policy");
    EXPECT_EQ(gate.checks()[68]->name(), "optimization_contract");
    EXPECT_EQ(gate.checks()[69]->name(), "uncertainty_validation");
    EXPECT_EQ(gate.checks()[70]->name(), "ensemble_out_of_fold");
    EXPECT_EQ(gate.checks()[71]->name(), "label_efficiency_gold_isolation");
    EXPECT_EQ(gate.checks()[72]->name(), "privacy_budget");
    EXPECT_EQ(gate.checks()[73]->name(), "streaming_correctness");
    EXPECT_EQ(gate.checks()[74]->name(), "entity_resolution");
    EXPECT_EQ(gate.checks()[75]->name(), "b4_per_field_confidence");
    EXPECT_EQ(gate.checks()[76]->name(), "b4_review_queue");
    EXPECT_EQ(gate.checks()[77]->name(), "b4_validated_workflow");
}

// ---- aggregation policy: warnings pass, any failure blocks ------------------

TEST(CorrectnessGate, WarningDoesNotBlockDeploy) {
    CorrectnessGate gate;
    gate.add_check(make_check("style", [](const ExecutionGraph&) {
        return CheckResult{"style", CheckStatus::Warn, "borderline naming"};
    }));

    const ValidationReport report = gate.run(one_step_graph());
    EXPECT_EQ(report.verdict(), CheckStatus::Warn);
    EXPECT_TRUE(report.passed());        // a warning still passes
    EXPECT_FALSE(report.block_deploy);   // ...and does not block deploy
}

TEST(CorrectnessGate, AnyFailureBlocksDeployAmongManyChecks) {
    CorrectnessGate gate;
    gate.add_check(make_check("a", [](const ExecutionGraph&) {
            return CheckResult{"a", CheckStatus::Pass, ""};
        }))
        .add_check(make_check("b", [](const ExecutionGraph&) {
            return CheckResult{"b", CheckStatus::Warn, "meh"};
        }))
        .add_check(make_check("c", [](const ExecutionGraph&) {
            return CheckResult{"c", CheckStatus::Fail, "broken"};
        }));

    const ValidationReport report = gate.run(one_step_graph());
    EXPECT_EQ(report.verdict(), CheckStatus::Fail);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);
}

// ---- make_check keeps the reported name in lockstep with registration -------

TEST(CorrectnessGate, MakeCheckStampsTheRegisteredName) {
    // The callable reports a *different* name; make_check overwrites it with the
    // registered name so the two can never drift.
    auto check = make_check("registered_name", [](const ExecutionGraph&) {
        return CheckResult{"some_other_name", CheckStatus::Pass, "ok"};
    });
    EXPECT_EQ(check->name(), "registered_name");

    const ExecutionGraph graph;
    const CheckResult result = check->evaluate(graph);
    EXPECT_EQ(result.name, "registered_name");
}

// ---- the produced report is a well-formed ValidationReport ------------------

TEST(CorrectnessGate, ProducedReportRoundTripsThroughJson) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    const ValidationReport report = gate.run(ExecutionGraph{});  // a failing run

    const nlohmann::json document = report;  // to_json ADL hook
    EXPECT_EQ(document.at("verdict"), "fail");
    EXPECT_EQ(document.at("block_deploy"), true);

    const auto parsed = zetta::pipeline::validation_report_from_json(document);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, report);
}
