// AutoML studies on the control surface (#1138, #1158, #1207;
// docs/ml/studies.md).

#include <algorithm>
#include <set>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/metric_names.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/ml/workspace_methodology.hpp"
#include "zetta/ml/workspace_study.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/step_authority.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json study_reference() {
    return {{"type", "string"}, {"minLength", 1}};
}

const nlohmann::json& create_study_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document =
            zetta::ml::study_value_schema().at("properties").at("spec");
        // objective, budgets and search_space may be omitted with
        // from_defaults: true, which fills what is missing (#1217); without
        // it the handler reports each missing field with its path.
        document["required"] = {"name", "methodology"};
        document["properties"]["objective"].erase("required");
        document["properties"]["budgets"].erase("required");
        document["properties"]["from_defaults"] = {
            {"type", "boolean"},
            {"description",
             "fill a missing objective (held_out_auc, maximize, validation "
             "split), budgets and the methodology's default parameter ranges"}};
        return document;
    }();
    return schema;
}

const nlohmann::json& validate_study_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"valid", "reason", "path", "normalized_spec"}},
        {"additionalProperties", false},
        {"properties",
         {{"valid", {{"type", "boolean"}}},
          {"reason", {{"type", "string"}}},
          {"path", {{"type", "string"}}},
          {"normalized_spec",
           {{"oneOf",
             nlohmann::json::array(
                 {zetta::ml::study_value_schema().at("properties").at("spec"),
                  nlohmann::json{{"type", "null"}}})}}}}}};
    return schema;
}

// A workspace methodology's study (lake#689): the trials waiting for Lake's
// runner. Present only on such a study.
nlohmann::json runner_work_schema() {
    return {{"type", "array"},
            {"items", zetta::ml::workspace_study_work_schema()},
            {"description",
             "a workspace methodology's study only: each trial waiting for Lake's runner, "
             "which runs it and reports back with record_methodology_run (study, trial)"}};
}

const nlohmann::json& study_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document = zetta::ml::study_value_schema();
        document["required"].push_back("id");
        document["properties"]["runner"] = runner_work_schema();
        return document;
    }();
    return schema;
}

const nlohmann::json& get_study_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study"}},
        {"additionalProperties", false},
        {"properties", {{"study", study_reference()}}}};
    return schema;
}

const nlohmann::json& list_studies_schema() {
    static const nlohmann::json schema = {
        {"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_studies_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json item = study_output_schema();
        item["properties"]["name"] = {{"type", "string"}};
        item["properties"]["project_name"] = {{"type", "string"}};
        item["properties"]["pipeline_name"] = {{"type", "string"}};
        item["properties"]["metric_summary"] = {
            {"type", "object"},
            {"required", {"metric", "direction", "completed_trials", "best"}},
            {"additionalProperties", false},
            {"properties",
             {{"metric", {{"type", "string"}}},
              {"direction", {{"type", "string"}}},
              {"completed_trials", {{"type", "integer"}, {"minimum", 0}}},
              {"best", {{"type", {"number", "null"}}}}}}};
        for (const char* field : {"name", "project_name", "pipeline_name",
                                  "metric_summary"}) {
            item["required"].push_back(field);
        }
        return nlohmann::json{
            {"type", "object"},
            {"required", {"studies"}},
            {"additionalProperties", false},
            {"properties", {{"studies", {{"type", "array"}, {"items", item}}}}}};
    }();
    return schema;
}

nlohmann::json trial_schema() {
    return zetta::ml::study_value_schema()
        .at("properties")
        .at("trials")
        .at("items");
}

const nlohmann::json& list_trials_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"state",
           {{"type", "string"},
            {"enum", {"running", "pruned", "failed", "complete"}}}}}}};
    return schema;
}

const nlohmann::json& list_trials_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "trials"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"trials", {{"type", "array"}, {"items", trial_schema()}}}}}};
    return schema;
}

const nlohmann::json& stop_study_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "status", "reason"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"status", {{"type", "string"}, {"const", "stopped"}}},
          {"reason", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& delete_study_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "name", "removed", "kept"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"name", {{"type", "string"}}},
          {"removed",
           {{"type", "object"},
            {"required", {"runs"}},
            {"additionalProperties", false},
            {"properties",
             {{"runs",
               {{"type", "array"},
                {"items", study_reference()},
                {"description",
                 "non-promoted trial Runs removed with their embedded metrics"}}}}}}},
          {"kept",
           {{"type", "object"},
            {"required", {"model", "run"}},
            {"additionalProperties", false},
            {"properties",
             {{"model",
               {{"type", "string"},
                {"description", "the promoted Model retained after deletion"}}},
              {"run",
               {{"type", "string"},
                {"description",
                 "the retained Run that produced the promoted Model"}}}}}}}}}};
    return schema;
}

const nlohmann::json& promote_trial_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "run"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()}, {"run", study_reference()}}}};
    return schema;
}

const nlohmann::json& stop_trial_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "run"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"run", study_reference()},
          {"reason",
           {{"type", "string"},
            {"maxLength", 2000},
            {"description",
             "why the trial ended, kept as its reason; Lake's runner names a run that failed "
             "or was stopped (default: stopped by caller)"}}}}}};
    return schema;
}

