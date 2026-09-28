#include <gtest/gtest.h>
#include <algorithm>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "zetta/ml/solution.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include <nlohmann/json-schema.hpp>

namespace {

zetta::ir::ExecutionGraph bound_s1_graph(int level = 1) {
    const zetta::ml::Solution* solution = zetta::ml::find_solution("S-1");
    if (solution == nullptr) {
        return {};
    }
    zetta::ml::ComposedSolution composed =
        zetta::ml::compose_solution(*solution, level, {});
    if (!composed.ok()) {
        return {};
    }
    for (const auto& source :
         composed.graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!source->attributes.contains("solution_stage")) {
            continue;
        }
        source->attributes["source_kind"] = "snapshot";
        source->attributes["snapshot_as_of"] = "2024-01-01T00:00:00Z";
    }
    const auto manifest = composed.graph.find_node("solution_manifest");
    manifest->attributes["foundation"] = nlohmann::json::object();
    manifest->attributes["s1_correctness"] = {
        {"prediction_time", "2024-01-01T00:00:00Z"},
        {"feature_window_end", "2024-01-01T00:00:00Z"},
        {"label_window_start", "2024-01-02T00:00:00Z"},
        {"blackout_gap_seconds", 86400.0},
        {"validation",
         {{"split", "out_of_time"},
          {"rolling_windows", 3},
          {"stable_across_windows", true},
          {"group_column", "customer_id"},
          {"group_disjoint", true}}},
        {"labels", {{"fully_observed", true}}},
        {"sampling",
         {{"used", false},
          {"calibration", "platt"},
          {"calibration_disjoint", true}}},
        {"entity_resolution",
         {{"methodology", "M-2"},
          {"actionable_unit", "customer_id"},
          {"duplicate_policy", "refuse"}}},
        {"leaky_feature_blocklist", {"churned", "churn_date"}},
        {"feature_names", {"usage", "tenure"}},
        {"too_good_auc_threshold", 0.95}};
    for (const auto& model :
         composed.graph.nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (model->attributes.value("model_kind", std::string{}) == "uplift") {
            model->attributes["randomized_cycles"] = 1;
            model->attributes["sleeping_dogs_action"] = "no_contact";
            model->attributes["positive_uplift_only"] = true;
        }
    }
    return composed.graph;
}

zetta::ir::ExecutionGraph bound_s2_graph(int level = 1) {
    const zetta::ml::Solution* solution = zetta::ml::find_solution("S-2");
    if (solution == nullptr) {
        return {};
    }
    zetta::ml::ComposedSolution composed =
        zetta::ml::compose_solution(*solution, level, {});
    if (!composed.ok()) {
        return {};
    }
    for (const auto& source :
         composed.graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!source->attributes.contains("solution_stage")) {
            continue;
        }
        source->attributes["source_kind"] = "snapshot";
        source->attributes["snapshot_as_of"] = "2024-01-01T00:00:00Z";
    }
    const auto manifest = composed.graph.find_node("solution_manifest");
    manifest->attributes["foundation"] = nlohmann::json::object();
    manifest->attributes["s2_correctness"] = {
        {"prediction_time", "2024-01-01T00:00:00Z"},
        {"feature_window_end", "2024-01-01T00:00:00Z"},
        {"label_window_start", "2024-01-02T00:00:00Z"},
        {"blackout_gap_seconds", 86400.0},
        {"labels",
         {{"label_column", "converted"}, {"fully_observed", true}}},
        {"post_qualification_blocklist",
         {"qualified_at", "opportunity_stage", "closed_won"}},
        {"feature_names", {"engagement", "company_size"}},
        {"routing",
         {{"capacity_aware", true},
          {"owner_column", "owner"},
          {"territory_column", "territory"},
          {"capacity_column", "rep_capacity_available"},
          {"over_capacity_action", "no_contact"}}}};
    return composed.graph;
}

zetta::ir::ExecutionGraph bound_s3_graph(int level = 1) {
    const zetta::ml::Solution* solution = zetta::ml::find_solution("S-3");
    if (solution == nullptr) {
        return {};
    }
    zetta::ml::ComposedSolution composed = zetta::ml::compose_solution(
        *solution, level,
        {{"M-1", {{"label_column", "future_value"},
                   {"value_horizon_days", 365}}}});
    if (!composed.ok()) {
        return {};
    }
    for (const auto& source :
         composed.graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!source->attributes.contains("solution_stage")) {
            continue;
        }
        source->attributes["source_kind"] = "snapshot";
        source->attributes["snapshot_as_of"] =
            "2026-09-01T00:00:00Z";
    }
    const auto manifest = composed.graph.find_node("solution_manifest");
    manifest->attributes["foundation"] = nlohmann::json::object();
    manifest->attributes["s3_correctness"] = {
        {"value_horizon_days", 365},
        {"value_label", "future_value"},
        {"short_tenure",
         {{"tenure_column", "tenure_days"},
          {"minimum_observed_days", 90},
          {"handling", "right_censor"},
          {"applied_to_training_labels", true}}},
        {"entity_accounting",
         {{"methodology", "M-2"},
          {"entity_column", "entity_id"},
          {"aggregation", "one_value_per_entity"},
          {"duplicate_policy", "refuse"}}}};
    return composed.graph;
}


