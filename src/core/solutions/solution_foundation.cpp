#include "zetta/solutions/solution_execution.hpp"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <map>
#include <optional>
#include <utility>
#include <nlohmann/json-schema.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/solution.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/query/ast.hpp"
#include "zetta/query/distributed_select.hpp"
#include "zetta/solutions/action_integration.hpp"
#include "zetta/solutions/audit_ledger.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/solutions/roi_readout.hpp"
#include "zetta/solutions/snapshot.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::solutions {
namespace {

using Json = nlohmann::json;
using api::ApiResult;

std::uint64_t experiment_seed(const std::string& scope) {
    std::uint64_t seed = 14695981039346656037ULL;
    for (const unsigned char byte : scope) {
        seed ^= byte;
        seed *= 1099511628211ULL;
    }
    return seed & 0x7fffffffffffffffULL;
}

std::string entity_key(const Json& entity) {
    if (entity.is_string()) {
        return entity.get<std::string>();
    }
    if (entity.is_number_integer()) {
        return std::to_string(entity.get<std::int64_t>());
    }
    if (entity.is_number_unsigned()) {
        return std::to_string(entity.get<std::uint64_t>());
    }
    if (entity.is_number_float()) {
        const double number = entity.get<double>();
        if (std::isfinite(number) && std::floor(number) == number &&
            number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
            number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return std::to_string(static_cast<std::int64_t>(number));
        }
    }
    return entity.dump();
}

std::shared_ptr<data::Table> table_at(Node& node, const std::string& path) {
    const auto split = path.rfind('.');
    if (split == std::string::npos) {
        return nullptr;
    }
    const auto database = node.find_database(path.substr(0, split));
    return database ? database->find_table(path.substr(split + 1)) : nullptr;
}

Json create_bindings(Node& node, const pipeline::Project& project,
                     const ml::Solution& solution, const std::string& name,
                     int level, const Json& settings, const std::string& actor,
                     std::string& error) {
    const std::string scope = project.id() + "/" + name;
    DecisionPolicyStore policies(*node.replication(), &node, &node.experiment_manager());
    const auto policy = policies.create(
        scope, solution_policy_template(solution.id, level), actor, &error);
    if (!policy) {
        return {};
    }
    ExperimentValue experiment;
    experiment.name = scope;
    experiment.seed = experiment_seed(scope);
    // Every configured risk tier has the same independent 10% allocation
    // probability. Tier membership does not influence the entity hash.
    const double holdout = solution.templates.experiment.holdouts.front().fraction;
    experiment.allocations.push_back({scope + "/holdout", ExperimentTier::Holdout, holdout});
    const auto& cells = solution.templates.experiment.treatment_cells;
    for (const std::string& cell : cells) {
        experiment.allocations.push_back(
            {scope + "/" + cell, ExperimentTier::Treatment, (1.0 - holdout) / cells.size()});
    }
    const auto created = node.experiment_manager().create(std::move(experiment), &error);
    if (!created) {
        return {};
    }
    MonitorSpec spec;
    spec.solution = scope;
    for (const SolutionGuardrail& guardrail : solution.templates.guardrails) {
        MetricBound bound;
        bound.name = guardrail.metric;
        if (guardrail.direction == "maximum") {
            bound.maximum = guardrail.bound;
        } else {
            bound.minimum = guardrail.bound;
        }
        spec.guardrails.push_back(std::move(bound));
    }
    const auto monitor = node.create_monitor(std::move(spec), &error);
    if (!monitor) {
        return {};
    }
    Json binding = settings;
    binding["solution"] = solution.id;
    binding["scope"] = scope;
    binding["policy_id"] = policy->id;
    binding["experiment_id"] = created->id;
    binding["monitor_id"] = monitor->id();
    binding["measurement"] = to_json(solution.templates.measurement);
    return binding;
}

void bind_sources(ir::ExecutionGraph& graph, const SnapshotSpec& snapshot) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    const std::string solution =
        manifest
            ? manifest->attributes.value("solution", std::string{})
            : std::string{};
    const bool solution_with_numeric_cutoff =
        manifest &&
        (solution == "S-1" || solution == "S-2" || solution == "S-5" ||
         solution == "S-8" || solution == "S-9");
    for (const auto& step : graph.nodes()) {
        if (step->kind != ir::StepKind::Connect ||
            !step->attributes.contains("solution_stage")) {
            continue;
        }
        const bool reads_forecast_history =
            solution == "S-6" &&
            step->attributes.value("methodology", std::string{}) == "M-4";
        if (reads_forecast_history) {
            continue;
        }
        step->attributes["source"] = snapshot.database;
        step->attributes["table"] = snapshot.target_table;
        if (manifest &&
            manifest->attributes.value("solution", std::string{}) == "S-7" &&
            step->attributes.value("methodology", std::string{}) == "M-4") {
            continue;
        }
        step->attributes["source_kind"] = "snapshot";
        if (snapshot.as_of.is_string()) {
            step->attributes["snapshot_as_of"] = snapshot.as_of;
        } else if (solution_with_numeric_cutoff) {
            step->attributes["solution_snapshot_as_of"] = snapshot.as_of;
        }
    }
}

void bind_s1_correctness(ir::ExecutionGraph& graph,
                         const SnapshotSpec& snapshot,
                         const Json& settings,
                         const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-1") {
        return;
    }
    const Json common =
        sources.contains("correctness") && sources.at("correctness").is_object()
            ? sources.at("correctness")
        : sources.contains("common") && sources.at("common").is_object()
            ? sources.at("common")
            : Json::object();
    const double blackout_gap =
        common.value("blackout_gap_seconds", 7.0 * 86400.0);
    const std::optional<TimePoint> cutoff = parse_time_point(snapshot.as_of);
    const double feature_window_end = cutoff ? cutoff->value : 0.0;
    const Json default_blocklist = {
        "churned", "churn_date", "cancellation_reason",
        "retention_outcome", "post_churn_status"};
    Json profile = {
        {"prediction_time", snapshot.as_of},
        {"feature_window_end", feature_window_end},
        {"label_window_start", feature_window_end + blackout_gap},
        {"blackout_gap_seconds", blackout_gap},
        {"validation",
         {{"split", common.value("split", "out_of_time")},
          {"rolling_windows", common.value("rolling_windows", 3)},
          {"stable_across_windows",
           common.value("stable_across_windows", true)},
          {"group_column",
           settings.at("dataset").value("entity_column", std::string{})},
          {"group_disjoint", common.value("group_disjoint", true)}}},
        {"labels",
         {{"fully_observed", common.value("fully_observed_labels", true)}}},
        {"sampling",
         {{"used", common.value("sampling_used", false)},
          {"calibration", common.value("calibration", "platt")},
          {"calibration_disjoint",
           common.value("calibration_disjoint", true)}}},
        {"entity_resolution",
         {{"methodology", "M-2"},
          {"actionable_unit",
           settings.at("dataset").value("entity_column", std::string{})},
          {"duplicate_policy",
           common.value("duplicate_policy", "refuse")}}},
        {"leaky_feature_blocklist",
         common.value("leaky_feature_blocklist", default_blocklist)},
        {"feature_names",
         settings.at("dataset").value("feature_columns", Json::array())},
        {"too_good_auc_threshold",
         common.value("too_good_auc_threshold", 0.95)}};
    if (common.contains("reported_auc")) {
        profile["reported_auc"] = common.at("reported_auc");
    }
    manifest->attributes["s1_correctness"] = std::move(profile);
    for (const std::shared_ptr<ir::Node>& model :
         graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!model ||
            model->attributes.value("model_kind", std::string{}) != "uplift") {
            continue;
        }
        model->attributes["randomized_cycles"] =
            common.value("randomized_cycles", 1);
        model->attributes["sleeping_dogs_action"] = "no_contact";
        model->attributes["positive_uplift_only"] = true;
    }
}

void bind_s2_correctness(ir::ExecutionGraph& graph,
                         const SnapshotSpec& snapshot,
                         const Json& settings,
                         const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-2") {
        return;
    }
    const Json common =
        sources.contains("correctness") && sources.at("correctness").is_object()
            ? sources.at("correctness")
        : sources.contains("common") && sources.at("common").is_object()
            ? sources.at("common")
            : Json::object();
    const double blackout_gap =
        common.value("blackout_gap_seconds", 86400.0);
    const std::optional<TimePoint> cutoff = parse_time_point(snapshot.as_of);
    const double feature_window_end = cutoff ? cutoff->value : 0.0;
    const Json default_blocklist = {
        "qualified_at", "qualification_status", "sales_accepted_at",
        "opportunity_stage", "closed_won", "closed_won_at",
        "conversion_outcome", "contract_value"};
    const Json label_window_start =
        common.contains("label_window_start")
            ? common.at("label_window_start")
            : Json(feature_window_end + blackout_gap);
    Json profile = {
        {"prediction_time", snapshot.as_of},
        {"feature_window_end", feature_window_end},
        {"label_window_start", label_window_start},
        {"blackout_gap_seconds", blackout_gap},
        {"labels",
         {{"label_column",
           settings.at("dataset").value("label_column", std::string{})},
          {"fully_observed",
           common.value("fully_observed_conversion_labels", true)}}},
        {"post_qualification_blocklist",
         common.value("post_qualification_blocklist", default_blocklist)},
        {"feature_names",
         settings.at("dataset").value("feature_columns", Json::array())},
        {"routing",
         {{"capacity_aware", common.value("capacity_aware", true)},
          {"owner_column", common.value("owner_column", "owner")},
          {"territory_column",
           common.value("territory_column", "territory")},
          {"capacity_column",
           common.value("capacity_column", "rep_capacity_available")},
          {"over_capacity_action",
           common.value("over_capacity_action", "no_contact")}}}};
    manifest->attributes["s2_correctness"] = std::move(profile);
}

