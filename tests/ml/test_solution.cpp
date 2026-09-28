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

void expect_s4_failure(const zetta::ir::ExecutionGraph& graph,
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
                      solution.id != "S-1" && solution.id != "S-4");
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

} // namespace