zetta::ir::ExecutionGraph bound_s4_graph(int level = 1) {
    const zetta::ml::Solution* solution = zetta::ml::find_solution("S-4");
    if (solution == nullptr) {
        return {};
    }
    zetta::ml::ComposedSolution composed =
        zetta::ml::compose_solution(*solution, level, {});
    if (!composed.ok()) {
        return {};
    }
    for (const auto& source :
         composed.graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!source->attributes.contains("solution_stage")) {
            continue;
        }
        source->attributes["source_kind"] = "snapshot";
        source->attributes["snapshot_as_of"] = "2024-01-01T00:00:00Z";
    }
    const auto manifest = composed.graph.find_node("solution_manifest");
    manifest->attributes["foundation"] = nlohmann::json::object();
    manifest->attributes["s4_correctness"] = {
        {"prediction_time", "2024-01-01T00:00:00Z"},
        {"policy", solution->templates.policy},
        {"regulatory_contact",
         {{"eligibility_field", "collections_eligible"},
          {"contact_allowed_field", "regulatory_contact_allowed"},
          {"contact_hours_field", "within_contact_hours"},
          {"contact_cap", 6},
          {"frequency_cap",
           {{"max_actions", 1}, {"window_seconds", 604800}}}}},
        {"vulnerable_customer",
         {{"field", "vulnerable"},
          {"excluded_when_true", true}}},
        {"promise_to_pay_labels",
         {{"semantics", "promise_to_pay"},
          {"label_column", "promise_to_pay"},
          {"observation_time_column", "promise_to_pay_at"},
          {"snapshot_as_of", "2024-01-01T00:00:00Z"},
          {"point_in_time", true},
          {"future_updates_excluded", true}}}};
    return composed.graph;
}

zetta::ir::ExecutionGraph bound_s12_graph(int level = 1) {
    const zetta::ml::Solution* solution = zetta::ml::find_solution("S-12");
    if (solution == nullptr) {
        return {};
    }
    zetta::ml::ComposedSolution composed = zetta::ml::compose_solution(
        *solution, level,
        {{"source", "claims"},
         {"table", "fnol"},
         {"M-11", {{"fields", {"claim_number", "loss_date", "loss_amount"}},
                    {"review_threshold", 0.8}}}});
    if (!composed.ok()) {
        return {};
    }
    for (const auto& source :
         composed.graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!source->attributes.contains("solution_stage")) {
            continue;
        }
        source->attributes["source_kind"] = "snapshot";
        source->attributes["snapshot_as_of"] =
            "2026-09-01T00:00:00.000Z";
    }
    const auto manifest = composed.graph.find_node("solution_manifest");
    manifest->attributes["foundation"] = nlohmann::json::object();
    manifest->attributes["s12_correctness"] = {
        {"policy", solution->templates.policy},
        {"fairness_review",
         {{"cohort_review_required", true},
          {"referral_rate_review_required", true},
          {"owner", "claims-governance"},
          {"blocked_feature_names", {"race", "ethnicity", "sex", "gender"}},
          {"feature_names", {"claim_amount", "prior_claims"}}}},
        {"document_confidence",
         {{"minimum_review_threshold", 0.8},
          {"low_confidence_routes_to_review", true},
          {"fast_track_requires_clear", true}}}};
    return composed.graph;
}

zetta::ir::ExecutionGraph bound_s6_graph(int level = 1) {
    const zetta::ml::Solution* solution = zetta::ml::find_solution("S-6");
    if (solution == nullptr) {
        return {};
    }
    zetta::ml::ComposedSolution composed =
        zetta::ml::compose_solution(*solution, level,
                                    {{"M-4", {{"horizon", 7}}}});
    if (!composed.ok()) {
        return {};
    }
    const auto manifest = composed.graph.find_node("solution_manifest");
    manifest->attributes["foundation"] = nlohmann::json::object();
    manifest->attributes["s6_correctness"] = {
        {"service_level_target", 0.95},
        {"budget_limit", 10000.0},
        {"required_constraints", {"service_level", "budget"}},
        {"lead_time_days", 7.0},
        {"lead_time_source", "snapshot"},
        {"forecast_horizon_days", 7.0},
        {"automatic_release_max_quantity", 100.0},
        {"hard_order_max_quantity", 500.0},
        {"approval_field", "order_quantity"},
        {"approval_above", 100.0},
        {"hard_order_bound_enforced", true},
        {"approval_required", true},
        {"level_action", level <= 1 ? "draft_order"
                          : level >= 4 ? "release_order"
                                       : "erp_replenishment_task"},
        {"kill_switch_configured", true}};
    return composed.graph;
}

const zetta::pipeline::CheckResult* check_result(
    const zetta::pipeline::ValidationReport& report,
    const std::string& name) {
    for (const auto& check : report.checks) {
        if (check.name == name) {
            return &check;
        }
    }
    return nullptr;
}