const nlohmann::json& stop_trial_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "run", "state"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"run", study_reference()},
          {"state", {{"type", "string"}, {"const", "failed"}}},
          {"status",
           {{"type", "string"},
            {"enum", {"running", "stopped", "complete", "failed"}},
            {"description", "the study's status after the trial ended"}}},
          {"runner", runner_work_schema()}}}};
    return schema;
}

const nlohmann::json& promote_trial_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "run", "model", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"run", study_reference()},
          {"model", study_reference()},
          {"project", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& analysis_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document = nlohmann::json::parse(R"JSON({
      "type":"object",
      "required":["best_trial","optimization_history","parameter_importances","parallel_coordinates","leaderboard"],
      "additionalProperties":false,
      "properties":{
        "best_trial":{"type":["object","null"],"additionalProperties":false,"properties":{"run":{"type":"string"},"number":{"type":"integer"},"value":{"type":"number"},"params":{"type":"object"}},"required":["run","number","value","params"]},
        "optimization_history":{"type":"array","items":{"type":"object","required":["number","run","value","best"],"additionalProperties":false,"properties":{"number":{"type":"integer"},"run":{"type":"string"},"value":{"type":"number"},"best":{"type":"number"}}}},
        "parameter_importances":{"type":"array","items":{"type":"object","required":["parameter","importance"],"additionalProperties":false,"properties":{"parameter":{"type":"string"},"importance":{"type":"number"}}}},
        "parallel_coordinates":{"type":"array","items":{"type":"object","required":["run","params","value"],"additionalProperties":false,"properties":{"run":{"type":"string"},"params":{"type":"object"},"value":{"type":"number"}}}},
        "leaderboard":{"type":"array","items":{"type":"object","required":["rank","run","value","confidence_low","confidence_high"],"additionalProperties":false,"properties":{"rank":{"type":"integer"},"run":{"type":"string"},"value":{"type":"number"},"confidence_low":{"type":"number"},"confidence_high":{"type":"number"}}}}
      }
    })JSON");
        const nlohmann::json params = trial_schema().at("properties").at("params");
        document["properties"]["best_trial"]["properties"]["params"] = params;
        document["properties"]["parallel_coordinates"]["items"]
                ["properties"]["params"] = params;
        return document;
    }();
    return schema;
}

nlohmann::json with_id(zetta::Node& node, const std::shared_ptr<zetta::ml::Study>& study) {
    nlohmann::json output = study->metadata();
    output["id"] = study->id();
    if (zetta::ml::is_workspace_study(study->spec())) {
        output["runner"] = zetta::ml::workspace_study_runner_work(node, *study);
    }
    return output;
}

// A workspace methodology's stored entry, or nullopt.
std::optional<nlohmann::json> workspace_entry(zetta::Node& node, const std::string& id) {
    if (node.replication() == nullptr) return std::nullopt;
    const zetta::ml::WorkspaceMethodologyStore store(*node.replication());
    return store.get(id);
}

struct StudyParameterDefinition {
    zetta::ml::StudyParameter parameter;
    nlohmann::json default_value;
    std::vector<std::string> aliases;
};

struct StudyMethodologyDefinition {
    std::string methodology;
    std::vector<StudyParameterDefinition> parameters;
    std::vector<std::string> objective_metrics;
    std::vector<std::string> validation_splits;
};

StudyParameterDefinition numeric_parameter(
    std::string name, zetta::ml::ParameterKind kind, double minimum,
    double maximum, double step, bool logarithmic,
    nlohmann::json default_value, std::vector<std::string> aliases = {}) {
    zetta::ml::StudyParameter parameter;
    parameter.name = std::move(name);
    parameter.kind = kind;
    parameter.minimum = minimum;
    parameter.maximum = maximum;
    parameter.step = step;
    parameter.logarithmic = logarithmic;
    parameter.condition_parameter = "model_family";
    parameter.condition_values = {"gbdt"};
    return {std::move(parameter), std::move(default_value),
            std::move(aliases)};
}

const StudyMethodologyDefinition& m1_study_definition() {
    static const StudyMethodologyDefinition definition = [] {
        StudyMethodologyDefinition value;
        value.methodology = "M-1";
        zetta::ml::StudyParameter family;
        family.name = "model_family";
        family.kind = zetta::ml::ParameterKind::Categorical;
        family.choices = {"gbdt"};
        value.parameters.push_back({std::move(family), "gbdt", {}});
        value.parameters.push_back(numeric_parameter(
            "gbdt.num_trees", zetta::ml::ParameterKind::Integer, 10, 200,
            10, false, 100));
        value.parameters.push_back(numeric_parameter(
            "gbdt.max_depth", zetta::ml::ParameterKind::Integer, 2, 8, 1,
            false, 4));
        value.parameters.push_back(numeric_parameter(
            "gbdt.max_bins", zetta::ml::ParameterKind::Integer, 8, 256, 8,
            false, 32));
        value.parameters.push_back(numeric_parameter(
            "learning_rate", zetta::ml::ParameterKind::Float, 0.001, 0.5,
            0.0, true, 0.3, {"gbdt.learning_rate"}));
        value.parameters.push_back(numeric_parameter(
            "l2_regularization", zetta::ml::ParameterKind::Float, 0.000001,
            10.0, 0.0, true, 1.0,
            {"gbdt.l2_regularization"}));
        value.parameters.push_back(numeric_parameter(
            "min_split_gain", zetta::ml::ParameterKind::Float, 0.0,
            10.0, 0.0, false, 0.0, {"gbdt.min_split_gain"}));
        value.parameters.push_back(numeric_parameter(
            "min_child_hessian", zetta::ml::ParameterKind::Float, 0.000001,
            10.0, 0.0, true, 0.001,
            {"gbdt.min_child_hessian"}));
        value.parameters.push_back(numeric_parameter(
            "holdout_fraction", zetta::ml::ParameterKind::Float, 0.01, 0.5,
            0.0, false, 0.2, {"gbdt.holdout_fraction"}));
        // Canonical held-out metric names (#1217); aliases such as
        // validation.auc or ROC-AUC normalize to these on input.
        value.objective_metrics = {
            "held_out_accuracy", "held_out_auc", "held_out_log_loss"};
        value.validation_splits = {"validation"};
        return value;
    }();
    return definition;
}