void bind_s6_correctness(ir::ExecutionGraph& graph, const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-6") {
        return;
    }
    const int level = manifest->attributes.value("level", 0);
    const Json common =
        sources.contains("correctness") && sources.at("correctness").is_object()
            ? sources.at("correctness")
            : Json::object();
    const Json forecast =
        sources.contains("M-4") && sources.at("M-4").is_object()
            ? sources.at("M-4")
            : Json::object();
    const Json policy = solution_policy_template("S-6", level);
    const Json& release = policy.at("actions").at("release_order");
    std::string action;
    for (const Json& tier : policy.at("tiers")) {
        if (tier.at("name") == "replenish") {
            action = tier.at("action").get<std::string>();
        }
    }
    manifest->attributes["s6_correctness"] = {
        {"service_level_target",
         common.value("service_level_target", 0.95)},
        {"budget_limit", policy.at("budget").at("limit")},
        {"required_constraints",
         common.value("required_constraints",
                      Json{"service_level", "budget"})},
        {"lead_time_days", common.value("lead_time_days", 7.0)},
        {"lead_time_source",
         common.value("lead_time_source", "snapshot")},
        {"forecast_horizon_days", forecast.value("horizon", 7.0)},
        {"automatic_release_max_quantity",
         common.value("automatic_release_max_quantity", 100.0)},
        {"hard_order_max_quantity",
         common.value("hard_order_max_quantity", 500.0)},
        {"approval_field", release.at("approval_field")},
        {"approval_above", release.at("approval_above")},
        {"hard_order_bound_enforced",
         common.value("hard_order_bound_enforced", true)},
        {"approval_required",
         common.value("approval_required", true)},
        {"level_action", action},
        {"kill_switch_configured", policy.contains("kill_switch")}};
}

void bind_s3_correctness(ir::ExecutionGraph& graph,
                         const Json& settings,
                         const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-3") {
        return;
    }
    const Json common =
        sources.contains("correctness") && sources.at("correctness").is_object()
            ? sources.at("correctness")
            : Json::object();
    const Json m1 =
        sources.contains("M-1") && sources.at("M-1").is_object()
            ? sources.at("M-1")
            : Json::object();
    const int horizon = m1.value(
        "value_horizon_days", common.value("value_horizon_days", 365));
    manifest->attributes["s3_correctness"] = {
        {"value_horizon_days", horizon},
        {"value_label",
         settings.at("dataset").value("label_column", std::string{})},
        {"short_tenure",
         {{"tenure_column", common.value("tenure_column", "tenure_days")},
          {"minimum_observed_days",
           common.value("minimum_observed_days", 90)},
          {"handling", common.value("short_tenure_handling", "right_censor")},
          {"applied_to_training_labels",
           common.value("short_tenure_censored", true)}}},
        {"entity_accounting",
         {{"methodology", "M-2"},
          {"entity_column",
           settings.at("dataset").value("entity_column", std::string{})},
          {"aggregation",
           common.value("entity_value_aggregation",
                        "one_value_per_entity")},
          {"duplicate_policy",
           common.value("duplicate_entity_policy", "refuse")}}}};
}

void bind_s4_correctness(ir::ExecutionGraph& graph,
                         const SnapshotSpec& snapshot,
                         const Json& settings,
                         const Json& sources,
                         const ml::Solution& solution) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-4") {
        return;
    }
    const Json common =
        sources.contains("correctness") && sources.at("correctness").is_object()
            ? sources.at("correctness")
        : sources.contains("common") && sources.at("common").is_object()
            ? sources.at("common")
            : Json::object();
    const Json& policy = solution.templates.policy;
    Json profile = {
        {"prediction_time", snapshot.as_of},
        {"policy", policy},
        {"regulatory_contact",
         {{"eligibility_field", "collections_eligible"},
          {"contact_allowed_field", "regulatory_contact_allowed"},
          {"contact_hours_field", "within_contact_hours"},
          {"contact_cap", policy.at("contact_cap")},
          {"frequency_cap", policy.at("frequency_cap")}}},
        {"vulnerable_customer",
         {{"field", "vulnerable"},
          {"excluded_when_true", true}}},
        {"promise_to_pay_labels",
         {{"semantics", "promise_to_pay"},
          {"label_column",
           settings.at("dataset").value("label_column", std::string{})},
          {"observation_time_column", snapshot.time_column},
          {"snapshot_as_of", snapshot.as_of},
          {"point_in_time",
           common.value("promise_to_pay_point_in_time", true)},
          {"future_updates_excluded",
           common.value("future_promise_updates_excluded", true)}}}};
    manifest->attributes["s4_correctness"] = std::move(profile);
    for (const std::shared_ptr<ir::Node>& model :
         graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!model ||
            model->attributes.value("model_kind", std::string{}) != "uplift") {
            continue;
        }
        model->attributes["randomized_cycles"] =
            common.value("randomized_cycles", 1);
        model->attributes["sleeping_dogs_action"] = "no_contact";
        model->attributes["positive_uplift_only"] = true;
    }
}

void bind_s12_correctness(ir::ExecutionGraph& graph,
                          const ml::Solution& solution,
                          const Json& settings,
                          const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-12") {
        return;
    }
    const Json common =
        sources.contains("correctness") &&
                sources.at("correctness").is_object()
            ? sources.at("correctness")
            : Json::object();
    const Json default_protected_fields = {
        "race", "ethnicity", "sex", "gender", "disability",
        "religion", "national_origin"};
    manifest->attributes["s12_correctness"] = {
        {"policy", solution.templates.policy},
        {"fairness_review",
         {{"cohort_review_required",
           common.value("cohort_review_required", true)},
          {"referral_rate_review_required",
           common.value("referral_rate_review_required", true)},
          {"owner", common.value("fairness_review_owner",
                                  "claims-governance")},
          {"blocked_feature_names",
           common.value("protected_feature_blocklist",
                        default_protected_fields)},
          {"feature_names",
           settings.at("dataset").value("feature_columns", Json::array())}}},
        {"document_confidence",
         {{"minimum_review_threshold",
           common.value("minimum_document_review_threshold", 0.8)},
          {"low_confidence_routes_to_review",
           common.value("low_confidence_routes_to_review", true)},
          {"fast_track_requires_clear",
           common.value("fast_track_requires_clear", true)}}}};
}

void bind_s5_correctness(ir::ExecutionGraph& graph,
                         const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-5") {
        return;
    }
    const Json common =
        sources.contains("correctness") &&
                sources.at("correctness").is_object()
            ? sources.at("correctness")
            : Json::object();
    const ir::Node* forecast = nullptr;
    for (const std::shared_ptr<ir::Node>& model :
         graph.nodes_of_kind(ir::StepKind::Model)) {
        if (model &&
            model->attributes.value("model_kind", std::string{}) ==
                "forecast") {
            forecast = model.get();
            break;
        }
    }
    if (forecast == nullptr) {
        return;
    }
    const std::string series_column =
        forecast->attributes.value("series_column", std::string{});
    Json hierarchy_columns = common.value(
        "hierarchy_columns",
        series_column.empty() ? Json::array() : Json::array({series_column}));
    manifest->attributes["s5_correctness"] = {
        {"validation",
         {{"split", forecast->attributes.value("split", std::string{})},
          {"backtest_folds",
           forecast->attributes.value("backtest_folds", 0)}}},
        {"future_covariates",
         common.value("future_covariates", Json::array())},
        {"time_column",
         forecast->attributes.value("time_column", std::string{})},
        {"target_column",
         forecast->attributes.value("target_column", std::string{})},
        {"series_column", series_column},
        {"hierarchy_columns", std::move(hierarchy_columns)},
        {"hierarchy_mode",
         common.value("hierarchy_mode", "bottom_level_only")},
        {"hierarchy_consistent",
         common.value("hierarchy_consistent", true)}};
}

void bind_s7_correctness(ir::ExecutionGraph& graph,
                         const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-7") {
        return;
    }
    const Json common =
        sources.contains("correctness") && sources.at("correctness").is_object()
            ? sources.at("correctness")
        : sources.contains("common") && sources.at("common").is_object()
            ? sources.at("common")
            : Json::object();
    int model_alert_budget = 0;
    for (const std::shared_ptr<ir::Node>& model :
         graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!model ||
            model->attributes.value("model_kind", std::string{}) !=
                "anomaly") {
            continue;
        }
        model_alert_budget = model->attributes.value("alert_budget", 0);
        break;
    }
    manifest->attributes["s7_correctness"] = {
        {"driver_attribution",
         {{"claim", common.value("driver_claim", "associational")},
          {"causal_claims", common.value("causal_claims", false)},
          {"caveat_required", common.value("caveat_required", true)},
          {"caveat",
           common.value(
               "causal_caveat",
               "Candidate drivers are associations with the anomaly, not "
               "evidence that they caused the KPI change.")}}},
        {"alerting",
         {{"alert_budget",
           common.value("alert_budget", model_alert_budget)},
          {"window_days", common.value("alert_window_days", 7)},
          {"owner_column", common.value("owner_column", "owner")}}},
        {"baseline",
         {{"kind", common.value("baseline_kind", "causal_seasonal")},
          {"strictly_prior", common.value("strictly_prior", true)}}}};
}

void bind_s8_correctness(ir::ExecutionGraph& graph,
                         const SnapshotSpec& snapshot,
                         const Json& settings,
                         const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-8") {
        return;
    }
    const Json common =
        sources.contains("correctness") &&
                sources.at("correctness").is_object()
            ? sources.at("correctness")
        : sources.contains("common") && sources.at("common").is_object()
            ? sources.at("common")
            : Json::object();
    const Json survival =
        sources.contains("M-5") && sources.at("M-5").is_object()
            ? sources.at("M-5")
            : Json::object();
    const Json feature_names =
        settings.at("dataset").value("feature_columns", Json::array());
    const Json default_blocklist = {
        "failure_code", "failure_at", "repair_completed_at",
        "replacement_reason", "post_failure_inspection",
        "maintenance_outcome"};
    Json profile = {
        {"prediction_time", snapshot.as_of},
        {"censoring",
         {{"event_indicator",
           settings.at("dataset").value("label_column", std::string{})},
          {"duration_column",
           survival.value("duration_column", "duration")},
          {"censored_assets_included",
           common.value("censored_assets_included", true)},
          {"censored_value", common.value("censored_value", 0)}}},
        {"sensor_drift",
         {{"monitoring_enabled",
           common.value("sensor_drift_monitoring", true)},
          {"baseline_end",
           common.contains("sensor_baseline_end")
               ? common.at("sensor_baseline_end")
               : snapshot.as_of},
          {"sensor_columns",
           common.value("sensor_columns", feature_names)},
          {"maximum_psi", common.value("maximum_sensor_psi", 0.2)}}},
        {"maintenance_records",
         {{"failure_time_column",
           common.value("failure_time_column", "failure_at")},
          {"truncate_at_failure",
           common.value("truncate_at_failure", true)},
          {"post_failure_records",
           common.value("post_failure_records", "exclude")}}},
        {"post_failure_blocklist",
         common.value("post_failure_blocklist", default_blocklist)},
        {"feature_names", feature_names}};
    manifest->attributes["s8_correctness"] = std::move(profile);
}

