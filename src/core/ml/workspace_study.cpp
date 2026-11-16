// SPDX-License-Identifier: MIT

// Experiments over a workspace methodology's parameters (lake#689;
// docs/ml/workspace-methodologies.md#experiments).

#include "zetta/ml/workspace_study.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "zetta/ml/workspace_methodology.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"

namespace zetta::ml {
namespace {

// Lake's runner holds out `holdout_fraction` of the rows (0.2 by default) and
// reports every metric on them as held_out_<m>: the study's validation split.
constexpr const char* kValidationSplit = "validation";

std::string at_parameter(const std::string& name) { return "/search_space/" + name; }

const nlohmann::json* declared_parameter(const nlohmann::json& entry, const std::string& name) {
    const auto parameters = entry.find("parameters");
    if (parameters == entry.end() || !parameters->is_array()) return nullptr;
    for (const nlohmann::json& parameter : *parameters) {
        if (parameter.value("name", std::string()) == name) return &parameter;
    }
    return nullptr;
}

// A declared parameter as a searchable StudyParameter, or nullopt when it has
// no range to search (a number without both bounds).
std::optional<StudyParameter> searchable(const nlohmann::json& parameter) {
    StudyParameter result;
    result.name = parameter.at("name").get<std::string>();
    const std::string kind = parameter.at("kind").get<std::string>();
    if (kind == "bool") {
        result.kind = ParameterKind::Categorical;
        result.choices = {false, true};
        return result;
    }
    if (kind == "categorical") {
        result.kind = ParameterKind::Categorical;
        result.choices = parameter.value("choices", std::vector<nlohmann::json>{});
        if (result.choices.empty()) return std::nullopt;
        return result;
    }
    if (!parameter.contains("minimum") || !parameter.contains("maximum")) return std::nullopt;
    result.kind = kind == "int" ? ParameterKind::Integer : ParameterKind::Float;
    result.minimum = parameter.at("minimum").get<double>();
    result.maximum = parameter.at("maximum").get<double>();
    result.logarithmic = parameter.value("log", false);
    if (result.kind == ParameterKind::Integer) result.step = 1.0;
    if (!(result.minimum < result.maximum)) return std::nullopt;
    return result;
}

nlohmann::json parameter_listing(const StudyParameter& parameter,
                                 const nlohmann::json& default_value) {
    return {{"name", parameter.name},
            {"kind", to_string(parameter.kind)},
            {"minimum", parameter.minimum},
            {"maximum", parameter.maximum},
            {"step", parameter.step},
            {"log", parameter.logarithmic},
            {"default", default_value},
            {"choices", parameter.choices},
            {"aliases", nlohmann::json::array()},
            {"condition_parameter", ""},
            {"condition_values", nlohmann::json::array()}};
}

}  // namespace

bool is_workspace_study(const StudySpec& spec) {
    return is_workspace_methodology_id(spec.methodology);
}

bool metric_is_minimized(std::string_view metric) {
    static constexpr std::array<std::string_view, 9> smaller_is_better{
        "loss", "error", "rmse", "mse", "mae", "mape", "brier", "perplexity", "inertia"};
    return std::ranges::any_of(smaller_is_better, [&](std::string_view word) {
        return metric.find(word) != std::string_view::npos;
    });
}

WorkspaceStudyDefinition workspace_study_definition(const nlohmann::json& entry) {
    WorkspaceStudyDefinition definition;
    for (const nlohmann::json& parameter : entry.value("parameters", nlohmann::json::array())) {
        definition.defaults[parameter.at("name").get<std::string>()] = parameter.at("default");
        if (std::optional<StudyParameter> found = searchable(parameter)) {
            definition.parameters.push_back(std::move(*found));
        }
    }
    for (const nlohmann::json& metric : entry.value("metrics", nlohmann::json::array())) {
        definition.objective_metrics.push_back("held_out_" + metric.get<std::string>());
    }
    if (!definition.objective_metrics.empty()) {
        definition.default_objective.metric = definition.objective_metrics.front();
        definition.default_objective.direction =
            metric_is_minimized(definition.default_objective.metric) ? StudyDirection::Minimize
                                                                     : StudyDirection::Maximize;
        definition.default_objective.validation_split = kValidationSplit;
    }
    const std::string id = entry.value("id", std::string());
    if (!workspace_methodology_runnable(entry)) {
        definition.reason = "Validate " + id +
                            " first: an experiment runs only code that passed validation.";
    } else if (definition.parameters.empty()) {
        definition.reason = id +
                            " declares no parameter with a range to search: give a number its "
                            "minimum and maximum, or a categorical its choices.";
    } else if (definition.objective_metrics.empty()) {
        definition.reason = id + " reports no metric to compare its trials by.";
    }
    return definition;
}

nlohmann::json workspace_study_listing_fields(const nlohmann::json& entry) {
    const WorkspaceStudyDefinition definition = workspace_study_definition(entry);
    if (!definition.reason.empty()) {
        return {{"studies", false},
                {"study_reason", definition.reason},
                {"study_search_space", nullptr}};
    }
    nlohmann::json parameters = nlohmann::json::array();
    for (const StudyParameter& parameter : definition.parameters) {
        parameters.push_back(
            parameter_listing(parameter, definition.defaults.at(parameter.name)));
    }
    nlohmann::json aliases = nlohmann::json::object();
    for (const std::string& metric : definition.objective_metrics) {
        aliases[metric] = nlohmann::json::array();
    }
    return {{"studies", true},
            {"study_reason", ""},
            {"study_search_space",
             {{"parameters", std::move(parameters)},
              {"objective_metrics", definition.objective_metrics},
              {"objective_metric_aliases", std::move(aliases)},
              {"validation_splits", nlohmann::json::array({kValidationSplit})},
              {"dataset_path",
               {{"kind", "table"},
                {"format", "database.table"},
                {"must_exist", true},
                {"required_without_project_pipeline", true},
                {"directory_paths", false}}}}}};
}

std::optional<std::string> normalize_workspace_study_spec(const nlohmann::json& entry,
                                                          StudySpec& spec) {
    const WorkspaceStudyDefinition definition = workspace_study_definition(entry);
    if (!definition.reason.empty()) return "/methodology: " + definition.reason;
    if (spec.objective.metric.empty()) spec.objective = definition.default_objective;
    if (!spec.objective.metric.starts_with("held_out_")) {
        const std::string prefixed = "held_out_" + spec.objective.metric;
        if (std::ranges::contains(definition.objective_metrics, prefixed)) {
            spec.objective.metric = prefixed;
        }
    }
    if (!std::ranges::contains(definition.objective_metrics, spec.objective.metric)) {
        std::string reason = "/objective/metric: " + spec.methodology + " reports";
        for (const std::string& metric : definition.objective_metrics) reason += " " + metric;
        return reason + "; '" + spec.objective.metric + "' is not one of them";
    }
    if (spec.objective.validation_split != kValidationSplit) {
        return std::string("/objective/validation_split: a workspace methodology's trials are "
                           "compared on their held-out rows, the 'validation' split");
    }
    if (spec.search_space.empty()) spec.search_space = definition.parameters;
    std::set<std::string> names;
    for (StudyParameter& parameter : spec.search_space) {
        const nlohmann::json* declared = declared_parameter(entry, parameter.name);
        if (declared == nullptr) {
            return at_parameter(parameter.name) + ": " + spec.methodology +
                   " declares no parameter '" + parameter.name + "'";
        }
        if (!names.insert(parameter.name).second) {
            return at_parameter(parameter.name) + ": the parameter appears more than once";
        }
        const std::string kind = declared->at("kind").get<std::string>();
        const ParameterKind expected = kind == "int"     ? ParameterKind::Integer
                                       : kind == "float" ? ParameterKind::Float
                                                         : ParameterKind::Categorical;
        if (parameter.kind != expected) {
            return at_parameter(parameter.name) + "/kind: it is declared " + kind + ", searched as " +
                   std::string(to_string(expected));
        }
        if (!parameter.condition_parameter.empty()) {
            return at_parameter(parameter.name) +
                   "/condition_parameter: a workspace methodology's parameters are not conditional";
        }
        if (expected == ParameterKind::Categorical) {
            const std::vector<nlohmann::json> allowed =
                kind == "bool" ? std::vector<nlohmann::json>{false, true}
                               : declared->value("choices", std::vector<nlohmann::json>{});
            for (const nlohmann::json& choice : parameter.choices) {
                if (!std::ranges::contains(allowed, choice)) {
                    return at_parameter(parameter.name) + "/choices: " + choice.dump() +
                           " is not one of its declared choices";
                }
            }
            continue;
        }
        if (declared->contains("minimum") &&
            parameter.minimum < declared->at("minimum").get<double>()) {
            return at_parameter(parameter.name) + "/minimum: below its declared minimum " +
                   declared->at("minimum").dump();
        }
        if (declared->contains("maximum") &&
            parameter.maximum > declared->at("maximum").get<double>()) {
            return at_parameter(parameter.name) + "/maximum: above its declared maximum " +
                   declared->at("maximum").dump();
        }
        if (expected == ParameterKind::Integer && parameter.step < 1.0) parameter.step = 1.0;
    }
    // Lake's runner reports one result per trial, so nothing can be pruned
    // part-way, and the runner does not train privately.
    spec.pruner = PrunerKind::None;
    if (spec.differential_privacy) {
        return std::string("/differential_privacy: a workspace methodology does not train "
                           "with differential privacy");
    }
    const nlohmann::json target = entry.value("target", nlohmann::json::object());
    if (target.value("required", false) && spec.target.empty() && spec.pipeline.empty()) {
        return std::string("/target: ") + spec.methodology + " needs a " +
               target.value("kind", std::string("target")) +
               " target column; name it in target";
    }
    return std::nullopt;
}

nlohmann::json workspace_study_runner_work(Node& node, const Study& study) {
    nlohmann::json work = nlohmann::json::array();
    const StudySpec& spec = study.spec();
    if (!is_workspace_study(spec) || study.status() != StudyStatus::Running) return work;
    nlohmann::json defaults = nlohmann::json::object();
    if (const auto* replication = node.replication(); replication != nullptr) {
        const WorkspaceMethodologyStore store(*node.replication());
        if (const std::optional<nlohmann::json> entry = store.get(spec.methodology)) {
            defaults = workspace_study_definition(*entry).defaults;
        }
    }
    // A pipeline study runs what its pipeline's steps say: the connect step's
    // table, the model step's target and holdout.
    std::string project_id;
    std::string graph_id;
    std::string dataset = spec.dataset;
    std::string target = spec.target;
    double holdout = 0.2;
    if (!spec.project.empty()) {
        for (const auto& project : node.projects()) {
            if (project->id() != spec.project && project->name() != spec.project) continue;
            project_id = project->id();
            auto graph = project->find_graph_by_id(spec.pipeline);
            if (!graph) graph = project->find_graph(spec.pipeline);
            if (!graph) break;
            graph_id = graph->id();
            for (const auto& step : graph->graph().nodes_of_kind(ir::StepKind::Model)) {
                if (!step || !step->attributes.is_object()) continue;
                if (target.empty()) target = step->attributes.value("target", std::string());
                holdout = step->attributes.value("holdout_fraction", holdout);
                for (const auto& [name, value] :
                     step->attributes.value("parameters", nlohmann::json::object()).items()) {
                    defaults[name] = value;
                }
                break;
            }
            if (dataset.empty()) {
                for (const auto& step : graph->graph().nodes_of_kind(ir::StepKind::Connect)) {
                    if (!step || !step->attributes.is_object()) continue;
                    const std::string source = step->attributes.value("source", std::string());
                    const std::string table = step->attributes.value("table", std::string());
                    if (!source.empty() && !table.empty()) {
                        dataset = source + "." + table;
                        break;
                    }
                }
            }
            break;
        }
    }
    for (const StudyTrial& trial : study.value().trials) {
        if (trial.state != TrialState::Running) continue;
        nlohmann::json params = defaults;
        double trial_holdout = holdout;
        for (const auto& [name, value] : trial.params.items()) {
            if (name == "holdout_fraction" && value.is_number()) {
                trial_holdout = value.get<double>();
                continue;
            }
            params[name] = value;
        }
        nlohmann::json item = {{"study", study.id()},
                               {"study_name", spec.name},
                               {"trial", trial.run},
                               {"number", trial.number},
                               {"methodology", spec.methodology},
                               {"params", std::move(params)},
                               {"seed", trial.seed},
                               {"dataset_path", dataset},
                               {"target", target},
                               {"features", spec.features},
                               {"holdout_fraction", trial_holdout}};
        if (!project_id.empty() && !graph_id.empty()) {
            item["project"] = project_id;
            item["graph"] = graph_id;
        }
        work.push_back(std::move(item));
    }
    return work;
}

const nlohmann::json& workspace_study_work_schema() {
    static const nlohmann::json scalar = {{"type", {"string", "number", "integer", "boolean"}}};
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"study", "study_name", "trial", "number", "methodology", "params", "seed",
          "dataset_path", "target", "features", "holdout_fraction"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", {{"type", "string"}}},
          {"study_name", {{"type", "string"}}},
          {"trial", {{"type", "string"}, {"description", "the trial's Run id"}}},
          {"number", {{"type", "integer"}, {"minimum", 0}}},
          {"methodology", {{"type", "string"}}},
          {"params",
           {{"type", "object"},
            {"additionalProperties", scalar},
            {"description", "the declared defaults overridden by the trial's sampled values"}}},
          {"seed", {{"type", "integer"}, {"minimum", 0}}},
          {"dataset_path", {{"type", "string"}}},
          {"target", {{"type", "string"}}},
          {"features", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"holdout_fraction", {{"type", "number"}}},
          {"project", {{"type", "string"}}},
          {"graph", {{"type", "string"}}}}}};
    return schema;
}

}  // namespace zetta::ml