const StudyMethodologyDefinition* study_definition(
    std::string_view methodology) {
    const zetta::ml::Methodology* resolved =
        zetta::ml::find_methodology(methodology);
    if (resolved != nullptr && resolved->id == "M-1") {
        return &m1_study_definition();
    }
    return nullptr;
}

const StudyParameterDefinition* parameter_definition(
    const StudyMethodologyDefinition& methodology,
    std::string_view name) {
    for (const StudyParameterDefinition& definition : methodology.parameters) {
        if (definition.parameter.name == name ||
            std::ranges::contains(definition.aliases, name)) {
            return &definition;
        }
    }
    return nullptr;
}

// Each objective metric's accepted input aliases, keyed by canonical name.
nlohmann::json objective_metric_aliases(
    const StudyMethodologyDefinition& methodology) {
    nlohmann::json aliases = nlohmann::json::object();
    for (const zetta::ml::MetricName& metric : zetta::ml::metric_names()) {
        if (std::ranges::contains(methodology.objective_metrics,
                                  metric.canonical)) {
            aliases[metric.canonical] = metric.aliases;
        }
    }
    return aliases;
}

nlohmann::json parameter_definition_json(
    const StudyParameterDefinition& definition) {
    const zetta::ml::StudyParameter& parameter = definition.parameter;
    return {{"name", parameter.name},
            {"kind", zetta::ml::to_string(parameter.kind)},
            {"minimum", parameter.minimum},
            {"maximum", parameter.maximum},
            {"step", parameter.step},
            {"log", parameter.logarithmic},
            {"default", definition.default_value},
            {"choices", parameter.choices},
            {"aliases", definition.aliases},
            {"condition_parameter", parameter.condition_parameter},
            {"condition_values", parameter.condition_values}};
}

std::vector<zetta::ml::StudyParameter> default_space(
    const StudyMethodologyDefinition& methodology) {
    std::vector<zetta::ml::StudyParameter> parameters;
    parameters.reserve(methodology.parameters.size());
    for (const StudyParameterDefinition& definition : methodology.parameters) {
        parameters.push_back(definition.parameter);
    }
    return parameters;
}

bool validate_study_dataset(zetta::Node& node, const zetta::ml::StudySpec& spec,
                            std::string& reason) {
    if (spec.dataset.empty()) return true;
    const zetta::data::DatasetPath path = zetta::data::parse_dataset_path(spec.dataset);
    if (path.kind != zetta::data::DatasetPathKind::Table) {
        reason = "/dataset: study datasets must name database.table";
        return false;
    }
    const zetta::data::ResolvedDataset dataset = zetta::data::resolve_dataset(node, path);
    if (!dataset.ok()) {
        reason = "/dataset: " + dataset.error;
        return false;
    }
    return true;
}