void bind_s9_correctness(ir::ExecutionGraph& graph,
                         const Json& settings,
                         const Json& sources) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest ||
        manifest->attributes.value("solution", std::string{}) != "S-9") {
        return;
    }
    const Json common =
        sources.contains("correctness") && sources.at("correctness").is_object()
            ? sources.at("correctness")
        : sources.contains("common") && sources.at("common").is_object()
            ? sources.at("common")
            : Json::object();
    const Json default_score = {
        {"methodology", "M-1"},
        {"output_name", "outbound response scores"},
        {"entity_column", "entity"},
        {"score_column", "response_propensity"}};
    const Json primary_score =
        settings.contains("decision_source")
            ? settings.at("decision_source")
            : default_score;
    Json profile = {
        {"audience",
         {{"consent_required", common.value("consent_required", true)},
          {"consent_column", common.value("consent_column", "consent")},
          {"suppression_column",
           common.value("suppression_column", "suppressed")},
          {"channel_eligibility_column",
           common.value("channel_eligibility_column", "channel_eligible")},
          {"missing_evidence_action",
           common.value("missing_evidence_action", "no_contact")},
          {"suppressed_action",
           common.value("suppressed_action", "no_contact")}}},
        {"contact_caps",
         {{"lifetime", common.value("contact_cap", 4)},
          {"maximum_actions", common.value("maximum_actions", 1)},
          {"window_seconds", common.value("window_seconds", 7 * 86400)},
          {"contacts_column", common.value("contacts_column", "contacts")},
          {"recent_actions_column",
           common.value("recent_actions_column", "recent_actions")},
          {"include_recorded_decisions",
           common.value("include_recorded_decisions", true)},
          {"at_cap_action", common.value("at_cap_action", "no_contact")}}},
        {"primary_score", primary_score}};
    manifest->attributes["s9_correctness"] = std::move(profile);
    for (const std::shared_ptr<ir::Node>& model :
         graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!model ||
            model->attributes.value("model_kind", std::string{}) != "uplift") {
            continue;
        }
        model->attributes["selection_basis"] =
            common.value("selection_basis", "incremental_uplift");
        model->attributes["positive_uplift_only"] =
            common.value("positive_uplift_only", true);
        model->attributes["non_positive_uplift_action"] =
            common.value("non_positive_uplift_action", "no_contact");
    }
}

ApiResult configure_readout(Node& node, const pipeline::Project& project,
                            const pipeline::ProjectGraph& graph,
                            const Json& binding, const std::string& actor) {
    const Json& economics = binding.at("roi_model");
    RoiReadoutSpec spec;
    spec.solution = binding.at("scope");
    spec.project = project.name();
    spec.graph = graph.id();
    spec.experiment = binding.at("experiment_id");
    spec.measurement_design = binding.at("measurement").at("design");
    spec.outcome_window_days = binding.at("measurement").at("outcome").at("window_days");
    spec.outcome_unit = binding.at("measurement").at("outcome").at("unit");
    spec.roi_model = {binding.at("measurement").at("outcome").at("name"),
                      economics.at("value_per_outcome"), economics.at("investment"),
                      economics.at("currency")};
    std::string error;
    const auto definition = RoiReadoutStore(*node.replication()).configure(spec, actor, &error);
    if (!definition) {
        return ApiResult::failure("engine_error", error);
    }
    return ApiResult::success({{"readout_id", definition->id}});
}

} // namespace

const Json& solution_foundation_schema() {
    static const Json schema = {
        {"type", "object"}, {"additionalProperties", false},
        {"required", {"snapshot", "delivery", "roi_model", "dataset"}},
        {"properties", {
            {"snapshot", snapshot_spec_schema()},
            {"dataset", data::dataset_contract_schema()},
            {"delivery", {{"type", "object"}, {"additionalProperties", false},
                {"required", {"endpoint", "owner"}}, {"properties", {
                    {"endpoint", {{"type", "string"}, {"minLength", 1}}},
                    {"owner", {{"type", "string"}, {"minLength", 1}}},
                    {"kind", {{"type", "string"}, {"enum", {"webhook", "salesforce", "hubspot"}}}}}}}},
            {"roi_model", {{"type", "object"}, {"additionalProperties", false},
                {"required", {"value_per_outcome", "investment", "currency"}}, {"properties", {
                    {"value_per_outcome", {{"type", "number"}}},
                    {"investment", {{"type", "number"}, {"minimum", 0}}},
                    {"currency", {{"type", "string"}, {"minLength", 1}}}}}}},
            {"decision_source", {{"type", "object"}, {"additionalProperties", false},
                {"required", {"methodology", "entity_column", "score_column"}}, {"properties", {
                    {"methodology", {{"type", "string"}, {"minLength", 1}}},
                    {"entity_column", {{"type", "string"}, {"minLength", 1}}},
                    {"score_column", {{"type", "string"}, {"minLength", 1}}},
                    {"output_name", {{"type", "string"}}},
                    {"horizon", {{"type", "number"}, {"exclusiveMinimum", 0}}}}}}}}}};
    return schema;
}

std::string validate_solution_binding(const Json& binding) {
    class Error final : public nlohmann::json_schema::error_handler {
    public:
        std::string reason;

        void error(const Json::json_pointer& path, const Json&, const std::string& message) override {
            if (reason.empty()) {
                reason = path.to_string() + ": " + message;
            }
        }
    };
    Json schema = solution_foundation_schema();
    for (const std::string key : {"scope", "policy_id", "experiment_id", "monitor_id",
                                  "solution"}) {
        schema["required"].push_back(key);
        schema["properties"][key] = {{"type", "string"}, {"minLength", 1}};
    }
    schema["required"].push_back("measurement");
    schema["required"].push_back("decision_source");
    schema["properties"]["measurement"] = solution_measurement_schema();
    schema["properties"]["level"] = {
        {"type", "integer"}, {"minimum", 1}, {"maximum", 4}};
    schema["properties"]["s1_correctness"] = {{"type", "object"}};
    schema["properties"]["s2_correctness"] = {{"type", "object"}};
    schema["properties"]["s6_correctness"] = {{"type", "object"}};
    schema["properties"]["s3_correctness"] = {{"type", "object"}};
    schema["properties"]["s4_correctness"] = {{"type", "object"}};
    schema["properties"]["s12_correctness"] = {{"type", "object"}};
    schema["properties"]["s5_correctness"] = {{"type", "object"}};
    schema["properties"]["s7_correctness"] = {{"type", "object"}};
    schema["properties"]["s8_correctness"] = {{"type", "object"}};
    schema["properties"]["s9_correctness"] = {{"type", "object"}};
    nlohmann::json_schema::json_validator validator(schema);
    Error error;
    validator.validate(binding, error);
    return error.reason;
}

