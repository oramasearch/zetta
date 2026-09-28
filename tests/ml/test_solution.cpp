#include <gtest/gtest.h>

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

zetta::ir::ExecutionGraph bound_s7_graph(int level = 1) {
    const zetta::ml::Solution* solution = zetta::ml::find_solution("S-7");
    if (solution == nullptr) {
        return {};
    }
    const nlohmann::json sources = {
        {"source", "datasets"},
        {"table", "kpi_history"},
        {"M-7",
         {{"mode", "kpi_time_series"},
          {"entity_column", "entity_id"},
          {"label_column", "confirmed_anomaly"},
          {"time_column", "period"},
          {"feature_columns", {"kpi", "candidate_driver"}},
          {"alert_budget", 12}}},
        {"M-3",
         {{"entity_column", "entity_id"},
          {"label_column", "confirmed_anomaly"},
          {"time_column", "period"},
          {"as_of", 239.0},
          {"feature_columns", {"kpi", "candidate_driver"}}}},
        {"M-4",
         {{"time_column", "period"},
          {"target_column", "kpi"},
          {"series_column", ""}}}};
    zetta::ml::ComposedSolution composed =
        zetta::ml::compose_solution(*solution, level, sources);
    if (!composed.ok()) {
        return {};
    }
    for (const auto& source :
         composed.graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!source->attributes.contains("solution_stage")) {
            continue;
        }
        if (source->attributes.value("methodology", std::string{}) == "M-4") {
            continue;
        }
        source->attributes["source_kind"] = "snapshot";
        source->attributes["snapshot_as_of"] = "2024-01-01T00:00:00Z";
    }
    const auto manifest = composed.graph.find_node("solution_manifest");
    manifest->attributes["foundation"] = nlohmann::json::object();
    manifest->attributes["s7_correctness"] = {
        {"driver_attribution",
         {{"claim", "associational"},
          {"causal_claims", false},
          {"caveat_required", true},
          {"caveat", "Candidate drivers do not establish causation."}}},
        {"alerting",
         {{"alert_budget", 12},
          {"window_days", 7},
          {"owner_column", "owner"}}},
        {"baseline",
         {{"kind", "causal_seasonal"}, {"strictly_prior", true}}}};
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

void expect_s7_failure(const zetta::ir::ExecutionGraph& graph,
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
                          solution.id != "S-7");
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

TEST(SolutionRegistry, KpiAnomalyEntryCarriesTheCompleteContract) {
    const zetta::ml::Solution* anomaly = zetta::ml::find_solution("S-7");
    ASSERT_NE(anomaly, nullptr);
    EXPECT_TRUE(anomaly->runnable);
    EXPECT_EQ(anomaly->issue,
              "https://github.com/oramasearch/zetta/issues/1116");
    EXPECT_FALSE(anomaly->templates.minimal);
    EXPECT_EQ(anomaly->action_patterns.size(), 2u);
    EXPECT_EQ(anomaly->templates.experiment.holdouts.size(), 2u);
    EXPECT_EQ(anomaly->templates.experiment.treatment_cells,
              (std::vector<std::string>{"forum_routed"}));
    ASSERT_EQ(anomaly->templates.guardrails.size(), 6u);
    EXPECT_EQ(anomaly->templates.guardrails.front().metric, "alert_volume");
    EXPECT_EQ(anomaly->templates.guardrails.back().direction, "minimum");
    EXPECT_EQ(anomaly->templates.measurement.design, "phased");
    EXPECT_EQ(anomaly->templates.measurement.outcome, "decision_adopted");

    const nlohmann::json policy = anomaly->templates.policy;
    EXPECT_EQ(policy.at("tiers").size(), 2u);
    EXPECT_EQ(policy.at("contact_cap"), 12);
    EXPECT_EQ(policy.at("actions").at("executive_review").at("autonomy"),
              "A1");
    EXPECT_EQ(policy.at("eligibility_rules").size(), 3u);
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

TEST(SolutionRegistry, S7ChecksRefuseWrongAttributionBudgetAndBaseline) {
    const auto accepted = zetta::pipeline::default_gate().run(bound_s7_graph());
    ASSERT_TRUE(accepted.passed()) << nlohmann::json(accepted).dump(2);

    auto causal_claim = bound_s7_graph();
    causal_claim.find_node("solution_manifest")
        ->attributes["s7_correctness"]["driver_attribution"]
                   ["causal_claims"] = true;
    expect_s7_failure(causal_claim, "s7_causal_humility");

    auto budget = bound_s7_graph();
    budget.find_node("solution_manifest")
        ->attributes["s7_correctness"]["alerting"]["alert_budget"] = 11;
    expect_s7_failure(budget, "s7_alert_budget");

    auto seasonality = bound_s7_graph();
    for (const auto& model :
         seasonality.nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (model->attributes.value("model_kind", std::string{}) ==
            "anomaly") {
            model->attributes["seasonal_period"] = 1;
        }
    }
    expect_s7_failure(seasonality, "s7_seasonality_baseline");

    auto future_fitted = bound_s7_graph();
    future_fitted.find_node("solution_manifest")
        ->attributes["s7_correctness"]["baseline"]["strictly_prior"] = false;
    expect_s7_failure(future_fitted, "s7_seasonality_baseline");

    auto forecast = bound_s7_graph(3);
    for (const auto& model :
         forecast.nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (model->attributes.value("model_kind", std::string{}) ==
            "forecast") {
            model->attributes["baseline"] = "last_value";
        }
    }
    expect_s7_failure(forecast, "s7_seasonality_baseline");
}

TEST(SolutionRegistry, S7EveryLevelBuildsThroughRegisteredMethodologies) {
    for (int level = 1; level <= 4; ++level) {
        const zetta::ir::ExecutionGraph graph = bound_s7_graph(level);
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

} // namespace