void expect_s1_failure(const zetta::ir::ExecutionGraph& graph,
                       const std::string& name) {
    const auto report = zetta::pipeline::default_gate().run(graph);
    const auto* check = check_result(report, "solution/" + name);
    ASSERT_NE(check, nullptr) << nlohmann::json(report).dump(2);
    EXPECT_EQ(check->status, zetta::pipeline::CheckStatus::Fail)
        << check->reason;
}

void expect_s2_failure(const zetta::ir::ExecutionGraph& graph,
                       const std::string& name) {
    const auto report = zetta::pipeline::default_gate().run(graph);
    const auto* check = check_result(report, "solution/" + name);
    ASSERT_NE(check, nullptr) << nlohmann::json(report).dump(2);
    EXPECT_EQ(check->status, zetta::pipeline::CheckStatus::Fail)
        << check->reason;
}

void expect_s3_failure(const zetta::ir::ExecutionGraph& graph,
                       const std::string& name) {
    const auto report = zetta::pipeline::default_gate().run(graph);
    const auto* check = check_result(report, "solution/" + name);
    ASSERT_NE(check, nullptr) << nlohmann::json(report).dump(2);
    EXPECT_EQ(check->status, zetta::pipeline::CheckStatus::Fail)
        << check->reason;
}


void expect_s4_failure(const zetta::ir::ExecutionGraph& graph,
                       const std::string& name) {
    const auto report = zetta::pipeline::default_gate().run(graph);
    const auto* check = check_result(report, "solution/" + name);
    ASSERT_NE(check, nullptr) << nlohmann::json(report).dump(2);
    EXPECT_EQ(check->status, zetta::pipeline::CheckStatus::Fail)
        << check->reason;
}

void expect_s12_failure(const zetta::ir::ExecutionGraph& graph,
                        const std::string& name) {
    const auto report = zetta::pipeline::default_gate().run(graph);
    const auto* check = check_result(report, "solution/" + name);
    ASSERT_NE(check, nullptr) << nlohmann::json(report).dump(2);
    EXPECT_EQ(check->status, zetta::pipeline::CheckStatus::Fail)
        << check->reason;
}

TEST(SolutionRegistry, ListsTwelveCatalogueModulesAndEightSketches) {
    ASSERT_EQ(zetta::ml::solutions().size(), 20u);
    std::set<std::string> ids;
    std::size_t catalogue = 0;
    std::size_t sketches = 0;
    for (const zetta::ml::Solution& solution : zetta::ml::solutions()) {
        EXPECT_TRUE(ids.insert(solution.id).second) << solution.id;
        EXPECT_FALSE(solution.promise.empty());
        EXPECT_FALSE(solution.use_case.empty());
        if (solution.status == "sketch") {
            ++sketches;
            EXPECT_FALSE(solution.promotion_condition.empty());
            EXPECT_FALSE(solution.required_data_roles.empty());
        } else {
            ++catalogue;
            for (const auto& missing : solution.missing_components) {
                EXPECT_TRUE(missing.id.starts_with("M-"));
            }
            nlohmann::json_schema::json_validator validator(
                zetta::solutions::decision_policy_config_schema());
            EXPECT_NO_THROW(validator.validate(solution.templates.policy)) << solution.id;
            EXPECT_FALSE(solution.templates.policy.at("kill_switch").get<bool>());
            EXPECT_EQ(solution.templates.minimal,
                      solution.id != "S-1" && solution.id != "S-2" &&
                          solution.id != "S-3" && solution.id != "S-4" &&
                          solution.id != "S-6" && solution.id != "S-12");
            EXPECT_FALSE(solution.composition.empty());
            EXPECT_FALSE(solution.action_patterns.empty());
            EXPECT_FALSE(solution.latency_classes.empty());
            EXPECT_FALSE(solution.autonomy_defaults.empty());
            EXPECT_FALSE(solution.required_data_roles.empty());
            EXPECT_FALSE(solution.decision_policy_template.empty());
            EXPECT_FALSE(solution.measurement_design.empty());
            EXPECT_FALSE(solution.guardrails.empty());
            EXPECT_EQ(solution.capability_levels.size(), 4u);
        }
    }
    EXPECT_EQ(catalogue, 12u);
    EXPECT_EQ(sketches, 8u);
}

TEST(SolutionRegistry, UsesPortfolioMethodologyNumbering) {
    const std::vector<std::pair<std::string, std::vector<std::string>>> expected = {
        {"S-1", {"M-1", "M-2", "M-3", "M-5", "M-6", "M-8"}},
        {"S-2", {"M-1", "M-2", "M-3", "M-6"}},
        {"S-3", {"M-1", "M-2", "M-3", "M-5", "M-6"}},
        {"S-4", {"M-1", "M-2", "M-3", "M-6"}},
        {"S-5", {"M-4", "M-2", "M-3", "M-5"}},
        {"S-6", {"M-4", "M-16", "M-5"}},
        {"S-7", {"M-7", "M-1", "M-3", "M-4"}},
        {"S-8", {"M-5", "M-1", "M-2", "M-7"}},
        {"S-9", {"M-1", "M-3", "M-2", "M-6", "M-8", "M-10"}},
        {"S-10", {"M-1", "M-2", "M-12", "M-5"}},
        {"S-11", {"M-8", "M-1", "M-6", "M-2", "M-15"}},
        {"S-12", {"M-1", "M-2", "M-11", "M-3"}},
    };
    for (const auto& [solution_id, methodology_ids] : expected) {
        const zetta::ml::Solution* solution =
            zetta::ml::find_solution(solution_id);
        ASSERT_NE(solution, nullptr) << solution_id;
        std::vector<std::string> actual;
        for (const zetta::ml::SolutionMethodology& methodology :
             solution->composition) {
            actual.push_back(methodology.id);
        }
        EXPECT_EQ(actual, methodology_ids) << solution_id;
    }
}