ApiResult create_composed_solution(const Json& input, api::ApiContext& context) {
    const auto project = context.node.find_project(input.at("project"));
    const ml::Solution* solution = ml::find_solution(input.at("solution").get<std::string>());
    if (!project || solution == nullptr) {
        return ApiResult::failure("not_found", "project or solution not found");
    }
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "solution creation requires replicated state");
    }
    const int level = input.at("level");
    const std::string name = input.value("name", solution->id + " CL-" + std::to_string(level));
    if (project->find_graph(name)) {
        return ApiResult::failure("invalid_input", "a graph with this name already exists");
    }
    auto composed = ml::compose_solution(*solution, level, input.at("sources"));
    if (!composed.ok()) {
        return ApiResult::failure("not_supported", composed.error);
    }
    std::string error;
    const Json& settings = input.at("foundation");
    const auto snapshot = SnapshotSpec::from_json(settings.at("snapshot"), &error);
    if (!snapshot) {
        return ApiResult::failure("invalid_input", error);
    }
    const auto database = context.node.find_database(snapshot->database);
    if (!database || !database->find_table(snapshot->source_table)) {
        return ApiResult::failure("not_found", "snapshot source table does not exist");
    }
    bind_sources(composed.graph, *snapshot);
    bind_s1_correctness(composed.graph, *snapshot, settings,
                        input.at("sources"));
    bind_s2_correctness(composed.graph, *snapshot, settings,
                        input.at("sources"));
    bind_s6_correctness(composed.graph, input.at("sources"));
    bind_s3_correctness(composed.graph, settings, input.at("sources"));
    bind_s4_correctness(composed.graph, *snapshot, settings,
                        input.at("sources"), *solution);
    bind_s12_correctness(composed.graph, *solution, settings,
                         input.at("sources"));
    bind_s5_correctness(composed.graph, input.at("sources"));
    bind_s7_correctness(composed.graph, input.at("sources"));
    bind_s8_correctness(composed.graph, *snapshot, settings,
                        input.at("sources"));
    bind_s9_correctness(composed.graph, settings, input.at("sources"));
    composed.graph.find_node("solution_manifest")->attributes["foundation"] =
        settings;
    const pipeline::ValidationReport correctness =
        pipeline::default_gate().run(composed.graph);
    if (!correctness.passed()) {
        for (const pipeline::CheckResult& check : correctness.checks) {
            if (check.status == pipeline::CheckStatus::Fail) {
                return ApiResult::failure(
                    "invalid_input", "solution correctness gate: " +
                                         check.name + ": " + check.reason);
            }
        }
    }
    const auto primary = std::ranges::find_if(solution->composition, [](const auto& entry) {
        return entry.role == "primary";
    });
    if (primary != solution->composition.end() && primary->id != "M-1" &&
        solution->id != "S-5" && !settings.contains("decision_source")) {
        return ApiResult::failure("invalid_input", "this module requires explicit decision_source output and score bindings");
    }
    if (settings.contains("decision_source") &&
        std::ranges::find(composed.methodologies,
            settings.at("decision_source").at("methodology").get<std::string>()) ==
            composed.methodologies.end()) {
        return ApiResult::failure("invalid_input", "decision_source must name a selected methodology");
    }
    const std::string actor = context.acting_key.empty() ? "local" : context.acting_key;
    Json binding = create_bindings(context.node, *project, *solution, name,
                                   level, settings, actor, error);
    if (!error.empty()) {
        return ApiResult::failure("engine_error", error);
    }
    binding["level"] = level;
    if (!binding.contains("decision_source") && solution->id == "S-3") {
        binding["decision_source"] = {
            {"methodology", "M-1"},
            {"entity_column", "entity"},
            {"score_column", "predicted_value"},
            {"output_name", "Write lifetime value predictions"}};
    } else if (!binding.contains("decision_source")) {
        binding["decision_source"] = {{"methodology", "M-1"},
                                       {"entity_column", "account"}, {"score_column", "risk"}};
        if (solution->id == "S-4") {
            binding["decision_source"]["entity_column"] = "receivable";
            binding["decision_source"]["score_column"] = "recovery_score";
            binding["decision_source"]["output_name"] =
                "Prioritize collections treatments";
        } else if (solution->id == "S-12") {
            binding["decision_source"] = {
                {"methodology", "M-1"},
                {"entity_column", "claim_id"},
                {"score_column", "referral_score"},
                {"output_name", "Publish claim referral scores"}};
        } else if (solution->id == "S-5") {
            binding["decision_source"] = {
                {"methodology", "M-4"},
                {"entity_column", "series"},
                {"score_column", "exception_score"},
                {"output_name", "forecast"}};
        } else if (solution->id == "S-9") {
            binding["decision_source"] = {
                {"methodology", "M-1"},
                {"output_name", "outbound response scores"},
                {"entity_column", "entity"},
                {"score_column", "response_propensity"}};
        }
    }
    if (solution->id == "S-1") {
        binding["s1_correctness"] = composed.graph.find_node("solution_manifest")
                                        ->attributes.at("s1_correctness");
        if (level >= 3) {
            binding["measurement"]["design"] = "uplift_cells";
        }
    } else if (solution->id == "S-2") {
        binding["s2_correctness"] = composed.graph.find_node("solution_manifest")
                                        ->attributes.at("s2_correctness");
    } else if (solution->id == "S-3") {
        binding["s3_correctness"] =
            composed.graph.find_node("solution_manifest")
                ->attributes.at("s3_correctness");
    } else if (solution->id == "S-4") {
        binding["s4_correctness"] = composed.graph.find_node("solution_manifest")
                                        ->attributes.at("s4_correctness");
    } else if (solution->id == "S-12") {
        binding["s12_correctness"] =
            composed.graph.find_node("solution_manifest")
                ->attributes.at("s12_correctness");
    } else if (solution->id == "S-5") {
        binding["s5_correctness"] =
            composed.graph.find_node("solution_manifest")
                ->attributes.at("s5_correctness");
    } else if (solution->id == "S-6") {
        binding["s6_correctness"] =
            composed.graph.find_node("solution_manifest")
                ->attributes.at("s6_correctness");
    } else if (solution->id == "S-7") {
        binding["s7_correctness"] = composed.graph.find_node("solution_manifest")
                                        ->attributes.at("s7_correctness");
    } else if (solution->id == "S-8") {
        binding["s8_correctness"] = composed.graph.find_node("solution_manifest")
                                        ->attributes.at("s8_correctness");
    } else if (solution->id == "S-9") {
        binding["s9_correctness"] = composed.graph.find_node("solution_manifest")
                                        ->attributes.at("s9_correctness");
    }
    for (const auto& step : composed.graph.nodes()) {
        const std::string component = step->attributes.value("foundation_component", "");
        if (component == "policy") {
            step->attributes["policy_id"] = binding.at("policy_id");
        } else if (component == "holdout") {
            step->attributes["experiment_id"] = binding.at("experiment_id");
        } else if (component == "actions") {
            step->attributes["delivery"] = binding.at("delivery");
        } else if (component == "monitor") {
            step->attributes["monitor_id"] = binding.at("monitor_id");
        } else if (component == "readout") {
            step->attributes["solution_scope"] = binding.at("scope");
            step->attributes["measurement"] = binding.at("measurement");
        }
    }
    composed.graph.find_node("solution_manifest")->attributes["foundation"] = binding;
    composed.graph.find_node("solution_snapshots")->attributes["snapshot"] = snapshot->to_json();
    const auto graph = project->create_graph(name, std::move(composed.graph));
    if (!graph) {
        return ApiResult::failure("engine_error", "could not persist the composed pipeline");
    }
    const ApiResult readout = configure_readout(context.node, *project, *graph, binding, actor);
    if (!readout.ok) {
        return readout;
    }
    return ApiResult::success({{"project", project->name()}, {"solution", solution->id},
        {"level", level}, {"graph", graph->name()}, {"preview", false},
        {"methodologies", composed.methodologies}, {"missing_components", Json::array()},
        {"policy_id", binding.at("policy_id")}, {"experiment_id", binding.at("experiment_id")},
        {"monitor_id", binding.at("monitor_id")}, {"readout_id", readout.value.at("readout_id")}});
}

ApiResult prepare_solution_snapshot(Node& node, const Json& binding) {
    std::string error;
    const auto spec = SnapshotSpec::from_json(binding.at("snapshot"), &error);
    if (!spec) {
        return ApiResult::failure("invalid_input", error);
    }
    const auto existing = table_at(node, spec->database + "." + spec->target_table);
    if (existing) {
        for (std::size_t index = 0; index < existing->store().size(); ++index) {
            const auto row = existing->store().at(index);
            if (!row || !row->contains("as_of") || row->at("as_of") != spec->as_of) {
                return ApiResult::failure("invalid_input", "solution snapshot table must contain exactly one cutoff");
            }
        }
    }
    const auto snapshot = materialize_snapshot(node, *spec);
    if (!snapshot.ok()) {
        return ApiResult::failure("engine_error", snapshot.error);
    }
    for (std::size_t index = 0; index < snapshot.table->store().size(); ++index) {
        const auto row = snapshot.table->store().at(index);
        if (!row || row->at("as_of") != spec->as_of) {
            return ApiResult::failure("invalid_input", "solution snapshot table must contain exactly one cutoff");
        }
    }
    if (!snapshot.table->partition_manifest() || snapshot.materialized_rows != 0) {
        snapshot.table->set_partitioner(std::make_shared<data::RoundRobinPartitioner>(3));
        const auto database = node.find_database(spec->database);
        const auto written = data::write_partitioned(node, *database, *snapshot.table);
        if (!written.ok()) {
            return ApiResult::failure("engine_error", written.error);
        }
    }
    return ApiResult::success({{"dataset_path", spec->database + "." + spec->target_table},
                               {"future_rows_excluded", snapshot.future_rows_excluded}});
}