bool validate_driver_spec(zetta::Node& node, zetta::ml::StudySpec& spec,
                          std::string& reason) {
    const bool has_project = !spec.project.empty();
    const bool has_pipeline = !spec.pipeline.empty();
    if (has_project != has_pipeline) {
        reason = "/project: project and pipeline must be supplied together";
        return false;
    }
    if (!has_project && spec.dataset.empty()) {
        reason = "/dataset: a dataset-level study requires database.table";
        return false;
    }
    if (zetta::ml::is_workspace_methodology_id(spec.methodology)) {
        const std::optional<nlohmann::json> entry =
            workspace_entry(node, spec.methodology);
        if (!entry) {
            reason = "/methodology: no methodology '" + spec.methodology + "' in this workspace";
            return false;
        }
        if (const std::optional<std::string> problem =
                zetta::ml::normalize_workspace_study_spec(*entry, spec)) {
            reason = *problem;
            return false;
        }
        return validate_study_dataset(node, spec, reason);
    }
    const zetta::ml::Methodology* methodology =
        zetta::ml::find_methodology(spec.methodology);
    const StudyMethodologyDefinition* definition =
        methodology != nullptr ? study_definition(methodology->id) : nullptr;
    if (methodology == nullptr || definition == nullptr) {
        reason = "/methodology: the study driver currently supports M-1; '" +
                 spec.methodology + "' cannot run real study trials";
        return false;
    }
    spec.methodology = methodology->id;
    if (!std::ranges::contains(definition->validation_splits,
                               spec.objective.validation_split)) {
        reason = "/objective/validation_split: M-1 supports the 'validation' split";
        return false;
    }
    if (const std::optional<std::string> canonical =
            zetta::ml::canonical_metric_name(spec.objective.metric)) {
        spec.objective.metric = *canonical;
    }
    if (!std::ranges::contains(definition->objective_metrics,
                               spec.objective.metric)) {
        reason = "/objective/metric: unsupported M-1 validation metric '" +
                 spec.objective.metric + "'; use one of";
        for (const std::string& metric : definition->objective_metrics) {
            reason += " " + metric;
        }
        return false;
    }
    std::set<std::string> normalized_names;
    for (zetta::ml::StudyParameter& parameter : spec.search_space) {
        const StudyParameterDefinition* accepted =
            parameter_definition(*definition, parameter.name);
        if (accepted == nullptr) {
            reason = "/search_space/" + parameter.name +
                     ": unknown M-1 hyperparameter";
            return false;
        }
        parameter.name = accepted->parameter.name;
        if (!normalized_names.insert(parameter.name).second) {
            reason = "/search_space/" + parameter.name +
                     ": parameter appears more than once after normalization";
            return false;
        }
        if (parameter.kind != accepted->parameter.kind) {
            reason = "/search_space/" + parameter.name +
                     "/kind: expected '" +
                     std::string(zetta::ml::to_string(accepted->parameter.kind)) +
                     "'";
            return false;
        }
        if (!parameter.condition_parameter.empty()) {
            const StudyParameterDefinition* parent = parameter_definition(
                *definition, parameter.condition_parameter);
            if (parent != nullptr) {
                parameter.condition_parameter = parent->parameter.name;
            }
        }
        for (nlohmann::json& value : parameter.condition_values) {
            if (value == "gradient_boosting") value = "gbdt";
        }
        if (parameter.kind != zetta::ml::ParameterKind::Categorical) continue;
        for (nlohmann::json& choice : parameter.choices) {
            if (choice == "gradient_boosting") choice = "gbdt";
            if (!std::ranges::contains(accepted->parameter.choices, choice)) {
                const std::string rendered =
                    choice.is_string() ? choice.get<std::string>() : choice.dump();
                reason = parameter.name == "model_family"
                             ? "/search_space/model_family/choices: unsupported family '" +
                                   rendered + "'"
                             : "/search_space/" + parameter.name +
                                   "/choices: unsupported choice '" + rendered + "'";
                return false;
            }
        }
        std::ranges::sort(parameter.choices,
                          [](const nlohmann::json& left,
                             const nlohmann::json& right) {
                              return left.dump() < right.dump();
                          });
        const auto unique = std::ranges::unique(parameter.choices);
        parameter.choices.erase(unique.begin(), unique.end());
    }
    return validate_study_dataset(node, spec, reason);
}

nlohmann::json default_objective() {
    return {{"metric", "held_out_auc"},
            {"direction", "maximize"},
            {"validation_split", "validation"}};
}

// Ten trials, two at a time, for at most ten minutes; cost is bounded by the
// wall time alone (0 = no cost cap).
nlohmann::json default_budgets() {
    return {{"max_trials", 10},
            {"max_wall_time_ms", 600000},
            {"max_parallel_trials", 2},
            {"cost_cap", 0.0}};
}

nlohmann::json search_space_json(const std::vector<zetta::ml::StudyParameter>& parameters) {
    nlohmann::json items = nlohmann::json::array();
    for (const zetta::ml::StudyParameter& parameter : parameters) {
        items.push_back({{"name", parameter.name},
                         {"kind", zetta::ml::to_string(parameter.kind)},
                         {"minimum", parameter.minimum},
                         {"maximum", parameter.maximum},
                         {"step", parameter.step},
                         {"log", parameter.logarithmic},
                         {"choices", parameter.choices},
                         {"condition_parameter", parameter.condition_parameter},
                         {"condition_values", parameter.condition_values}});
    }
    return items;
}

