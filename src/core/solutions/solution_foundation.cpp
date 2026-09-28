#include "zetta/solutions/solution_execution.hpp"

#include <cstdint>
#include <algorithm>
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
                     const Json& settings, const std::string& actor,
                     std::string& error) {
    const std::string scope = project.id() + "/" + name;
    DecisionPolicyStore policies(*node.replication(), &node, &node.experiment_manager());
    const auto policy = policies.create(scope, solution.templates.policy, actor, &error);
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
    const bool s1 = manifest &&
                    manifest->attributes.value("solution", std::string{}) ==
                        "S-1";
    for (const auto& step : graph.nodes()) {
        if (step->kind != ir::StepKind::Connect ||
            !step->attributes.contains("solution_stage")) {
            continue;
        }
        step->attributes["source"] = snapshot.database;
        step->attributes["table"] = snapshot.target_table;
        step->attributes["source_kind"] = "snapshot";
        if (snapshot.as_of.is_string()) {
            step->attributes["snapshot_as_of"] = snapshot.as_of;
        } else if (s1) {
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
                    {"output_name", {{"type", "string"}}}}}}}}}};
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
    schema["properties"]["s3_correctness"] = {{"type", "object"}};
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
    bind_s3_correctness(composed.graph, settings, input.at("sources"));
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
        !settings.contains("decision_source")) {
        return ApiResult::failure("invalid_input", "this module requires explicit decision_source output and score bindings");
    }
    if (settings.contains("decision_source") &&
        std::ranges::find(composed.methodologies,
            settings.at("decision_source").at("methodology").get<std::string>()) ==
            composed.methodologies.end()) {
        return ApiResult::failure("invalid_input", "decision_source must name a selected methodology");
    }
    const std::string actor = context.acting_key.empty() ? "local" : context.acting_key;
    Json binding = create_bindings(context.node, *project, *solution, name, settings, actor, error);
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
    }
    if (solution->id == "S-1") {
        binding["s1_correctness"] = composed.graph.find_node("solution_manifest")
                                        ->attributes.at("s1_correctness");
        if (level >= 3) {
            binding["measurement"]["design"] = "uplift_cells";
        }
    } else if (solution->id == "S-3") {
        binding["s3_correctness"] =
            composed.graph.find_node("solution_manifest")
                ->attributes.at("s3_correctness");
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
            return rows;
        }
    }
    error = "selected methodology produced no table matching decision_source";
    return std::nullopt;
}