TEST(SolutionRegistry, ComposesEverySelectedMethodologyThroughItsCallback) {
    const zetta::ml::Solution* churn = zetta::ml::find_solution("S-1");
    ASSERT_NE(churn, nullptr);
    const zetta::ml::ComposedSolution composed = zetta::ml::compose_solution(
        *churn, 1, {{"source", "datasets"}, {"table", "customers"}});
    ASSERT_TRUE(composed.ok()) << composed.error;
    EXPECT_FALSE(composed.preview);
    EXPECT_EQ(composed.methodologies,
              (std::vector<std::string>{"M-2", "M-1", "M-3"}));
    EXPECT_GT(composed.graph.node_count(), 12u);
    EXPECT_TRUE(composed.missing_components.empty());
    EXPECT_TRUE(churn->runnable);

    for (int stage = 1; stage <= 3; ++stage) {
        const zetta::ir::ExecutionGraph stage_graph =
            zetta::ml::solution_stage_graph(composed.graph, stage);
        EXPECT_FALSE(stage_graph.empty()) << stage;
    }
}

TEST(SolutionRegistry, ChurnEntryCarriesTheCompleteReferenceContract) {
    const zetta::ml::Solution* churn = zetta::ml::find_solution("S-1");
    ASSERT_NE(churn, nullptr);
    EXPECT_TRUE(churn->runnable);
    EXPECT_EQ(churn->issue,
              "https://github.com/oramasearch/zetta/issues/1110");
    EXPECT_EQ(churn->action_patterns.size(), 3u);
    EXPECT_EQ(churn->templates.experiment.holdouts.size(), 3u);
    EXPECT_EQ(churn->templates.experiment.treatment_cells,
              (std::vector<std::string>{"csm_task", "retention_journey"}));
    ASSERT_EQ(churn->templates.guardrails.size(), 4u);
    EXPECT_EQ(churn->templates.guardrails.back().metric,
              "action_execution_rate");
    EXPECT_EQ(churn->templates.guardrails.back().direction, "minimum");

    const nlohmann::json value = zetta::ml::solution_json(*churn);
    EXPECT_FALSE(value.contains("capability_levels"));
    ASSERT_TRUE(value.contains("levels"));
    ASSERT_EQ(value.at("levels").size(), 4u);
    EXPECT_EQ(value.at("levels").at(0),
              (nlohmann::json{{"level", 1},
                              {"name", "CL-1"},
                              {"description", "propensity, reasons, segments, holdout and report"}}));
}

TEST(SolutionRegistry, LeadScoringEntryCarriesTheCompleteContract) {
    const zetta::ml::Solution* lead_scoring =
        zetta::ml::find_solution("S-2");
    ASSERT_NE(lead_scoring, nullptr);
    EXPECT_TRUE(lead_scoring->runnable);
    EXPECT_EQ(lead_scoring->issue,
              "https://github.com/oramasearch/zetta/issues/1111");
    EXPECT_FALSE(lead_scoring->templates.minimal);
    EXPECT_EQ(lead_scoring->action_patterns.size(), 2u);
    EXPECT_EQ(lead_scoring->templates.experiment.holdouts.size(), 2u);
    EXPECT_EQ(lead_scoring->templates.experiment.treatment_cells,
              (std::vector<std::string>{"routed"}));
    ASSERT_EQ(lead_scoring->templates.guardrails.size(), 4u);
    EXPECT_EQ(lead_scoring->templates.guardrails.front().metric,
              "sla_miss_rate");
    EXPECT_EQ(lead_scoring->templates.guardrails.back().direction,
              "minimum");
    EXPECT_EQ(lead_scoring->templates.measurement.outcome, "converted");

    const nlohmann::json policy = lead_scoring->templates.policy;
    EXPECT_EQ(policy.at("tiers").size(), 2u);
    EXPECT_EQ(policy.at("contact_cap"), 6);
    EXPECT_EQ(policy.at("actions").at("priority_queue_review").at("autonomy"),
              "A1");
    EXPECT_EQ(policy.at("eligibility_rules").size(), 3u);
}