std::optional<zetta::ml::StudySpec> spec_from_input(
    zetta::Node& node, const nlohmann::json& input, std::string& reason) {
    const std::string methodology_id =
        input.at("methodology").get<std::string>();
    // A workspace methodology (lake#689): its declared parameters, its first
    // held-out metric and its held-out rows are the defaults.
    std::optional<zetta::ml::WorkspaceStudyDefinition> workspace;
    if (zetta::ml::is_workspace_methodology_id(methodology_id)) {
        const std::optional<nlohmann::json> entry = workspace_entry(node, methodology_id);
        if (!entry) {
            reason = "/methodology: no methodology '" + methodology_id + "' in this workspace";
            return std::nullopt;
        }
        workspace = zetta::ml::workspace_study_definition(*entry);
        if (!workspace->reason.empty()) {
            reason = "/methodology: " + workspace->reason;
            return std::nullopt;
        }
    }
    const zetta::ml::Methodology* methodology =
        workspace ? nullptr : zetta::ml::find_methodology(methodology_id);
    if (!workspace && (methodology == nullptr || !methodology->runnable)) {
        reason = "/methodology: no runnable methodology named '" +
                 methodology_id + "'";
        return std::nullopt;
    }
    nlohmann::json spec = input;
    spec.erase("from_defaults");
    const bool from_defaults = input.value("from_defaults", false);
    // objective and budgets: each missing field is either filled from the
    // defaults or reported at its own path (#1217).
    const auto fill = [&](const char* object, const nlohmann::json& defaults) {
        if (!spec.contains(object)) {
            if (!from_defaults) {
                reason = std::string("/") + object +
                         ": required";
                return false;
            }
            spec[object] = nlohmann::json::object();
        }
        for (const auto& [field, value] : defaults.items()) {
            if (spec[object].contains(field)) continue;
            if (!from_defaults) {
                reason = std::string("/") + object + "/" + field +
                         ": required";
                return false;
            }
            spec[object][field] = value;
        }
        return true;
    };
    const nlohmann::json objective_defaults =
        workspace ? nlohmann::json{{"metric", workspace->default_objective.metric},
                                   {"direction",
                                    zetta::ml::to_string(workspace->default_objective.direction)},
                                   {"validation_split",
                                    workspace->default_objective.validation_split}}
                  : default_objective();
    if (!fill("objective", objective_defaults) ||
        !fill("budgets", default_budgets())) {
        return std::nullopt;
    }
    spec["project"] = input.value("project", std::string{});
    spec["pipeline"] = input.value("pipeline", std::string{});
    spec["dataset"] = input.value("dataset", std::string{});
    spec["sampler"] = input.value("sampler", std::string{"tpe"});
    spec["pruner"] = input.value("pruner", std::string{workspace ? "none" : "asha"});
    spec["seed"] = input.value("seed", std::uint64_t{0});
    spec["differential_privacy"] =
        input.value("differential_privacy", false) ||
        (methodology != nullptr && methodology->id == "M-20");
    spec["released_metrics"] =
        input.value("released_metrics", std::vector<std::string>{});
    if (!input.contains("search_space")) {
        if (!input.value("from_defaults", false)) {
            reason = "/search_space: required";
            return std::nullopt;
        }
        if (workspace) {
            spec["search_space"] = search_space_json(workspace->parameters);
        }
    }
    if (!spec.contains("search_space")) {
        nlohmann::json parameters = nlohmann::json::array();
        const StudyMethodologyDefinition* definition =
            study_definition(methodology->id);
        if (definition == nullptr) {
            reason = "/methodology: the study driver currently supports M-1; '" +
                     methodology->id + "' cannot run real study trials";
            return std::nullopt;
        }
        for (const zetta::ml::StudyParameter& parameter :
             default_space(*definition)) {
            parameters.push_back(
                {{"name", parameter.name},
                 {"kind", zetta::ml::to_string(parameter.kind)},
                 {"minimum", parameter.minimum},
                 {"maximum", parameter.maximum},
                 {"step", parameter.step},
                 {"log", parameter.logarithmic},
                 {"choices", parameter.choices},
                 {"condition_parameter", parameter.condition_parameter},
                 {"condition_values", parameter.condition_values}});
        }
        spec["search_space"] = std::move(parameters);
    }
    nlohmann::json document = {
        {"spec", std::move(spec)},
        {"status", "running"},
        {"created", zetta::util::now_iso8601()},
        {"updated", zetta::util::now_iso8601()},
        {"trials", nlohmann::json::array()},
        {"spent_cost", 0.0},
        {"stop_reason", ""},
        {"promoted_model", ""}};
    const std::optional<zetta::ml::StudyValue> parsed =
        zetta::ml::parse_study_value(document, &reason);
    if (!parsed) return std::nullopt;
    return parsed->spec;
}

struct StudyValidation {
    std::optional<zetta::ml::StudySpec> spec;
    std::string code;
    std::string reason;
    std::string path;
};

StudyValidation validation_failure(std::string code, std::string reason,
                                   std::string path = {}) {
    if (path.empty() && reason.starts_with('/')) {
        const std::size_t separator = reason.find(": ");
        if (separator != std::string::npos) {
            path = reason.substr(0, separator);
            reason.erase(0, separator + 2);
        }
    }
    return {std::nullopt, std::move(code), std::move(reason),
            std::move(path)};
}

// The from_defaults offer (#1217). An input that does not already ask for
// the defaults is told, in the error it hits, that from_defaults: true fills
// the objective, budgets and search space — so the first rejection names the
// one-step way through instead of revealing the rules one call at a time.
std::string from_defaults_hint(const nlohmann::json& input) {
    if (input.is_object()) {
        const auto found = input.find("from_defaults");
        if (found != input.end() && found->is_boolean() &&
            found->get<bool>()) {
            return {};
        }
    }
    return "set from_defaults: true to fill a missing objective "
           "(held_out_auc, maximize, validation split), budgets and the M-1 "
           "search space";
}

// Errors under the fields from_defaults can fill carry the offer; a name
// clash or a missing dataset does not.
StudyValidation with_from_defaults_hint(StudyValidation failure,
                                        const nlohmann::json& input) {
    if (failure.spec) return failure;
    const std::string hint = from_defaults_hint(input);
    const bool fillable = failure.path.empty() ||
                          failure.path.starts_with("/objective") ||
                          failure.path.starts_with("/budgets") ||
                          failure.path.starts_with("/search_space");
    if (!hint.empty() && fillable) {
        failure.reason += "; " + hint;
    }
    return failure;
}

nlohmann::json normalized_spec_json(const zetta::ml::StudySpec& spec) {
    zetta::ml::StudyValue value;
    value.spec = spec;
    return zetta::ml::to_json(value).at("spec");
}