std::optional<std::map<std::string, Json>> uplift_rows(
    Node& node, const Json& binding,
    const std::vector<pipeline::RunOutput>& outputs, std::string& error) {
    std::map<std::string, Json> targeting;
    const std::string solution_id =
        binding.value("solution", std::string{});
    if ((solution_id != "S-1" && solution_id != "S-3") ||
        binding.value("level", 1) < 3) {
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
                error = solution_id +
                        " level 3 treatment effects have an invalid row";
                return std::nullopt;
            }
            targeting[entity_key(row.at("entity"))] = row;
        }
        return targeting;
    }
    error = solution_id + " level 3 produced no treatment-effects table";
    return std::nullopt;
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
    std::map<std::string, Json> evidence;
    for (std::size_t index = 0; index < snapshot->store().size(); ++index) {
        const auto row = snapshot->store().at(index);
        if (!row) {
            return ApiResult::failure("engine_error", "could not read snapshot evidence");
        }
        evidence.emplace(entity_key(row->at("entity_id")), *row);
    }
    const Json& mapping = binding.at("decision_source");
    std::vector<Json> entities;
    std::set<std::string> seen;
    for (const Json& row : *scored) {
        const std::string entity_column = mapping.at("entity_column");
        const std::string score_column = mapping.at("score_column");
        if (!row.contains(entity_column) || !row.contains(score_column) ||
            !row.at(score_column).is_number()) {
            return ApiResult::failure("invalid_input", "scored output must contain an entity key and numeric score");
        }
        const std::string key = entity_key(row.at(entity_column));
        if (!evidence.contains(key) || !seen.insert(key).second) {
            return ApiResult::failure("invalid_input", "decision output must join exactly once to snapshot entity " + key);
        }
        Json entity = evidence.at(key);
        entity["score"] = row.at(score_column);
        for (const std::string field : {"dominant_driver", "driver_effect",
                                        "reason_codes"}) {
            if (row.contains(field)) {
                entity[field] = row.at(field);
            }
        }
        if (!targeting->empty()) {
            const auto treatment = targeting->find(key);
            if (treatment == targeting->end()) {
                return ApiResult::failure(
                    "invalid_input",
                    binding.at("solution").get<std::string>() +
                    " level 3 treatment output must join exactly once to "
                    "decision entity " + key);
            }
            entity["estimated_uplift"] =
                treatment->second.at("estimated_uplift");
            entity["uplift_recommended"] =
                treatment->second.at("recommended");
            if (!treatment->second.at("recommended").get<bool>() ||
                treatment->second.at("estimated_uplift").get<double>() <= 0.0) {
                entity["action"] = "no_contact";
            }
        }
        entity["experiment"] = binding.at("experiment_id");
        entities.push_back(std::move(entity));
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
    double complaints = 0.0;
    double maximum_contacts = 0.0;
    double consent_violations = 0.0;
    double margin_violations = 0.0;
    double discount_fairness_gap = 0.0;
    double cohort_calibration_error = 0.0;
    double predicted_value_total = 0.0;
    std::size_t actionable = 0;
    std::size_t delivered = 0;
    std::map<std::string, std::size_t> driver_counts;
    Json decision_log = Json::array();
    std::string model_version = "run:" + run_id;
    for (const auto& output : scores) {
        if (output.kind == "model") {
            model_version = output.ref;
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
        const std::string driver =
            entity.value("dominant_driver", "unclassified");
        if (binding.value("solution", std::string{}) == "S-3") {
            entity["decision_cell"] = {
                {"value_band", decision->tier},
                {"segment", entity.value("segment", "unclassified")},
                {"driver", driver}};
        } else {
            entity["decision_cell"] = {{"risk", decision->tier},
                                       {"value", value_tier},
                                       {"driver", driver}};
        }
        Json decision_row = to_json(*decision);
        decision_row["decision_cell"] = entity.at("decision_cell");
        decision_row["reason_codes"] =
            entity.value("reason_codes", Json::array());
        if (entity.contains("estimated_uplift")) {
            decision_row["estimated_uplift"] =
                entity.at("estimated_uplift");
            decision_row["uplift_recommended"] =
                entity.at("uplift_recommended");
        }
        decisions->add_row(std::move(decision_row));
        if (decision->actionable) {
            ++actionable;
        }
        if (!deliver_decision(node, *decision, entity,
                              policy->current()->config,
                              binding, model_version, actor, error)) {
            return ApiResult::failure("engine_error", error);
        }
        if (decision->actionable) {
            ++delivered;
        }
        ++driver_counts[driver];
        decision_log.push_back({{"entity_id", decision->entity_id},
                                {"action", decision->action.empty()
                                               ? "no_contact"
                                               : decision->action},
                                {"cell", entity.at("decision_cell")}});
        complaints += entity.value("open_complaint", Json(false)) == Json(true) ? 1.0 : 0.0;
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
        if (binding.value("solution", std::string{}) == "S-3") {
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
        } else {
            monitor->observe_guardrail("complaints",
                                       complaints / entities.size());
            monitor->observe_guardrail("discount_budget", cost);
            monitor->observe_guardrail("contact_fatigue",
                                       maximum_contacts);
            monitor->observe_guardrail(
                "action_execution_rate",
                actionable == 0
                    ? 1.0
                    : static_cast<double>(delivered) /
                          static_cast<double>(actionable));
        }
    }
    std::string publication_id;
    const std::string solution_id =
        binding.value("solution", std::string{});
    if ((solution_id == "S-1" || solution_id == "S-3") &&
        binding.at("delivery").value("kind", "webhook") == "webhook") {
        Json drivers = Json::array();
        for (const auto& [driver, count] : driver_counts) {
            drivers.push_back({{"driver", driver}, {"accounts", count}});
        }
        ReportPublicationSpec report;
        report.run_id = run_id;
        report.title = solution_id == "S-3"
                           ? "Customer lifetime value allocation report"
                           : "Churn and retention driver report";
        report.driver_report = {{"drivers", std::move(drivers)}};
        if (solution_id == "S-3") {
            report.driver_report["predicted_value_total"] =
                predicted_value_total;
            report.driver_report["value_horizon_days"] =
                binding.at("s3_correctness").at("value_horizon_days");
        }
        report.decision_log = std::move(decision_log);
        report.connector.kind = ActionConnectorKind::Webhook;
        report.connector.endpoint = binding.at("delivery").at("endpoint");
        report.connector.token = binding.value("delivery_token", "");
        report.idempotency_key = run_id +
            (solution_id == "S-3" ? "/allocation-report"
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
    return ApiResult::success({{"decisions", "solution_results." + table_name},
                               {"rows", entities.size()},
                               {"driver_report", publication_id}});
}

} // namespace zetta::solutions