TEST(SolutionRegistry, ReplenishmentEntryCarriesExecutableDefaults) {
    const zetta::ml::Solution* replenishment =
        zetta::ml::find_solution("S-6");
    ASSERT_NE(replenishment, nullptr);
    EXPECT_TRUE(replenishment->runnable);
    EXPECT_EQ(replenishment->issue,
              "https://github.com/oramasearch/zetta/issues/1115");
    EXPECT_FALSE(replenishment->templates.minimal);
    EXPECT_EQ(replenishment->templates.measurement.design, "holdout");
    EXPECT_EQ(replenishment->templates.measurement.outcome,
              "service_level");
    EXPECT_EQ(replenishment->templates.experiment.treatment_cells,
              (std::vector<std::string>{"replenishment_action"}));
    ASSERT_EQ(replenishment->templates.guardrails.size(), 5u);
    EXPECT_EQ(replenishment->templates.guardrails.front().metric,
              "replenishment_spend");
    EXPECT_EQ(replenishment->templates.guardrails.at(1).direction,
              "minimum");
    EXPECT_EQ(zetta::solutions::solution_policy_template("S-6", 1)
                  .at("tiers")
                  .at(0)
                  .at("action"),
              "draft_order");
    EXPECT_EQ(zetta::solutions::solution_policy_template("S-6", 2)
                  .at("tiers")
                  .at(0)
                  .at("action"),
              "erp_replenishment_task");
    EXPECT_EQ(zetta::solutions::solution_policy_template("S-6", 4)
                  .at("tiers")
                  .at(0)
                  .at("action"),
              "release_order");
}

TEST(SolutionRegistry, RefusesSketchesAndUnknownLevels) {
    const zetta::ml::Solution* sketch = zetta::ml::find_solution("B-1");
    ASSERT_NE(sketch, nullptr);
    const zetta::ml::ComposedSolution refused =
        zetta::ml::compose_solution(*sketch, 1, {});
    EXPECT_FALSE(refused.ok());
    EXPECT_NE(refused.error.find(sketch->promotion_condition),
              std::string::npos);

    const zetta::ml::Solution* churn = zetta::ml::find_solution("S-1");
    ASSERT_NE(churn, nullptr);
    EXPECT_FALSE(zetta::ml::compose_solution(*churn, 0, {}).ok());
    EXPECT_FALSE(zetta::ml::compose_solution(*churn, 5, {}).ok());
}

TEST(SolutionRegistry, ComposedGateChecksEachStageAndRejectsHiddenDataDependencies) {
    const auto* solution = zetta::ml::find_solution("S-1");
    ASSERT_NE(solution, nullptr);
    auto composed = zetta::ml::compose_solution(*solution, 1, {});
    ASSERT_TRUE(composed.ok());
    const auto gate = zetta::pipeline::default_gate();
    const auto initial = gate.run(composed.graph);
    EXPECT_TRUE(initial.passed()) << nlohmann::json(initial).dump(2);
    auto model = composed.graph.find_node("solution_stage_3_train_segments");
    ASSERT_TRUE(model);
    model->attributes["segmentation"]["scaling"] = "none";
    EXPECT_FALSE(gate.run(composed.graph).passed());
    model->attributes["segmentation"]["scaling"] = "global_auto";
    composed.graph.add_edge("solution_stage_2_train_churn_model", model->id,
                            zetta::ir::EdgeKind::DataFlow);
    EXPECT_FALSE(gate.run(composed.graph).passed());
}

TEST(SolutionRegistry, S1CorrectnessChecksRefuseEveryWrongCase) {
    const auto accepted = zetta::pipeline::default_gate().run(bound_s1_graph());
    ASSERT_TRUE(accepted.passed()) << nlohmann::json(accepted).dump(2);

    auto point_in_time = bound_s1_graph();
    for (const auto& source :
         point_in_time.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (source->attributes.contains("solution_stage")) {
            source->attributes["source_kind"] = "live";
            break;
        }
    }
    expect_s1_failure(point_in_time, "s1_point_in_time");

    auto label_window = bound_s1_graph();
    label_window.find_node("solution_manifest")
        ->attributes["s1_correctness"]["label_window_start"] =
            "2024-01-01T12:00:00Z";
    expect_s1_failure(label_window, "s1_label_window");

    auto temporal = bound_s1_graph();
    temporal.find_node("solution_manifest")
        ->attributes["s1_correctness"]["validation"]["split"] = "random";
    expect_s1_failure(temporal, "s1_temporal_validation");

    auto group = bound_s1_graph();
    group.find_node("solution_manifest")
        ->attributes["s1_correctness"]["validation"]["group_disjoint"] = false;
    expect_s1_failure(group, "s1_group_split");

    auto censoring = bound_s1_graph();
    censoring.find_node("solution_manifest")
        ->attributes["s1_correctness"]["labels"]["fully_observed"] = false;
    expect_s1_failure(censoring, "s1_censoring");

    auto calibration = bound_s1_graph();
    auto& sampling = calibration.find_node("solution_manifest")
                         ->attributes["s1_correctness"]["sampling"];
    sampling["used"] = true;
    sampling["calibration"] = "none";
    expect_s1_failure(calibration, "s1_calibration");

    auto entities = bound_s1_graph();
    entities.find_node("solution_manifest")
        ->attributes["s1_correctness"]["entity_resolution"]
                   ["duplicate_policy"] = "allow";
    expect_s1_failure(entities, "s1_entity_resolution");

    auto blocked = bound_s1_graph();
    blocked.find_node("solution_manifest")
        ->attributes["s1_correctness"]["leaky_feature_blocklist"]
        .push_back("usage");
    expect_s1_failure(blocked, "s1_leakage_review");

    auto suspicious = bound_s1_graph();
    suspicious.find_node("solution_manifest")
        ->attributes["s1_correctness"]["reported_auc"] = 0.951;
    expect_s1_failure(suspicious, "s1_leakage_review");
}