std::shared_ptr<zetta::pipeline::Project> project_for(
    zetta::Node& node, const std::string& reference) {
    for (const auto& project : node.projects()) {
        if (project->id() == reference || project->name() == reference) {
            return project;
        }
    }
    return nullptr;
}

StudyValidation validate_and_normalize_study(const nlohmann::json& input,
                                             ApiContext& context) {
    std::string reason;
    std::optional<zetta::ml::StudySpec> spec =
        spec_from_input(context.node, input, reason);
    if (!spec) {
        return validation_failure("invalid_input", std::move(reason));
    }
    if (!validate_driver_spec(context.node, *spec, reason)) {
        return validation_failure("invalid_input", std::move(reason));
    }
    if (!spec->project.empty() || !spec->pipeline.empty()) {
        const auto project = project_for(context.node, spec->project);
        if (!project) {
            return validation_failure("not_found", "project was not found",
                                      "/project");
        }
        const auto graph = project->find_graph_by_id(spec->pipeline)
                               ? project->find_graph_by_id(spec->pipeline)
                               : project->find_graph(spec->pipeline);
        if (!graph) {
            return validation_failure("not_found", "pipeline was not found",
                                      "/pipeline");
        }
        const zetta::pipeline::ValidationReport report =
            zetta::pipeline::default_gate().run(graph->graph());
        // A workspace methodology's pipeline: the graph must run it; the
        // pure gate's methodology check never passes a python:W- step, and
        // the entry's own validation was checked above (lake#689).
        const bool workspace = zetta::ml::is_workspace_study(*spec);
        if (workspace &&
            zetta::ml::workspace_methodology_of_graph(graph->graph()) != spec->methodology) {
            return validation_failure("invalid_input",
                                      "the pipeline does not run the workspace methodology " +
                                          spec->methodology,
                                      "/pipeline");
        }
        const bool blocked =
            workspace ? std::ranges::any_of(report.checks,
                                            [](const zetta::pipeline::CheckResult& check) {
                                                return check.name != "methodology" &&
                                                       check.status ==
                                                           zetta::pipeline::CheckStatus::Fail;
                                            })
                      : report.block_deploy;
        if (blocked) {
            return validation_failure(
                "invalid_input",
                "the pipeline failed Verification and cannot be studied",
                "/pipeline");
        }
        spec->project = project->name();
        spec->pipeline = graph->name();
    }
    if (context.node.find_study(spec->name)) {
        return validation_failure("invalid_input",
                                  "a study with this name already exists",
                                  "/name");
    }
    if (!zetta::ml::validate_study_spec(*spec, &reason)) {
        return validation_failure("invalid_input", std::move(reason));
    }
    if (!zetta::ml::is_workspace_study(*spec) &&
        !zetta::ml::validate_study_data(context.node, *spec, &reason)) {
        return validation_failure("invalid_input", std::move(reason),
                                  "/dataset");
    }
    StudyValidation result;
    result.spec = std::move(spec);
    return result;
}

void publish_study_event(zetta::Node& node,
                         const std::shared_ptr<zetta::ml::Study>& study,
                         std::string event, std::string run = {}) {
    zetta::pubsub::PubSub* broker = node.pubsub();
    if (broker == nullptr) return;
    const zetta::pubsub::Identity engine{
        zetta::pubsub::Address{"service", "studies"}, {}};
    const zetta::pubsub::Address topic{
        "topic", "study-events." + study->id()};
    if (!broker->topic_settings(topic)) {
        zetta::pubsub::TopicSettings settings;
        settings.exposed = true;
        settings.lifecycle = zetta::pubsub::TopicLifecycle::Ephemeral;
        std::string reason;
        broker->create_topic(engine, topic, settings, &reason);
    }
    zetta::pubsub::PublishRequest request;
    request.to = {topic};
    request.encoding = zetta::pubsub::Encoding::Json;
    request.mime_type = "application/json";
    request.correlation_id = study->id();
    request.payload = nlohmann::json{{"study", study->id()},
                                     {"event", std::move(event)},
                                     {"run", std::move(run)},
                                     {"status", zetta::ml::to_string(study->status())},
                                     {"time", zetta::util::now_iso8601()}}
                          .dump();
    broker->publish(engine, std::move(request));
}

ApiResult handle_create_study(const nlohmann::json& input,
                              ApiContext& context) {
    StudyValidation validation = with_from_defaults_hint(
        validate_and_normalize_study(input, context), input);
    if (!validation.spec) {
        return ApiResult::failure(validation.code, validation.reason,
                                  validation.path);
    }
    std::string reason;
    // The study records its creator: every trial runs as them (#1322).
    const auto study = context.node.create_study(
        *validation.spec, &reason,
        zetta::pipeline::RunPrincipal{context.acting_key, context.acting_is_user});
    if (!study) {
        const StudyValidation failure =
            validation_failure("invalid_input", std::move(reason));
        return ApiResult::failure(failure.code, failure.reason, failure.path);
    }
    publish_study_event(context.node, study, "created");
    if (zetta::ml::is_workspace_study(study->spec())) {
        zetta::ml::advance_workspace_study(context.node, *study);
    }
    return ApiResult::success(with_id(context.node, study));
}