namespace {

std::optional<std::vector<Json>> read_rows(Node& node, const data::Table& table,
                                          std::string& error) {
    if (table.partition_only()) {
        query::SelectStmt statement;
        statement.all = true;
        const auto selected = query::select_over_partitions(
            node, node.distributed_select(), statement, table.id());
        if (!selected.ok()) {
            error = selected.error;
            return std::nullopt;
        }
        return selected.rows;
    }
    std::vector<Json> rows;
    for (std::size_t index = 0; index < table.store().size(); ++index) {
        const auto row = table.store().at(index);
        if (!row) {
            error = row.error();
            return std::nullopt;
        }
        rows.push_back(*row);
    }
    return rows;
}

std::optional<std::vector<Json>> score_rows(Node& node, const Json& binding,
                                           const std::vector<pipeline::RunOutput>& outputs,
                                           std::string& error) {
    const Json& mapping = binding.at("decision_source");
    for (const auto& output : outputs) {
        if (output.kind != "dataset" ||
            (mapping.contains("output_name") && output.name != mapping.at("output_name").get<std::string>())) {
            continue;
        }
        const auto table = table_at(node, output.ref);
        if (!table) {
            continue;
        }
        auto rows = read_rows(node, *table, error);
        if (!rows) {
            return std::nullopt;
        }
        if (!rows->empty() && rows->front().contains(mapping.at("entity_column").get<std::string>()) &&
            rows->front().contains(mapping.at("score_column").get<std::string>())) {
            if (binding.value("solution", std::string{}) == "S-8") {
                const std::string entity_column = mapping.at("entity_column");
                const std::string score_column = mapping.at("score_column");
                const std::optional<double> selected_horizon =
                    mapping.contains("horizon")
                        ? std::optional<double>(mapping.at("horizon").get<double>())
                        : std::nullopt;
                std::map<std::string, Json> selected;
                std::set<std::string> scored_entities;
                for (const Json& row : *rows) {
                    if (!row.contains(entity_column) ||
                        !row.contains(score_column) ||
                        !row.at(score_column).is_number() ||
                        !row.contains("horizon") ||
                        !row.at("horizon").is_number()) {
                        error = "S-8 survival output must contain entity, "
                                "numeric risk, and numeric horizon columns";
                        return std::nullopt;
                    }
                    const double horizon = row.at("horizon").get<double>();
                    const std::string key = entity_key(row.at(entity_column));
                    scored_entities.insert(key);
                    if (selected_horizon &&
                        std::abs(horizon - *selected_horizon) > 1e-9) {
                        continue;
                    }
                    const auto existing = selected.find(key);
                    if (existing == selected.end() ||
                        horizon < existing->second.at("horizon").get<double>()) {
                        selected[key] = row;
                    }
                }
                if (selected.size() != scored_entities.size()) {
                    error = "S-8 decision horizon must match exactly one row "
                            "for every asset in the M-5 survival table";
                    return std::nullopt;
                }
                std::vector<Json> decision_rows;
                decision_rows.reserve(selected.size());
                for (auto& entry : selected) {
                    decision_rows.push_back(std::move(entry.second));
                }
                return decision_rows;
            }
            return rows;
        }
    }
    error = "selected methodology produced no table matching decision_source";
    return std::nullopt;
}

std::optional<std::vector<Json>> aggregate_replenishment_forecast(
    const std::vector<Json>& rows,
    const Json& mapping,
    std::string& error) {
    const std::string entity_column = mapping.at("entity_column");
    const std::string score_column = mapping.at("score_column");
    std::map<std::string, Json> totals;
    for (const Json& row : rows) {
        if (!row.contains(entity_column) ||
            !row.contains(score_column) ||
            !row.at(score_column).is_number()) {
            error = "S-6 forecast output must contain an item-location key "
                    "and numeric horizon demand";
            return std::nullopt;
        }
        const std::string key = entity_key(row.at(entity_column));
        auto [found, inserted] = totals.try_emplace(key, row);
        if (!inserted) {
            found->second[score_column] =
                found->second.at(score_column).get<double>() +
                row.at(score_column).get<double>();
        }
    }
    std::vector<Json> aggregated;
    aggregated.reserve(totals.size());
    for (auto& [key, row] : totals) {
        (void)key;
        aggregated.push_back(std::move(row));
    }
    return aggregated;
}

std::optional<std::map<std::string, Json>> uplift_rows(
    Node& node, const Json& binding,
    const std::vector<pipeline::RunOutput>& outputs, std::string& error) {
    std::map<std::string, Json> targeting;
    const std::string solution_id =
        binding.value("solution", std::string{});
    const int minimum_level = solution_id == "S-9" ? 2 : 3;
    if ((solution_id != "S-1" && solution_id != "S-3" && solution_id != "S-4" &&
         solution_id != "S-9") ||
        binding.value("level", 1) < minimum_level) {
        return targeting;
    }
    for (const pipeline::RunOutput& output : outputs) {
        if (output.kind != "dataset" || output.name != "treatment effects") {
            continue;
        }
        const std::shared_ptr<data::Table> table = table_at(node, output.ref);
        if (!table) {
            continue;
        }
        const std::optional<std::vector<Json>> rows =
            read_rows(node, *table, error);
        if (!rows) {
            return std::nullopt;
        }
        for (const Json& row : *rows) {
            if (!row.contains("entity") ||
                !row.contains("estimated_uplift") ||
                !row.at("estimated_uplift").is_number() ||
                !row.contains("recommended") ||
                !row.at("recommended").is_boolean()) {
                error = solution_id + " treatment effects have an invalid row";
                return std::nullopt;
            }
            targeting[entity_key(row.at("entity"))] = row;
        }
        return targeting;
    }
    error = solution_id + " produced no treatment-effects table";
    return std::nullopt;
}

std::optional<std::vector<Json>> s5_decision_entities(
    const std::vector<Json>& forecasts,
    const data::Table& snapshot,
    const Json& binding,
    std::string& error) {
    const Json& correctness = binding.at("s5_correctness");
    const std::string source_series = correctness.at("series_column");
    const std::string source_time = correctness.at("time_column");
    struct TimedEvidence {
        TimePoint time;
        Json row;
    };
    std::map<std::string, TimedEvidence> evidence;
    for (std::size_t index = 0; index < snapshot.store().size(); ++index) {
        const auto row = snapshot.store().at(index);
        if (!row || !row->contains(source_series) ||
            !row->contains(source_time)) {
            error = "S-5 snapshot rows must carry the declared series and time columns";
            return std::nullopt;
        }
        const std::optional<TimePoint> observed =
            parse_time_point(row->at(source_time));
        if (!observed) {
            error = "S-5 snapshot contains an invalid series observation time";
            return std::nullopt;
        }
        const std::string key = entity_key(row->at(source_series));
        const auto current = evidence.find(key);
        if (current == evidence.end() ||
            observed->value > current->second.time.value) {
            evidence.insert_or_assign(
                key, TimedEvidence{*observed, *row});
        }
    }

    const Json& mapping = binding.at("decision_source");
    const std::string output_series = mapping.at("entity_column");
    const std::string output_score = mapping.at("score_column");
    std::map<std::string, Json> highest_uncertainty;
    for (const Json& forecast : forecasts) {
        if (!forecast.contains(output_series) ||
            !forecast.contains(output_score) ||
            !forecast.at(output_score).is_number() ||
            !forecast.contains("timestamp") ||
            !forecast.contains("point_forecast") ||
            !forecast.contains("lower") ||
            !forecast.contains("upper")) {
            error = "S-5 forecast output is missing its series, interval, or exception score";
            return std::nullopt;
        }
        const std::string key = entity_key(forecast.at(output_series));
        const auto current = highest_uncertainty.find(key);
        if (current == highest_uncertainty.end() ||
            forecast.at(output_score).get<double>() >
                current->second.at(output_score).get<double>()) {
            highest_uncertainty.insert_or_assign(key, forecast);
        }
    }

    std::vector<Json> entities;
    entities.reserve(highest_uncertainty.size());
    for (const auto& [key, forecast] : highest_uncertainty) {
        const auto found = evidence.find(key);
        if (found == evidence.end()) {
            error = "S-5 forecast series does not join to snapshot evidence: " + key;
            return std::nullopt;
        }
        Json entity = found->second.row;
        entity["entity_id"] = forecast.at(output_series);
        entity["score"] = forecast.at(output_score);
        for (const std::string field : {
                 "series", "timestamp", "point_forecast", "lower", "upper",
                 "exception_score"}) {
            entity[field] = forecast.at(field);
        }
        entity["experiment"] = binding.at("experiment_id");
        entities.push_back(std::move(entity));
    }
    return entities;
}

bool record_no_contact(Node& node, const DecisionRecord& decision,
                       const Json& binding, const std::string& run_id,
                       const std::string& actor) {
    ActionLedgerValue action;
    action.subject = decision.entity_id;
    action.score_id = decision.id;
    action.score = decision.score;
    action.decision = decision.id;
    action.action = "no_contact";
    action.actor = actor;
    action.model_version = run_id;
    action.policy_version = std::to_string(decision.policy_version);
    action.agent_version = "solution-foundation-v1";
    action.cell = decision.cell;
    action.currency = binding.at("roi_model").at("currency");
    action.score_at = decision.created;
    action.decision_at = decision.created;
    action.action_at = util::now_iso8601();
    return static_cast<bool>(node.record_action(std::move(action)));
}

bool deliver_decision(Node& node, const DecisionRecord& decision,
                      const Json& entity,
                      const Json& config, const Json& binding,
                      const std::string& run_id, const std::string& actor,
                      std::string& error) {
    if (!decision.actionable) {
        if (!record_no_contact(node, decision, binding, run_id, actor)) {
            error = "could not record no-contact ledger entry";
            return false;
        }
        return true;
    }
    const Json& destination = binding.at("delivery");
    const Json action = config.at("actions").at(decision.action);
    ActionTaskSpec spec;
    spec.decision_id = decision.id;
    spec.owner = entity.value("owner", destination.at("owner").get<std::string>());
    spec.sla_due_at = util::iso8601_seconds_ago(-action.value("sla_seconds", 86400LL));
    spec.connector.kind = *action_connector_kind_from(destination.value("kind", "webhook"));
    spec.connector.endpoint = destination.at("endpoint");
    spec.payload = {{"solution", binding.at("scope")}, {"tier", decision.tier},
                    {"action", decision.action}, {"decision_id", decision.id},
                    {"cost", decision.cost},
                    {"currency", binding.at("roi_model").at("currency")}};
    if (binding.value("solution", std::string{}) == "S-3") {
        spec.payload["predicted_value"] = decision.score;
        spec.payload["value_horizon_days"] =
            binding.at("s3_correctness").at("value_horizon_days");
    }
    spec.payload["reason_codes"] =
        entity.value("reason_codes", Json::array());
    spec.payload["decision_cell"] =
        entity.value("decision_cell", Json::object());
    if (binding.value("solution", std::string{}) == "S-2") {
        spec.payload["routing_queue"] = decision.action;
        spec.payload["territory"] =
            entity.value("territory", std::string{});
        spec.payload["rep_capacity_available"] =
            entity.value("rep_capacity_available", false);
        spec.payload["queue_owner"] = spec.owner;
    } else if (binding.value("solution", std::string{}) == "S-12") {
        spec.payload["claim_id"] = decision.entity_id;
        spec.payload["assigned_path"] = decision.action;
        spec.payload["human_denial_only"] = true;
        spec.payload["document_confidence_clear"] =
            entity.value("document_confidence_clear", false);
        spec.payload["fairness_review_passed"] =
            entity.value("fairness_review_passed", false);
    } else if (binding.value("solution", std::string{}) == "S-6") {
        Json proposal = Json::object();
        for (const std::string field :
             {"item_id", "location_id", "supplier_id", "order_quantity",
              "lead_time_days", "service_level_target", "cost"}) {
            if (entity.contains(field)) {
                proposal[field] = entity.at(field);
            }
        }
        spec.payload["order_proposal"] = std::move(proposal);
    } else if (binding.value("solution", std::string{}) == "S-7") {
        spec.payload["forum"] = spec.owner;
        spec.payload["period"] = entity.value("period", 0.0);
        spec.payload["anomaly_score"] = decision.score;
        spec.payload["anomaly_threshold"] =
            entity.value("threshold", 0.0);
        spec.payload["top_driver"] =
            entity.value("top_contributor", std::string{});
        spec.payload["driver_deviation"] =
            entity.value("deviation", 0.0);
        spec.payload["contributors"] =
            entity.value("contributors", Json::array());
        spec.payload["causal_caveat"] =
            binding.at("s7_correctness")
                .at("driver_attribution")
                .at("caveat");
        spec.payload["decision_log_required"] = true;
    } else if (binding.value("solution", std::string{}) == "S-8") {
        spec.payload["asset_id"] = decision.entity_id;
        spec.payload["asset_class"] =
            entity.value("asset_class", std::string{});
        spec.payload["work_order_type"] = decision.action;
        spec.payload["failure_risk"] = decision.score;
        spec.payload["risk_horizon"] = entity.value("horizon", 0.0);
        spec.payload["expected_time_to_failure"] =
            entity.value("expected_time", 0.0);
        spec.payload["median_time_to_failure"] =
            entity.value("median_survival", 0.0);
        spec.payload["maintenance_window"] =
            entity.value("maintenance_window", std::string{});
        spec.payload["cmms_owner"] = spec.owner;
    } else if (binding.value("solution", std::string{}) == "S-9") {
        spec.payload["journey"] = decision.action;
        spec.payload["channel"] =
            entity.value("channel", std::string{});
        spec.payload["consent"] = entity.value("consent", false);
        spec.payload["suppressed"] = entity.value("suppressed", true);
        spec.payload["contacts"] = entity.value("contacts", 0);
        spec.payload["recent_actions"] =
            entity.value("recent_actions", 0);
    }
    for (const std::string field : {
             "series", "timestamp", "point_forecast", "lower", "upper",
             "exception_score"}) {
        if (entity.contains(field)) {
            spec.payload[field] = entity.at(field);
        }
    }
    spec.idempotency_key = decision.id;
    spec.model_version = run_id;
    spec.agent_version = "solution-foundation-v1";
    spec.cell = decision.cell;
    spec.currency = binding.at("roi_model").at("currency");
    ActionRuntime runtime(node);
    const auto task = runtime.enqueue_task(std::move(spec), actor, &error);
    if (!task) {
        return false;
    }
    const auto delivered = runtime.deliver_task(
        task->task_id, binding.value("delivery_token", ""), actor, &error);
    if (!delivered || delivered->status != ActionTaskStatus::Delivered) {
        if (error.empty()) {
            error = "action delivery was not acknowledged; inspect task " + task->task_id;
        }
        return false;
    }
    return true;
}

bool s12_adverse_action(std::string action) {
    std::transform(action.begin(), action.end(), action.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return action.find("denial") != std::string::npos ||
           action.find("deny") != std::string::npos ||
           action.find("reject") != std::string::npos;
}

bool s12_required_rule(const Json& config, const std::string& field) {
    for (const Json& rule :
         config.value("eligibility_rules", Json::array())) {
        if (rule.is_object() &&
            rule.value("field", std::string{}) == field &&
            rule.value("operator", std::string{}) == "eq" &&
            rule.value("value", Json(false)) == Json(true)) {
            return true;
        }
    }
    return false;
}

std::string validate_s12_runtime_policy(const Json& binding,
                                        const Json& config) {
    if (binding.value("solution", std::string{}) != "S-12") {
        return {};
    }
    bool has_manual_denial = false;
    for (const Json& tier : config.value("tiers", Json::array())) {
        if (tier.is_object() &&
            s12_adverse_action(tier.value("action", std::string{}))) {
            return "S-12 refuses a policy that selects automated denial";
        }
    }
    for (const auto& [action, settings] :
         config.value("actions", Json::object()).items()) {
        if (!s12_adverse_action(action)) {
            continue;
        }
        if (!settings.is_object() ||
            settings.value("autonomy", std::string{}) != "A0") {
            return "S-12 adverse outcomes must remain A0 human-only";
        }
        has_manual_denial = true;
    }
    if (!has_manual_denial) {
        return "S-12 policy must retain an A0 denial recommendation";
    }
    if (!s12_required_rule(config, "document_confidence_clear") ||
        !s12_required_rule(config, "fairness_review_passed")) {
        return "S-12 policy must require document confidence and fairness review evidence";
    }
    return {};
}

} // namespace

ApiResult finish_solution_run(Node& node, const Json& binding,
                              const std::string& run_id,
                              const std::vector<pipeline::RunOutput>& scores,
                              const std::string& actor) {
    if (!node.find_monitor(binding.at("monitor_id"))) {
        return ApiResult::failure("not_found", "the solution guardrail monitor no longer exists");
    }
    std::string read_error;
    const auto scored = score_rows(node, binding, scores, read_error);
    if (!scored) {
        return ApiResult::failure("engine_error", read_error);
    }
    std::vector<Json> decision_scores = *scored;
    if (binding.value("solution", std::string{}) == "S-6") {
        const auto aggregated = aggregate_replenishment_forecast(
            decision_scores, binding.at("decision_source"), read_error);
        if (!aggregated) {
            return ApiResult::failure("invalid_input", read_error);
        }
        decision_scores = *aggregated;
    }
    const auto targeting = uplift_rows(node, binding, scores, read_error);
    if (!targeting) {
        return ApiResult::failure("engine_error", read_error);
    }
    if (binding.value("solution", std::string{}) == "S-1") {
        const double threshold =
            binding.at("s1_correctness").value("too_good_auc_threshold", 0.95);
        for (const pipeline::RunOutput& output : scores) {
            if (output.kind != "model") {
                continue;
            }
            const std::shared_ptr<ml::Model> model = node.find_model(output.ref);
            if (!model) {
                continue;
            }
            const Json metrics = model->metrics();
            const double auc = metrics.contains("held_out_auc")
                                   ? metrics.at("held_out_auc").get<double>()
                                   : metrics.value("auc", 0.0);
            if (auc > threshold) {
                return ApiResult::failure(
                    "invalid_input",
                    "S-1 AUC exceeds the too-good threshold; review target "
                    "and post-outcome leakage before actions");
            }
        }
    }
    const Json& snapshot_spec = binding.at("snapshot");
    const std::string snapshot_path = snapshot_spec.at("database").get<std::string>() + "." +
                                      snapshot_spec.at("target_table").get<std::string>();
    const auto snapshot = table_at(node, snapshot_path);
    if (!snapshot) {
        return ApiResult::failure("not_found", "the solution snapshot no longer exists");
    }
    const Json& mapping = binding.at("decision_source");
    std::vector<Json> entities;
    if (binding.value("solution", std::string{}) == "S-5") {
        const auto forecast_entities =
            s5_decision_entities(*scored, *snapshot, binding, read_error);
        if (!forecast_entities) {
            return ApiResult::failure("invalid_input", read_error);
        }
        entities = *forecast_entities;
    } else {
        std::map<std::string, Json> evidence;
        for (std::size_t index = 0; index < snapshot->store().size(); ++index) {
            const auto row = snapshot->store().at(index);
            if (!row) {
                return ApiResult::failure(
                    "engine_error", "could not read snapshot evidence");
            }
            evidence.emplace(entity_key(row->at("entity_id")), *row);
        }
        std::set<std::string> seen;
        for (const Json& row : decision_scores) {
            const std::string entity_column = mapping.at("entity_column");
            const std::string score_column = mapping.at("score_column");
            if (!row.contains(entity_column) ||
                !row.contains(score_column) ||
                !row.at(score_column).is_number()) {
                return ApiResult::failure(
                    "invalid_input",
                    "scored output must contain an entity key and numeric score");
            }
            const std::string key = entity_key(row.at(entity_column));
            if (!evidence.contains(key)) {
                return ApiResult::failure(
                    "invalid_input",
                    "decision output has no snapshot entity " + key);
            }
            if (!seen.insert(key).second) {
                return ApiResult::failure(
                    "invalid_input",
                    "decision output repeats snapshot entity " + key);
            }
            Json entity = evidence.at(key);
            entity["score"] = row.at(score_column);
            for (const std::string field : {
                     "dominant_driver", "driver_effect", "reason_codes",
                     "flagged", "threshold", "top_contributor", "deviation",
                     "contributors", "period"}) {
                if (row.contains(field)) {
                    entity[field] = row.at(field);
                }
            }
            if (binding.value("solution", std::string{}) == "S-7" &&
                !entity.value("flagged", false)) {
                entity["action"] = "no_contact";
            }
            if (binding.value("solution", std::string{}) == "S-8") {
                for (const std::string field :
                     {"horizon", "hazard", "survival", "expected_time",
                      "median_survival"}) {
                    if (row.contains(field)) {
                        entity[field] = row.at(field);
                    }
                }
                entity["failure_risk"] = row.at(score_column);
            }
            if (!targeting->empty()) {
                const auto treatment = targeting->find(key);
                if (treatment == targeting->end()) {
                    return ApiResult::failure(
                        "invalid_input",
                        binding.at("solution").get<std::string>() +
                        " treatment output must join exactly once to "
                        "decision entity " + key);
                }
                entity["estimated_uplift"] =
                    treatment->second.at("estimated_uplift");
                entity["uplift_recommended"] =
                    treatment->second.at("recommended");
                if (!treatment->second.at("recommended").get<bool>() ||
                    treatment->second.at("estimated_uplift").get<double>() <=
                        0.0) {
                    entity["action"] = "no_contact";
                }
            }
            entity["experiment"] = binding.at("experiment_id");
            entities.push_back(std::move(entity));
        }
    }
    std::vector<double> customer_values;
    for (const Json& entity : entities) {
        for (const std::string field : {"customer_value", "mrr", "value"}) {
            if (entity.contains(field) && entity.at(field).is_number()) {
                customer_values.push_back(entity.at(field).get<double>());
                break;
            }
        }
    }
    std::sort(customer_values.begin(), customer_values.end());
    const double low_value = customer_values.empty()
                                 ? 0.0
                                 : customer_values[customer_values.size() / 3];
    const double high_value = customer_values.empty()
                                  ? 0.0
                                  : customer_values[(customer_values.size() * 2) / 3];
    auto database = node.find_database("solution_results");
    if (!database) {
        database = node.create_database("solution_results");
    }
    std::string table_name = "decisions_" + run_id;
    std::erase(table_name, '-');
    const auto decisions = database ? database->create_table(table_name) : nullptr;
    if (!decisions) {
        return ApiResult::failure("engine_error", "could not create solution decision table");
    }
    DecisionPolicyStore policies(*node.replication(), &node, &node.experiment_manager());
    const auto policy = policies.get(binding.at("policy_id"));
    if (!policy) {
        return ApiResult::failure("not_found", "solution policy no longer exists");
    }
    if (const std::string policy_error =
            validate_s12_runtime_policy(binding, policy->current()->config);
        !policy_error.empty()) {
        return ApiResult::failure("invalid_input", policy_error);
    }
    double complaints = 0.0;
    double unsubscribes = 0.0;
    double outbound_consent_violations = 0.0;
    double maximum_contacts = 0.0;
    double consent_violations = 0.0;
    double margin_violations = 0.0;
    double discount_fairness_gap = 0.0;
    double cohort_calibration_error = 0.0;
    double predicted_value_total = 0.0;
    double regulatory_contact_violations = 0.0;
    double vulnerable_customer_contacts = 0.0;
    double automated_denials = 0.0;
    double low_confidence_path_assignments = 0.0;
    double fairness_review_failures = 0.0;
    std::size_t actionable = 0;
    std::size_t delivered = 0;
    std::size_t capacity_violations = 0;
    std::size_t anomaly_alerts = 0;
    std::size_t sensitive_access_violations = 0;
    std::size_t unsafe_work_orders = 0;
    std::size_t false_work_orders = 0;
    std::size_t missed_failures = 0;
    double maximum_sensor_drift = 0.0;
    double maximum_capacity_utilization = 0.0;
    double service_level_total = 0.0;
    std::size_t service_level_count = 0;
    double maximum_inventory_exposure = 0.0;
    double maximum_order_volatility = 0.0;
    std::size_t unapproved_over_bound_orders = 0;
    std::map<std::string, std::size_t> driver_counts;
    std::set<std::string> reportable_forecast_series;
    std::size_t protected_forecast_series = 0;
    double stale_forecasts = 0.0;
    double stockout_censored = 0.0;
    double unaudited_overrides = 0.0;
    Json decision_log = Json::array();
    std::string model_version = "run:" + run_id;
    Json forecast_metrics = Json::object();
    for (const auto& output : scores) {
        if (output.kind == "model") {
            model_version = output.ref;
            if (const std::shared_ptr<ml::Model> model =
                    node.find_model(output.ref)) {
                forecast_metrics = model->metrics();
            }
            break;
        }
    }
    for (Json& entity : entities) {
        std::string error;
        const auto decision = policies.decide(policy->id, entity, actor, &error);
        if (!decision) {
            return ApiResult::failure("engine_error", error);
        }
        std::string value_tier = entity.value("value_tier", std::string{});
        if (value_tier.empty()) {
            std::optional<double> customer_value;
            for (const std::string field : {"customer_value", "mrr", "value"}) {
                if (entity.contains(field) && entity.at(field).is_number()) {
                    customer_value = entity.at(field).get<double>();
                    break;
                }
            }
            if (!customer_value) {
                value_tier = "unclassified";
            } else if (*customer_value >= high_value) {
                value_tier = "high";
            } else if (*customer_value >= low_value) {
                value_tier = "medium";
            } else {
                value_tier = "low";
            }
        }
        std::string driver = entity.value(
            "dominant_driver",
            entity.value("top_contributor", "unclassified"));
        if (binding.value("solution", std::string{}) == "S-7" &&
            !entity.value("sensitive_driver_access", false)) {
            driver = "redacted";
        }
        if (binding.value("solution", std::string{}) == "S-2") {
            entity["decision_cell"] = {
                {"conversion_band", decision->tier},
                {"value", value_tier},
                {"territory", entity.value("territory", std::string{})},
                {"queue_owner",
                 entity.value("owner",
                              binding.at("delivery").at("owner").get<std::string>())}};
        } else if (binding.value("solution", std::string{}) == "S-3") {
            entity["decision_cell"] = {
                {"value_band", decision->tier},
                {"segment", entity.value("segment", "unclassified")},
                {"driver", driver}};
        } else if (binding.value("solution", std::string{}) == "S-5") {
            entity["decision_cell"] = {
                {"priority", decision->tier},
                {"series", entity.at("series")},
                {"driver", "interval_uncertainty"}};
        } else if (binding.value("solution", std::string{}) == "S-7") {
            entity["decision_cell"] = {
                {"severity", decision->tier},
                {"period", entity.value("period", 0.0)},
                {"driver", driver},
                {"flagged", entity.value("flagged", false)}};
        } else if (binding.value("solution", std::string{}) == "S-8") {
            entity["decision_cell"] = {
                {"failure_risk", decision->tier},
                {"asset_class",
                 entity.value("asset_class", std::string{})},
                {"risk_horizon", entity.value("horizon", 0.0)},
                {"expected_time_to_failure",
                 entity.value("expected_time", 0.0)}};
        } else if (binding.value("solution", std::string{}) == "S-9") {
            entity["decision_cell"] = {
                {"audience_tier", decision->tier},
                {"segment", entity.value("segment_id", -1)},
                {"journey", decision->action.empty()
                                ? "no_contact"
                                : decision->action},
                {"channel", entity.value("channel", std::string{})}};
        } else {
            entity["decision_cell"] = {{"risk", decision->tier},
                                       {"value", value_tier},
                                       {"driver", driver}};
        }
        Json decision_row = to_json(*decision);
        decision_row["decision_cell"] = entity.at("decision_cell");
        decision_row["reason_codes"] =
            entity.value("reason_codes", Json::array());
        for (const std::string field : {
                 "series", "timestamp", "point_forecast", "lower", "upper",
                 "exception_score"}) {
            if (entity.contains(field)) {
                decision_row[field] = entity.at(field);
            }
        }
        if (entity.contains("estimated_uplift")) {
            decision_row["estimated_uplift"] =
                entity.at("estimated_uplift");
            decision_row["uplift_recommended"] =
                entity.at("uplift_recommended");
        }
        decisions->add_row(std::move(decision_row));
        if (decision->actionable) {
            ++actionable;
            if (binding.value("solution", std::string{}) == "S-2" &&
                !entity.value("rep_capacity_available", false)) {
                ++capacity_violations;
            }
            if (s12_adverse_action(decision->action)) {
                automated_denials += 1.0;
            }
            if (!entity.value("document_confidence_clear", false)) {
                low_confidence_path_assignments += 1.0;
            }
            if (!entity.value("fairness_review_passed", false)) {
                fairness_review_failures += 1.0;
            }
            if (binding.value("solution", std::string{}) == "S-8") {
                const bool unsafe =
                    entity.value("safety_lockout", false) ||
                    !entity.value("safe_to_service", false) ||
                    !entity.value("maintenance_window_available", false);
                unsafe_work_orders += unsafe ? 1u : 0u;
                false_work_orders +=
                    entity.value("false_work_order", false) ? 1u : 0u;
            }
        }
        if (!deliver_decision(node, *decision, entity,
                              policy->current()->config,
                              binding, model_version, actor, error)) {
            return ApiResult::failure("engine_error", error);
        }
        if (decision->actionable) {
            ++delivered;
            const bool regulatory_allowed =
                entity.value("collections_eligible", false) &&
                entity.value("regulatory_contact_allowed", false) &&
                entity.value("within_contact_hours", false);
            if (!regulatory_allowed) {
                regulatory_contact_violations += 1.0;
            }
            if (entity.value("vulnerable", true)) {
                vulnerable_customer_contacts += 1.0;
            }
        }
        if (binding.value("solution", std::string{}) == "S-7" &&
            entity.value("flagged", false)) {
            ++anomaly_alerts;
            if (!entity.value("sensitive_driver_access", false)) {
                ++sensitive_access_violations;
            }
        }
        ++driver_counts[driver];
        decision_log.push_back({{"decision_id", decision->id},
                                {"entity_id", decision->entity_id},
                                {"score", decision->score},
                                {"status", decision->status},
                                {"action", decision->action.empty()
                                               ? "no_contact"
                                               : decision->action},
                                {"cell", entity.at("decision_cell")},
                                {"reasons", decision->reasons}});
        if (binding.value("solution", std::string{}) == "S-5") {
            const std::string series = entity_key(entity.at("series"));
            if (decision->holdout) {
                ++protected_forecast_series;
            } else if (decision->status != "blocked") {
                reportable_forecast_series.insert(series);
            }
            stale_forecasts +=
                entity.value("forecast_fresh", false) ? 0.0 : 1.0;
            stockout_censored +=
                entity.value("stockout_censored", false) ? 1.0 : 0.0;
            if (entity.value("manual_override", false) &&
                entity.value("override_reason", std::string{}).empty()) {
                unaudited_overrides += 1.0;
            }
        }
        if (binding.value("solution", std::string{}) == "S-9") {
            complaints +=
                entity.value("complaint", Json(false)) == Json(true)
                    ? 1.0
                    : 0.0;
            unsubscribes +=
                entity.value("unsubscribed", Json(false)) == Json(true)
                    ? 1.0
                    : 0.0;
            if (decision->actionable &&
                (!entity.value("consent", false) ||
                 entity.value("suppressed", true))) {
                outbound_consent_violations += 1.0;
            }
        } else {
            complaints +=
                entity.value("open_complaint", Json(false)) == Json(true)
                    ? 1.0
                    : 0.0;
        }
        predicted_value_total += entity.value("score", 0.0);
        if (decision->actionable && !entity.value("consent", false)) {
            consent_violations += 1.0;
        }
        if (decision->actionable && entity.value("margin", 0.0) < 0.2) {
            margin_violations += 1.0;
        }
        discount_fairness_gap = std::max(
            discount_fairness_gap,
            std::abs(entity.value("discount_fairness_gap", 0.0)));
        cohort_calibration_error = std::max(
            cohort_calibration_error,
            std::abs(entity.value("cohort_calibration_error", 0.0)));
        maximum_contacts = std::max(maximum_contacts,
            entity.value("recent_actions", 0.0) + (decision->actionable ? 1.0 : 0.0));
        if (binding.value("solution", std::string{}) == "S-6") {
            if (entity.contains("service_level") &&
                entity.at("service_level").is_number()) {
                service_level_total += entity.at("service_level").get<double>();
                ++service_level_count;
            }
            maximum_inventory_exposure = std::max(
                maximum_inventory_exposure,
                entity.value("inventory_exposure", 0.0));
            maximum_order_volatility = std::max(
                maximum_order_volatility,
                entity.value("order_volatility", 0.0));
            const double automatic_bound =
                binding.at("s6_correctness")
                    .at("automatic_release_max_quantity")
                    .get<double>();
            if (entity.value("order_quantity", 0.0) > automatic_bound &&
                decision->action == "release_order" &&
                decision->autonomy != "A1") {
                ++unapproved_over_bound_orders;
            }
        }
        if (binding.value("solution", std::string{}) == "S-8") {
            if (entity.value("failure_within_horizon", false) &&
                !decision->actionable && !decision->holdout) {
                ++missed_failures;
            }
            maximum_sensor_drift = std::max(
                maximum_sensor_drift,
                entity.value("sensor_drift_psi", 0.0));
            maximum_capacity_utilization = std::max(
                maximum_capacity_utilization,
                entity.value("maintenance_capacity_utilization", 0.0));
        }
    }
    double cost = 0.0;
    std::map<std::string, std::size_t> monthly_contacts;
    const std::string month_start = util::iso8601_seconds_ago(30LL * 86400);
    for (const auto& decision : policies.decisions()) {
        if (decision.policy_id != policy->id || decision.holdout ||
            (decision.status != "allowed" && decision.status != "approved" &&
             decision.status != "pending_approval")) {
            continue;
        }
        if (decision.policy_version == policy->current()->version) {
            cost += decision.cost;
        }
        if (decision.created >= month_start) {
            maximum_contacts = std::max(maximum_contacts,
                static_cast<double>(++monthly_contacts[decision.entity_id]));
        }
    }
    if (const auto monitor = node.find_monitor(binding.at("monitor_id"))) {
        const double execution_rate =
            actionable == 0
                ? 1.0
                : static_cast<double>(delivered) /
                      static_cast<double>(actionable);
        const double entity_count = static_cast<double>(entities.size());
        if (binding.value("solution", std::string{}) == "S-2") {
            monitor->observe_guardrail("sla_miss_rate", 0.0);
            monitor->observe_guardrail(
                "capacity_violation_rate",
                actionable == 0
                    ? 0.0
                    : static_cast<double>(capacity_violations) /
                          static_cast<double>(actionable));
            monitor->observe_guardrail("routing_coverage_rate",
                                       execution_rate);
            monitor->observe_guardrail("action_execution_rate",
                                       execution_rate);
        } else if (binding.value("solution", std::string{}) == "S-3") {
            const double policy_budget =
                policy->current()->config.at("budget").at("limit");
            monitor->observe_guardrail("consent_violations",
                                       consent_violations);
            monitor->observe_guardrail("margin_violations",
                                       margin_violations);
            monitor->observe_guardrail("discount_fairness_gap",
                                       discount_fairness_gap);
            monitor->observe_guardrail(
                "budget_utilization",
                policy_budget == 0.0 ? 0.0 : cost / policy_budget);
            monitor->observe_guardrail("cohort_calibration_error",
                                       cohort_calibration_error);
        } else if (binding.value("solution", std::string{}) == "S-4") {
            monitor->observe_guardrail(
                "regulatory_contact_violations",
                regulatory_contact_violations);
            monitor->observe_guardrail(
                "vulnerable_customer_contacts",
                vulnerable_customer_contacts);
            monitor->observe_guardrail(
                "contact_frequency", maximum_contacts);
            monitor->observe_guardrail("complaints", complaints / entities.size());
            monitor->observe_guardrail("action_execution_rate", execution_rate);
        } else if (binding.value("solution", std::string{}) == "S-12") {
            monitor->observe_guardrail("automated_denials",
                                       automated_denials);
            monitor->observe_guardrail(
                "low_confidence_path_assignments",
                low_confidence_path_assignments);
            monitor->observe_guardrail("fairness_review_failures",
                                       fairness_review_failures);
            for (const pipeline::RunOutput& output : scores) {
                if (output.kind != "model") {
                    continue;
                }
                const std::shared_ptr<ml::Model> model =
                    node.find_model(output.ref);
                if (!model || model->kind() != "document_extraction") {
                    continue;
                }
                const Json& metrics = model->metrics();
                if (metrics.contains("field_precision") &&
                    metrics.at("field_precision").is_number()) {
                    monitor->observe_guardrail(
                        "field_extraction_accuracy",
                        metrics.at("field_precision").get<double>());
                }
            }
            monitor->observe_guardrail("action_execution_rate",
                                       execution_rate);
        } else if (binding.value("solution", std::string{}) == "S-5") {
            monitor->observe_guardrail(
                "forecast_value_added",
                forecast_metrics.value("forecast_value_added", 0.0));
            monitor->observe_guardrail(
                "interval_coverage",
                forecast_metrics.value("interval_coverage", 0.0));
            monitor->observe_guardrail(
                "stale_forecast_fraction",
                entity_count == 0.0 ? 0.0
                                    : stale_forecasts / entity_count);
            monitor->observe_guardrail(
                "stockout_censored_fraction",
                entity_count == 0.0 ? 0.0
                                    : stockout_censored / entity_count);
            monitor->observe_guardrail(
                "unaudited_override_fraction",
                entity_count == 0.0 ? 0.0
                                    : unaudited_overrides / entity_count);
        } else if (binding.value("solution", std::string{}) == "S-6") {
            monitor->observe_guardrail("replenishment_spend", cost);
            monitor->observe_guardrail(
                "service_level",
                service_level_count == 0
                    ? 0.0
                    : service_level_total /
                          static_cast<double>(service_level_count));
            monitor->observe_guardrail("inventory_exposure",
                                       maximum_inventory_exposure);
            monitor->observe_guardrail("order_volatility",
                                       maximum_order_volatility);
            monitor->observe_guardrail(
                "unapproved_over_bound_orders",
                static_cast<double>(unapproved_over_bound_orders));
        } else if (binding.value("solution", std::string{}) == "S-7") {
            monitor->observe_guardrail(
                "alert_volume", static_cast<double>(anomaly_alerts));
            monitor->observe_guardrail(
                "sensitive_driver_access_violations",
                static_cast<double>(sensitive_access_violations));
            monitor->observe_guardrail("causal_claim_violations", 0.0);
            monitor->observe_guardrail("action_execution_rate",
                                       execution_rate);
        } else if (binding.value("solution", std::string{}) == "S-8") {
            monitor->observe_guardrail(
                "unsafe_work_order_rate",
                actionable == 0
                    ? 0.0
                    : static_cast<double>(unsafe_work_orders) /
                          static_cast<double>(actionable));
            monitor->observe_guardrail(
                "false_work_order_rate",
                actionable == 0
                    ? 0.0
                    : static_cast<double>(false_work_orders) /
                          static_cast<double>(actionable));
            monitor->observe_guardrail(
                "missed_failure_rate",
                entities.empty()
                    ? 0.0
                    : static_cast<double>(missed_failures) /
                          static_cast<double>(entities.size()));
            monitor->observe_guardrail("sensor_drift_psi",
                                       maximum_sensor_drift);
            monitor->observe_guardrail(
                "maintenance_capacity_utilization",
                maximum_capacity_utilization);
            monitor->observe_guardrail("action_execution_rate",
                                       execution_rate);
        } else if (binding.value("solution", std::string{}) == "S-9") {
            const double entity_count =
                static_cast<double>(entities.size());
            monitor->observe_guardrail(
                "unsubscribe_rate", unsubscribes / entity_count);
            monitor->observe_guardrail(
                "complaint_rate", complaints / entity_count);
            monitor->observe_guardrail(
                "consent_violation_rate", outbound_consent_violations / entity_count);
            monitor->observe_guardrail("contact_frequency",
                                       maximum_contacts);
            monitor->observe_guardrail("action_execution_rate",
                                       execution_rate);
        } else {
            monitor->observe_guardrail("complaints",
                                       complaints / entities.size());
            monitor->observe_guardrail("discount_budget", cost);
            monitor->observe_guardrail("contact_fatigue",
                                       maximum_contacts);
            monitor->observe_guardrail("action_execution_rate",
                                       execution_rate);
        }
    }
    std::string publication_id;
    const std::string solution_id =
        binding.value("solution", std::string{});
    if ((solution_id == "S-1" || solution_id == "S-3" || solution_id == "S-7") &&
        binding.at("delivery").value("kind", "webhook") == "webhook") {
        Json drivers = Json::array();
        for (const auto& [driver, count] : driver_counts) {
            drivers.push_back({{"driver", driver}, {"accounts", count}});
        }
        ReportPublicationSpec report;
        report.run_id = run_id;
        report.title = solution_id == "S-3"
                           ? "Customer lifetime value allocation report"
                           : solution_id == "S-7"
                               ? "KPI anomaly and candidate-driver report"
                               : "Churn and retention driver report";
        report.driver_report = {{"drivers", std::move(drivers)}};
        if (solution_id == "S-3") {
            report.driver_report["predicted_value_total"] =
                predicted_value_total;
            report.driver_report["value_horizon_days"] =
                binding.at("s3_correctness").at("value_horizon_days");
        } else if (solution_id == "S-7") {
            report.driver_report["alert_budget"] =
                binding.at("s7_correctness")
                    .at("alerting")
                    .at("alert_budget");
            report.driver_report["alerts"] = anomaly_alerts;
            report.driver_report["causal_caveat"] =
                binding.at("s7_correctness")
                    .at("driver_attribution")
                    .at("caveat");
        }
        report.decision_log = std::move(decision_log);
        report.connector.kind = ActionConnectorKind::Webhook;
        report.connector.endpoint = binding.at("delivery").at("endpoint");
        report.connector.token = binding.value("delivery_token", "");
        report.idempotency_key = run_id +
            (solution_id == "S-3" ? "/allocation-report"
                                   : solution_id == "S-7"
                                       ? "/decision-forum-report"
                                       : "/driver-report");
        report.model_version = model_version;
        report.policy_version =
            std::to_string(policy->current()->version);
        report.agent_version = "solution-foundation-v1";
        report.cell = binding.at("scope").get<std::string>() + "/report";
        report.currency = binding.at("roi_model").at("currency");
        std::string error;
        ActionRuntime runtime(node);
        const auto publication =
            runtime.publish_report(std::move(report), actor, &error);
        if (!publication ||
            publication->status != ActionTaskStatus::Delivered) {
            return ApiResult::failure(
                "engine_error", error.empty()
                                    ? "driver report delivery was not acknowledged"
                                    : error);
        }
        publication_id = publication->publication_id;
    }
    if (binding.value("solution", std::string{}) == "S-5") {
        Json forecast_rows = Json::array();
        for (const Json& forecast : *scored) {
            const std::string series = entity_key(forecast.at("series"));
            if (reportable_forecast_series.contains(series)) {
                forecast_rows.push_back(forecast);
            }
        }
        ReportPublicationSpec report;
        report.run_id = run_id;
        report.title = "Demand forecast planner report";
        report.driver_report = {
            {"forecast_rows", std::move(forecast_rows)},
            {"forecast_metrics", forecast_metrics},
            {"protected_series", protected_forecast_series}};
        report.decision_log = std::move(decision_log);
        report.connector.kind = *action_connector_kind_from(
            binding.at("delivery").value("kind", "webhook"));
        report.connector.endpoint = binding.at("delivery").at("endpoint");
        report.connector.token = binding.value("delivery_token", "");
        report.idempotency_key = run_id + "/planner-report";
        report.model_version = model_version;
        report.policy_version =
            std::to_string(policy->current()->version);
        report.agent_version = "solution-foundation-v1";
        report.cell = binding.at("scope").get<std::string>() + "/report";
        report.currency = binding.at("roi_model").at("currency");
        std::string error;
        ActionRuntime runtime(node);
        const auto publication =
            runtime.publish_report(std::move(report), actor, &error);
        if (!publication ||
            publication->status != ActionTaskStatus::Delivered) {
            return ApiResult::failure(
                "engine_error",
                error.empty()
                    ? "planner report delivery was not acknowledged"
                    : error);
        }
        publication_id = publication->publication_id;
    }
    return ApiResult::success({{"decisions", "solution_results." + table_name},
                               {"rows", entities.size()},
                               {"driver_report", publication_id}});
}

} // namespace zetta::solutions