TEST(SolutionRegistry, S1EveryLevelBuildsAndLevelThreeProtectsSleepingDogs) {
    for (int level = 1; level <= 4; ++level) {
        const auto report =
            zetta::pipeline::default_gate().run(bound_s1_graph(level));
        EXPECT_TRUE(report.passed())
            << "level " << level << '\n' << nlohmann::json(report).dump(2);
    }

    auto graph = bound_s1_graph(3);
    for (const auto& model :
         graph.nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (model->attributes.value("model_kind", std::string{}) == "uplift") {
            model->attributes["randomized_cycles"] = 0;
        }
    }
    expect_s1_failure(graph, "s1_level_composition");
}

TEST(SolutionRegistry, S2ChecksRefuseWrongConversionAndRoutingCases) {
    const auto accepted = zetta::pipeline::default_gate().run(bound_s2_graph());
    ASSERT_TRUE(accepted.passed()) << nlohmann::json(accepted).dump(2);

    auto conversion_window = bound_s2_graph();
    conversion_window.find_node("solution_manifest")
        ->attributes["s2_correctness"]["label_window_start"] =
            "2024-01-01T12:00:00Z";
    expect_s2_failure(conversion_window, "s2_point_in_time_conversion");

    auto incomplete_labels = bound_s2_graph();
    incomplete_labels.find_node("solution_manifest")
        ->attributes["s2_correctness"]["labels"]["fully_observed"] = false;
    expect_s2_failure(incomplete_labels, "s2_point_in_time_conversion");

    auto post_qualification = bound_s2_graph();
    post_qualification.find_node("solution_manifest")
        ->attributes["s2_correctness"]["feature_names"]
        .push_back("opportunity_stage");
    expect_s2_failure(post_qualification,
                      "s2_post_qualification_leakage");

    auto capacity = bound_s2_graph();
    capacity.find_node("solution_manifest")
        ->attributes["s2_correctness"]["routing"]["capacity_aware"] = false;
    expect_s2_failure(capacity, "s2_capacity_routing");
}

TEST(SolutionRegistry, S2EveryLevelBuildsThroughRegisteredMethodologies) {
    for (int level = 1; level <= 4; ++level) {
        const zetta::ir::ExecutionGraph graph = bound_s2_graph(level);
        ASSERT_FALSE(graph.empty()) << level;
        const auto report = zetta::pipeline::default_gate().run(graph);
        EXPECT_TRUE(report.passed())
            << "level " << level << '\n' << nlohmann::json(report).dump(2);
        const auto manifest = graph.find_node("solution_manifest");
        ASSERT_TRUE(manifest);
        const std::size_t expected = level >= 3 ? 4u : 3u;
        EXPECT_EQ(manifest->attributes.at("methodologies").size(), expected);
        for (std::size_t stage = 1; stage <= expected; ++stage) {
            EXPECT_FALSE(zetta::ml::solution_stage_graph(
                             graph, static_cast<int>(stage))
                             .empty());
        }
    }
}

TEST(SolutionRegistry, S3EntryCarriesExecutableValuePolicyAndHoldout) {
    const zetta::ml::Solution* lifetime_value =
        zetta::ml::find_solution("S-3");
    ASSERT_NE(lifetime_value, nullptr);
    EXPECT_TRUE(lifetime_value->runnable);
    EXPECT_EQ(lifetime_value->issue,
              "https://github.com/oramasearch/zetta/issues/1112");
    EXPECT_FALSE(lifetime_value->templates.minimal);
    ASSERT_EQ(lifetime_value->action_patterns.size(), 3u);
    ASSERT_EQ(lifetime_value->templates.experiment.holdouts.size(), 3u);
    EXPECT_EQ(lifetime_value->templates.experiment.treatment_cells,
              (std::vector<std::string>{"audience_sync",
                                        "budget_reallocation"}));
    ASSERT_EQ(lifetime_value->templates.guardrails.size(), 5u);
    EXPECT_EQ(lifetime_value->templates.measurement.outcome, "net_value");
    EXPECT_EQ(lifetime_value->templates.measurement.window_days, 365);

    const nlohmann::json& policy = lifetime_value->templates.policy;
    EXPECT_EQ(policy.at("tiers").at(0).at("action"),
              "budget_reallocation");
    EXPECT_EQ(policy.at("actions").at("budget_reallocation").at("autonomy"),
              "A1");
    EXPECT_EQ(policy.at("actions").at("audience_sync").at("autonomy"),
              "A2");
    EXPECT_EQ(policy.at("budget").at("limit"), 10000.0);
}