ApiResult handle_validate_study(const nlohmann::json& input,
                                ApiContext& context) {
    StudyValidation validation = with_from_defaults_hint(
        validate_and_normalize_study(input, context), input);
    if (!validation.spec) {
        return ApiResult::success(
            {{"valid", false},
             {"reason", validation.reason},
             {"path", validation.path},
             {"normalized_spec", nullptr}});
    }
    return ApiResult::success(
        {{"valid", true},
         {"reason", ""},
         {"path", ""},
         {"normalized_spec", normalized_spec_json(*validation.spec)}});
}

ApiResult handle_get_study(const nlohmann::json& input,
                           ApiContext& context) {
    const auto study =
        context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    return ApiResult::success(with_id(context.node, study));
}

ApiResult handle_list_studies(const nlohmann::json&, ApiContext& context) {
    nlohmann::json studies = nlohmann::json::array();
    for (const auto& study : context.node.studies()) {
        std::optional<double> best;
        std::size_t completed_trials = 0;
        for (const zetta::ml::StudyTrial& trial : study->value().trials) {
            if (trial.state != zetta::ml::TrialState::Complete || !trial.objective) {
                continue;
            }
            ++completed_trials;
            const bool improves = !best ||
                (study->spec().objective.direction == zetta::ml::StudyDirection::Maximize
                     ? *trial.objective > *best
                     : *trial.objective < *best);
            if (improves) best = trial.objective;
        }
        nlohmann::json item = with_id(context.node, study);
        item["name"] = study->spec().name;
        item["project_name"] = study->spec().project;
        item["pipeline_name"] = study->spec().pipeline;
        item["metric_summary"] = {
            {"metric", zetta::ml::canonical_metric_name(
                           study->spec().objective.metric)
                           .value_or(study->spec().objective.metric)},
            {"direction", std::string(to_string(study->spec().objective.direction))},
            {"completed_trials", completed_trials},
            {"best", best ? nlohmann::json(*best) : nlohmann::json(nullptr)}};
        studies.push_back(std::move(item));
    }
    return ApiResult::success({{"studies", std::move(studies)}});
}

ApiResult handle_list_trials(const nlohmann::json& input,
                             ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    const std::string state = input.value("state", std::string{});
    nlohmann::json trials = nlohmann::json::array();
    for (const nlohmann::json& trial : study->metadata().at("trials")) {
        if (state.empty() || trial.at("state") == state) trials.push_back(trial);
    }
    return ApiResult::success({{"study", study->id()}, {"trials", std::move(trials)}});
}

ApiResult handle_stop_study(const nlohmann::json& input,
                            ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    std::vector<std::string> running;
    for (const zetta::ml::StudyTrial& trial : study->value().trials) {
        if (trial.state == zetta::ml::TrialState::Running) {
            running.push_back(trial.run);
        }
    }
    context.node.request_stop_study_work(study->id());
    study->stop("stopped");
    for (const std::string& run_id : running) {
        if (const auto run = context.node.find_run(run_id)) {
            run->set_status(zetta::pipeline::RunStatus::Failed);
        }
        publish_study_event(context.node, study, "trial_failed", run_id);
    }
    publish_study_event(context.node, study, "stopped");
    return ApiResult::success({{"study", study->id()},
                               {"status", "stopped"},
                               {"reason", study->value().stop_reason}});
}

ApiResult handle_delete_study(const nlohmann::json& input,
                              ApiContext& context) {
    const std::string reference = input.at("study").get<std::string>();
    if (!context.node.find_study(reference)) {
        return ApiResult::failure("not_found", "study was not found", "/study");
    }
    std::string reason;
    const std::optional<zetta::ml::StudyDeleteResult> deleted =
        context.node.delete_study(reference, &reason);
    if (!deleted) {
        return ApiResult::failure("engine_error", reason, "/study");
    }
    return ApiResult::success(
        {{"study", deleted->id},
         {"name", deleted->name},
         {"removed", {{"runs", deleted->removed_runs}}},
         {"kept", {{"model", deleted->kept_model},
                   {"run", deleted->kept_run}}}});
}

ApiResult handle_stop_trial(const nlohmann::json& input,
                            ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    const std::string run_id = input.at("run").get<std::string>();
    std::string reason;
    context.node.request_stop_study_work(study->id(), run_id);
    const std::string why = input.value("reason", std::string{});
    if (!study->stop_trial(run_id, why.empty() ? "stopped by caller" : why, &reason)) {
        return ApiResult::failure("invalid_input", reason, "/run");
    }
    if (const auto run = context.node.find_run(run_id)) {
        if (!why.empty()) {
            run->add_log({zetta::util::now_iso8601(), "error", "trial ended: " + why});
        }
        run->set_status(zetta::pipeline::RunStatus::Failed);
    }
    publish_study_event(context.node, study, "trial_failed", run_id);
    nlohmann::json output = {{"study", study->id()}, {"run", run_id}, {"state", "failed"}};
    // A workspace methodology's study admits its next trial when one ends.
    if (zetta::ml::is_workspace_study(study->spec())) {
        zetta::ml::advance_workspace_study(context.node, *study);
        output["runner"] = zetta::ml::workspace_study_runner_work(context.node, *study);
    }
    output["status"] = zetta::ml::to_string(study->status());
    return ApiResult::success(std::move(output));
}

ApiResult handle_promote_trial(const nlohmann::json& input,
                               ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    const std::string run_id = input.at("run").get<std::string>();
    const auto trial = std::ranges::find(study->value().trials, run_id,
                                          &zetta::ml::StudyTrial::run);
    if (trial == study->value().trials.end() ||
        trial->state != zetta::ml::TrialState::Complete) {
        return ApiResult::failure("invalid_input", "only a complete trial can be promoted", "/run");
    }
    std::shared_ptr<zetta::ml::Model> model;
    if (!trial->model.empty()) model = context.node.find_model(trial->model);
    if (!model) {
        const std::vector<std::shared_ptr<zetta::ml::Model>> models =
            context.node.models();
        const auto found = std::ranges::find_if(
            models, [&](const auto& candidate) {
                return candidate->run() == run_id;
            });
        if (found != models.end()) model = *found;
    }
    if (!model) {
        return ApiResult::failure("not_found", "the trial Run has no registered model", "/run");
    }
    if (!study->spec().project.empty() &&
        model->project() != study->spec().project &&
        model->project_name() != study->spec().project) {
        return ApiResult::failure("invalid_input", "the trial model belongs to another project", "/run");
    }
    study->set_promoted_model(model->id());
    publish_study_event(context.node, study, "trial_promoted", run_id);
    return ApiResult::success({{"study", study->id()},
                               {"run", run_id},
                               {"model", model->id()},
                               {"project", model->project()}});
}

ApiResult handle_analysis(const nlohmann::json& input, ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    return ApiResult::success(zetta::ml::StudyOptimizer::analysis(study->value()));
}

}  // namespace

nlohmann::json study_search_space_for_methodology(
    std::string_view methodology) {
    const StudyMethodologyDefinition* definition =
        study_definition(methodology);
    if (definition == nullptr) {
        const zetta::ml::Methodology* resolved =
            zetta::ml::find_methodology(methodology);
        const std::string id =
            resolved != nullptr ? resolved->id : std::string(methodology);
        return {
            {"studies", false},
            {"study_reason",
             "Study trials are not supported for " + id +
                 "; the study driver currently supports M-1."},
            {"study_search_space", nullptr}};
    }
    nlohmann::json parameters = nlohmann::json::array();
    for (const StudyParameterDefinition& parameter : definition->parameters) {
        parameters.push_back(parameter_definition_json(parameter));
    }
    return {
        {"studies", true},
        {"study_reason", ""},
        {"study_search_space",
         {{"parameters", std::move(parameters)},
          {"objective_metrics", definition->objective_metrics},
          {"objective_metric_aliases", objective_metric_aliases(*definition)},
          {"validation_splits", definition->validation_splits},
          {"dataset_path",
           {{"kind", "table"},
            {"format", "database.table"},
            {"must_exist", true},
            {"required_without_project_pipeline", true},
            {"directory_paths", false}}}}}};
}

void register_study_capabilities(Registry& registry) {
    Capability validate{
        "validate_study", "Validate study",
        "Validate and normalize a proposed AutoML study without creating it or changing state.",
        create_study_schema(), validate_study_output_schema(),
        CapabilityKind::Read, Operation{CapabilityDomain::Api, "query"}, false,
        false, handle_validate_study};
    validate.invalid_input_hint = from_defaults_hint;
    registry.add(std::move(validate));
    Capability create{
        "create_study", "Create study",
        "Create a replicated AutoML study after validating its non-test objective and Pipeline; this mutates state.",
        create_study_schema(), study_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_study"}, true, false,
        handle_create_study};
    create.invalid_input_hint = from_defaults_hint;
    registry.add(std::move(create));
    registry.add(Capability{
        "get_study", "Get study",
        "Read one study's objective, search configuration, budgets and enforced usage. No side effects.",
        get_study_schema(), study_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_get_study});
    registry.add(Capability{
        "list_studies", "List studies",
        "Read every study with display names, lifecycle stamps, budgets and objective summary. No side effects.",
        list_studies_schema(), list_studies_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_list_studies});
    registry.add(Capability{
        "list_trials", "List trials",
        "Read the Runs belonging to a study, optionally filtered by trial state. No side effects.",
        list_trials_schema(), list_trials_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_list_trials});
    registry.add(Capability{
        "stop_study", "Stop study",
        "Stop a running study and cooperatively stop its running trial Runs; this mutates state.",
        get_study_schema(), stop_study_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "stop_study"}, true, false,
        handle_stop_study});
    registry.add(Capability{
        "delete_study", "Delete study",
        "Delete a stopped or complete study and its non-promoted trial Runs "
        "with their metrics while retaining promoted Model lineage; this is "
        "not undoable.",
        get_study_schema(), delete_study_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_study"}, true, false,
        handle_delete_study});
    registry.add(Capability{
        "stop_trial", "Stop trial",
        "Stop one running trial Run without stopping its study; this mutates state.",
        stop_trial_schema(), stop_trial_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "stop_trial"}, true, false,
        handle_stop_trial});
    registry.add(Capability{
        "promote_trial", "Promote trial",
        "Register a complete trial's existing model as the study project's promoted model version; this requires the dedicated promotion permission.",
        promote_trial_schema(), promote_trial_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "promote_trial"}, true, false,
        handle_promote_trial});
    registry.add(Capability{
        "get_study_analysis", "Get study analysis",
        "Read the best trial, optimization history, parameter importances, parallel-coordinate data and confidence leaderboard. No side effects.",
        get_study_schema(), analysis_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_analysis});
}

}  // namespace zetta::api