TEST(SolutionRegistry, S3CorrectnessChecksRefuseEveryWrongCase) {
    for (int level = 1; level <= 4; ++level) {
        const auto report =
            zetta::pipeline::default_gate().run(bound_s3_graph(level));
        EXPECT_TRUE(report.passed())
            << "level " << level << '\n'
            << nlohmann::json(report).dump(2);
    }

    auto tenure = bound_s3_graph();
    tenure.find_node("solution_manifest")
        ->attributes["s3_correctness"]["short_tenure"]
                   ["applied_to_training_labels"] = false;
    expect_s3_failure(tenure, "s3_short_tenure_censoring");

    auto horizon = bound_s3_graph();
    horizon.find_node("solution_manifest")
        ->attributes["s3_correctness"]["value_horizon_days"] = 0;
    expect_s3_failure(horizon, "s3_value_horizon");

    auto entities = bound_s3_graph();
    entities.find_node("solution_manifest")
        ->attributes["s3_correctness"]["entity_accounting"]
                   ["duplicate_policy"] = "sum";
    expect_s3_failure(entities, "s3_entity_accounting");
}


TEST(SolutionRegistry, CollectionsEntryCarriesTheCompleteExecutableContract) {
    const zetta::ml::Solution* collections =
        zetta::ml::find_solution("S-4");
    ASSERT_NE(collections, nullptr);
    EXPECT_TRUE(collections->runnable);
    EXPECT_EQ(collections->issue,
              "https://github.com/oramasearch/zetta/issues/1113");
    EXPECT_FALSE(collections->templates.minimal);
    EXPECT_EQ(collections->action_patterns,
              (std::vector<std::string>{"policy-engine treatment selection",
                                        "digital treatment delivery",
                                        "dialer queue tasks"}));
    EXPECT_EQ(collections->templates.experiment.treatment_cells,
              (std::vector<std::string>{"dialer_queue",
                                        "digital_treatment"}));
    ASSERT_EQ(collections->templates.guardrails.size(), 5u);
    EXPECT_EQ(collections->templates.measurement.design,
              "champion_challenger");
    EXPECT_EQ(collections->templates.measurement.outcome, "net_recovery");
    EXPECT_EQ(collections->templates.policy.at("contact_cap"), 6);
    EXPECT_EQ(collections->templates.policy.at("frequency_cap")
                  .at("window_seconds"),
              604800);
}

TEST(SolutionRegistry, S4EveryLevelBuildsAndDomainChecksRefuseWrongCases) {
    for (int level = 1; level <= 4; ++level) {
        const auto report =
            zetta::pipeline::default_gate().run(bound_s4_graph(level));
        EXPECT_TRUE(report.passed())
            << "level " << level << '\n' << nlohmann::json(report).dump(2);
    }

    auto regulatory = bound_s4_graph();
    regulatory.find_node("solution_manifest")
        ->attributes["s4_correctness"]["policy"]["contact_cap"] = 0;
    expect_s4_failure(regulatory, "s4_regulatory_contact");

    auto vulnerable = bound_s4_graph();
    vulnerable.find_node("solution_manifest")
        ->attributes["s4_correctness"]["policy"]["eligibility_rules"] =
            nlohmann::json::array();
    expect_s4_failure(vulnerable, "s4_vulnerable_customer");

    auto promise = bound_s4_graph();
    promise.find_node("solution_manifest")
        ->attributes["s4_correctness"]["promise_to_pay_labels"]
                   ["future_updates_excluded"] = false;
    expect_s4_failure(promise, "s4_promise_to_pay_point_in_time");
}

TEST(SolutionRegistry, ClaimsEntryCarriesTheCompleteExecutableContract) {
    const zetta::ml::Solution* claims = zetta::ml::find_solution("S-12");
    ASSERT_NE(claims, nullptr);
    EXPECT_TRUE(claims->runnable);
    EXPECT_FALSE(claims->templates.minimal);
    EXPECT_EQ(claims->issue,
              "https://github.com/oramasearch/zetta/issues/1121");
    EXPECT_EQ(claims->action_patterns,
              (std::vector<std::string>{"path assignment", "referral tasks"}));
    EXPECT_EQ(claims->templates.measurement.design, "holdout");
    EXPECT_EQ(claims->templates.measurement.outcome,
              "referral_confirmed");
    EXPECT_EQ(claims->templates.experiment.holdouts.size(), 3u);
    EXPECT_EQ(claims->templates.experiment.treatment_cells,
              (std::vector<std::string>{"path_assignment", "referral_task"}));
    ASSERT_TRUE(claims->templates.policy.at("actions").contains(
        "denial_recommendation"));
    EXPECT_EQ(claims->templates.policy.at("actions")
                  .at("denial_recommendation")
                  .at("autonomy"),
              "A0");
    EXPECT_EQ(claims->templates.policy.at("tiers").at(0).at("action"),
              "siu_referral");
    EXPECT_EQ(claims->templates.policy.at("tiers").at(1).at("action"),
              "adjuster_assignment");
    EXPECT_EQ(claims->templates.policy.at("tiers").at(2).at("action"),
              "fast_track");
    ASSERT_EQ(claims->templates.guardrails.size(), 5u);
    EXPECT_EQ(claims->templates.guardrails.front().metric,
              "automated_denials");
}

TEST(SolutionRegistry, S12CorrectnessChecksRefuseEveryWrongCase) {
    const auto accepted =
        zetta::pipeline::default_gate().run(bound_s12_graph());
    ASSERT_TRUE(accepted.passed()) << nlohmann::json(accepted).dump(2);

    auto denial = bound_s12_graph();
    denial.find_node("solution_manifest")
        ->attributes["s12_correctness"]["policy"]["tiers"][0]["action"] =
        "deny_claim";
    expect_s12_failure(denial, "s12_no_automated_denial");

    auto fairness = bound_s12_graph();
    fairness.find_node("solution_manifest")
        ->attributes["s12_correctness"]["fairness_review"]
                   ["cohort_review_required"] = false;
    expect_s12_failure(fairness, "s12_fairness_review");

    auto confidence = bound_s12_graph();
    for (const auto& model :
         confidence.nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (model->attributes.value("model_kind", std::string{}) ==
            "document_extraction") {
            model->attributes["review_threshold"] = 0.79;
        }
    }
    expect_s12_failure(confidence, "s12_document_confidence");

    auto composition = bound_s12_graph(3);
    auto& methodologies = composition.find_node("solution_manifest")
                               ->attributes["methodologies"];
    methodologies.erase(
        std::find(methodologies.begin(), methodologies.end(), "M-3"));
    expect_s12_failure(composition, "s12_level_composition");
}

TEST(SolutionRegistry, S12EveryLevelBuildsWithClaimScoreBindings) {
    for (int level = 1; level <= 4; ++level) {
        const auto graph = bound_s12_graph(level);
        const auto report = zetta::pipeline::default_gate().run(graph);
        EXPECT_TRUE(report.passed())
            << "level " << level << '\n' << nlohmann::json(report).dump(2);
        const auto manifest = graph.find_node("solution_manifest");
        ASSERT_TRUE(manifest);
        const auto& methods = manifest->attributes.at("methodologies");
        EXPECT_EQ(std::find(methods.begin(), methods.end(), "M-3") !=
                      methods.end(),
                  level >= 3);
    }
}

TEST(SolutionRegistry, S6CorrectnessChecksRefuseEveryWrongCase) {
    const auto accepted =
        zetta::pipeline::default_gate().run(bound_s6_graph());
    ASSERT_TRUE(accepted.passed()) << nlohmann::json(accepted).dump(2);

    auto service = bound_s6_graph();
    service.find_node("solution_manifest")
        ->attributes["s6_correctness"]["required_constraints"] = {"budget"};
    const auto service_report = zetta::pipeline::default_gate().run(service);
    const auto* service_check = check_result(
        service_report, "solution/s6_service_budget_constraints");
    ASSERT_NE(service_check, nullptr);
    EXPECT_EQ(service_check->status, zetta::pipeline::CheckStatus::Fail);

    auto budget = bound_s6_graph();
    budget.find_node("solution_manifest")
        ->attributes["s6_correctness"]["budget_limit"] = 0.0;
    const auto budget_report = zetta::pipeline::default_gate().run(budget);
    const auto* budget_check = check_result(
        budget_report, "solution/s6_service_budget_constraints");
    ASSERT_NE(budget_check, nullptr);
    EXPECT_EQ(budget_check->status, zetta::pipeline::CheckStatus::Fail);

    auto lead_time = bound_s6_graph();
    lead_time.find_node("solution_manifest")
        ->attributes["s6_correctness"]["forecast_horizon_days"] = 6.0;
    const auto lead_time_report =
        zetta::pipeline::default_gate().run(lead_time);
    const auto* lead_time_check = check_result(
        lead_time_report, "solution/s6_lead_time_realism");
    ASSERT_NE(lead_time_check, nullptr);
    EXPECT_EQ(lead_time_check->status, zetta::pipeline::CheckStatus::Fail);

    auto approval = bound_s6_graph();
    approval.find_node("solution_manifest")
        ->attributes["s6_correctness"]["approval_required"] = false;
    const auto approval_report =
        zetta::pipeline::default_gate().run(approval);
    const auto* approval_check = check_result(
        approval_report, "solution/s6_order_approval");
    ASSERT_NE(approval_check, nullptr);
    EXPECT_EQ(approval_check->status, zetta::pipeline::CheckStatus::Fail);
}

TEST(SolutionRegistry, S6EveryLevelBuildsThroughItsMethodologyCallbacks) {
    for (int level = 1; level <= 4; ++level) {
        const auto graph = bound_s6_graph(level);
        const auto report = zetta::pipeline::default_gate().run(graph);
        EXPECT_TRUE(report.passed())
            << "level " << level << '\n' << nlohmann::json(report).dump(2);
        const auto manifest = graph.find_node("solution_manifest");
        ASSERT_TRUE(manifest);
        const std::size_t expected = level >= 3 ? 3u : 2u;
        EXPECT_EQ(manifest->attributes.at("methodologies").size(), expected);
        for (std::size_t stage = 1; stage <= expected; ++stage) {
            EXPECT_FALSE(zetta::ml::solution_stage_graph(
                             graph, static_cast<int>(stage))
                             .empty());
        }
    }
}

} // namespace
