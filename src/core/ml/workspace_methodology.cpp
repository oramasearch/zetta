#include "zetta/ml/workspace_methodology.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "../catalog.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kModelKindPrefix = "python:";

bool is_lower_hex(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
}

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "type": "object",
      "required": ["id", "name", "input_kinds", "target", "metrics", "code"],
      "additionalProperties": false,
      "properties": {
        "id": { "type": "string", "pattern": "^W-[0-9a-f]{8}$" },
        "name": { "type": "string", "minLength": 1, "maxLength": 200 },
        "description": { "type": "string", "maxLength": 20000 },
        "family": { "type": "string", "maxLength": 200 },
        "input_kinds": {
          "type": "array", "minItems": 1, "uniqueItems": true,
          "items": { "type": "string",
                     "enum": ["tabular", "text", "time_series", "graph", "image", "audio",
                              "document"] }
        },
        "target": {
          "type": "object", "required": ["required", "kind"], "additionalProperties": false,
          "properties": {
            "required": { "type": "boolean" },
            "kind": { "type": "string", "enum": ["binary", "multiclass", "regression", "none"] }
          }
        },
        "required_features": {
          "type": "array", "uniqueItems": true,
          "items": { "type": "string", "minLength": 1 }
        },
        "parameters": {
          "type": "array",
          "items": {
            "type": "object", "required": ["name", "kind", "default"],
            "additionalProperties": false,
            "properties": {
              "name": { "type": "string", "pattern": "^[A-Za-z_][A-Za-z0-9_]*$" },
              "kind": { "type": "string", "enum": ["int", "float", "categorical", "bool"] },
              "default": { "type": ["string", "number", "integer", "boolean"] },
              "minimum": { "type": "number" },
              "maximum": { "type": "number" },
              "choices": { "type": "array", "minItems": 1,
                           "items": { "type": ["string", "number", "integer", "boolean"] } },
              "log": { "type": "boolean" },
              "description": { "type": "string" }
            }
          }
        },
        "metrics": {
          "type": "array", "minItems": 1, "uniqueItems": true,
          "items": { "type": "string", "pattern": "^[a-z][a-z0-9_]*$" }
        },
        "explain": { "type": "boolean" },
        "code": {
          "type": "object", "required": ["repository_id", "revision"],
          "additionalProperties": false,
          "properties": {
            "repository_id": { "type": "string", "minLength": 1 },
            "revision": { "type": "string", "minLength": 1 },
            "path": { "type": "string" }
          }
        },
        "image": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "build_id": { "type": "string" },
            "reference": { "type": "string" }
          }
        },
        "validation": {
          "type": "object", "required": ["status"], "additionalProperties": false,
          "properties": {
            "status": { "type": "string",
                        "enum": ["not_validated", "running", "passed", "failed"] },
            "findings": {
              "type": "array",
              "items": {
                "type": "object", "required": ["check", "ok"], "additionalProperties": false,
                "properties": {
                  "check": { "type": "string", "minLength": 1 },
                  "ok": { "type": "boolean" },
                  "message": { "type": "string" }
                }
              }
            },
            "run_id": { "type": "string" },
            "revision": { "type": "string" },
            "validated_at": { "type": "string" }
          }
        },
        "network": { "type": "boolean" },
        "resources": {
          "type": "object", "additionalProperties": false,
          "properties": {
            "cpu": { "type": "string" },
            "memory": { "type": "string" },
            "gpu": { "type": "integer", "minimum": 0 },
            "timeout_seconds": { "type": "integer", "minimum": 1 }
          }
        },
        "duplicated_from": { "type": "string" },
        "created_by": { "type": "string" },
        "created_at": { "type": "string" },
        "updated_at": { "type": "string" }
      }
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json&,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

std::optional<std::string> parameter_problem(const nlohmann::json& parameter,
                                             std::size_t index) {
    const std::string at = "/parameters/" + std::to_string(index);
    const std::string kind = parameter.at("kind").get<std::string>();
    const nlohmann::json& value = parameter.at("default");
    if (kind == "bool") {
        if (!value.is_boolean()) return at + "/default: a bool parameter's default is true or false";
        return std::nullopt;
    }
    if (kind == "categorical") {
        const auto choices = parameter.find("choices");
        if (choices == parameter.end()) {
            return at + "/choices: a categorical parameter lists its choices";
        }
        if (std::find(choices->begin(), choices->end(), value) == choices->end()) {
            return at + "/default: the default must be one of the choices";
        }
        return std::nullopt;
    }
    if (!value.is_number()) return at + "/default: a " + kind + " parameter's default is a number";
    if (kind == "int" && !value.is_number_integer()) {
        const double number = value.get<double>();
        if (std::floor(number) != number) return at + "/default: an int parameter's default is whole";
    }
    const double number = value.get<double>();
    const auto minimum = parameter.find("minimum");
    const auto maximum = parameter.find("maximum");
    if (minimum != parameter.end() && maximum != parameter.end() &&
        minimum->get<double>() > maximum->get<double>()) {
        return at + ": minimum is greater than maximum";
    }
    if (minimum != parameter.end() && number < minimum->get<double>()) {
        return at + "/default: below the minimum";
    }
    if (maximum != parameter.end() && number > maximum->get<double>()) {
        return at + "/default: above the maximum";
    }
    if (parameter.value("log", false) && minimum != parameter.end() &&
        minimum->get<double>() <= 0) {
        return at + "/minimum: a log-scaled parameter's minimum is above zero";
    }
    return std::nullopt;
}

nlohmann::json derived_inputs(const nlohmann::json& value) {
    nlohmann::json inputs = nlohmann::json::array();
    const nlohmann::json& target = value.at("target");
    if (target.at("required").get<bool>()) {
        inputs.push_back({{"role", "label"},
                          {"required", true},
                          {"description",
                           "the target column (" + target.at("kind").get<std::string>() + ")"}});
    }
    const nlohmann::json features = value.value("required_features", nlohmann::json::array());
    std::string description = "feature columns";
    if (!features.empty()) {
        description += "; required: ";
        for (std::size_t index = 0; index < features.size(); ++index) {
            if (index > 0) description += ", ";
            description += features[index].get<std::string>();
        }
    }
    inputs.push_back({{"role", "feature"}, {"required", !features.empty()},
                      {"description", std::move(description)}});
    return inputs;
}

} // namespace

bool is_workspace_methodology_id(std::string_view id) {
    if (id.size() != 10 || !id.starts_with("W-")) return false;
    return std::all_of(id.begin() + 2, id.end(), is_lower_hex);
}

std::string workspace_model_kind(std::string_view id) {
    return std::string(kModelKindPrefix) + std::string(id);
}

std::string workspace_methodology_id_of_model_kind(std::string_view model_kind) {
    if (!model_kind.starts_with(kModelKindPrefix)) return {};
    const std::string_view id = model_kind.substr(kModelKindPrefix.size());
    return is_workspace_methodology_id(id) ? std::string(id) : std::string();
}

const nlohmann::json& workspace_methodology_value_schema() { return schema_document(); }

std::optional<std::string> workspace_methodology_problem(const nlohmann::json& value) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) return handler.reason;

    const nlohmann::json& target = value.at("target");
    const bool required = target.at("required").get<bool>();
    const bool none = target.at("kind").get<std::string>() == "none";
    if (required == none) {
        return std::string(
            "/target: a methodology without a target says kind none and required false; one "
            "with a target names its kind and required true");
    }
    std::set<std::string> names;
    const nlohmann::json parameters = value.value("parameters", nlohmann::json::array());
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const std::string name = parameters[index].at("name").get<std::string>();
        if (!names.insert(name).second) {
            return "/parameters/" + std::to_string(index) + "/name: '" + name +
                   "' is declared twice";
        }
        if (auto problem = parameter_problem(parameters[index], index)) return problem;
    }
    return std::nullopt;
}

bool workspace_methodology_runnable(const nlohmann::json& value) {
    const auto validation = value.find("validation");
    if (validation == value.end() || !validation->is_object()) return false;
    if (validation->value("status", std::string()) != "passed") return false;
    return validation->value("revision", std::string()) ==
           value.at("code").value("revision", std::string());
}

nlohmann::json workspace_methodology_listing(const nlohmann::json& value) {
    const std::string id = value.at("id").get<std::string>();
    nlohmann::json metrics = nlohmann::json::array();
    for (const nlohmann::json& metric : value.at("metrics")) {
        metrics.push_back("held_out_" + metric.get<std::string>());
    }
    nlohmann::json interface = {
        {"input_kinds", value.at("input_kinds")},
        {"target", value.at("target")},
        {"required_features", value.value("required_features", nlohmann::json::array())},
        {"parameters", value.value("parameters", nlohmann::json::array())},
        {"metrics", value.at("metrics")},
        {"explain", value.value("explain", false)}};
    nlohmann::json listing = {
        {"id", id},
        {"name", value.at("name")},
        {"family", value.value("family", std::string("Workspace"))},
        {"model_kinds", nlohmann::json::array({workspace_model_kind(id)})},
        {"solutions", nlohmann::json::array()},
        {"inputs", derived_inputs(value)},
        {"outputs", nlohmann::json::array({"model", "predictions", "metrics"})},
        {"metrics", std::move(metrics)},
        {"runnable", workspace_methodology_runnable(value)},
        {"partial", false},
        {"issue", ""},
        {"studies", false},
        {"study_reason",
         "Experiments on a workspace methodology run its code in Lake's runner, which does not "
         "run experiment trials yet."},
        {"study_search_space", nullptr},
        {"source", "workspace"},
        {"editable", true},
        {"description", value.value("description", std::string())},
        {"interface", std::move(interface)},
        {"code", value.at("code")},
        {"image", value.value("image", nlohmann::json::object())},
        {"validation",
         value.value("validation", nlohmann::json{{"status", "not_validated"},
                                                  {"findings", nlohmann::json::array()}})},
        {"network", value.value("network", false)},
        {"resources", value.value("resources", nlohmann::json::object())},
        {"duplicated_from", value.value("duplicated_from", std::string())},
        {"created_by", value.value("created_by", std::string())},
        {"created_at", value.value("created_at", std::string())},
        {"updated_at", value.value("updated_at", std::string())}};
    return listing;
}

ir::ExecutionGraph build_workspace_methodology_shape(const nlohmann::json& value,
                                                     const ShapeRequest& request) {
    const std::string id = value.at("id").get<std::string>();
    const std::string name = value.at("name").get<std::string>();
    std::string source = request.hints.value("source", std::string());
    std::string table = request.hints.value("table", std::string());
    const std::string dataset_path = request.hints.value("dataset_path", std::string());
    if ((source.empty() || table.empty()) && !dataset_path.empty()) {
        if (const auto dot = dataset_path.find('.'); dot != std::string::npos) {
            source = dataset_path.substr(0, dot);
            table = dataset_path.substr(dot + 1);
        }
    }
    nlohmann::json parameters = nlohmann::json::object();
    for (const nlohmann::json& parameter : value.value("parameters", nlohmann::json::array())) {
        parameters[parameter.at("name").get<std::string>()] = parameter.at("default");
    }
    if (const auto given = request.hints.find("params");
        given != request.hints.end() && given->is_object()) {
        for (const auto& [key, entry] : given->items()) parameters[key] = entry;
    }
    std::string target = request.hints.value("label", std::string());
    if (target.empty()) target = request.hints.value("target", std::string());

    ir::ExecutionGraph graph;
    nlohmann::json connect = {{"note", "the rows the methodology's code is given"}};
    if (!source.empty()) connect["source"] = source;
    if (!table.empty()) connect["table"] = table;
    graph.add_node(ir::StepKind::Connect, table.empty() ? "Read the data" : "Read " + table,
                   std::move(connect), "read_methodology_rows");
    nlohmann::json model = {
        {"model_kind", workspace_model_kind(id)},
        {"methodology", id},
        {"parameters", std::move(parameters)},
        {"holdout_fraction", request.hints.value("holdout_fraction", 0.2)},
        {"runner", "lake"},
        {"note",
         "fit, predict and score in Lake's sandboxed runner; scored in-sample and on held-out "
         "rows"}};
    if (!target.empty()) model["target"] = target;
    graph.add_node(ir::StepKind::Model, "Fit " + name, std::move(model), "python_methodology");
    graph.add_node(ir::StepKind::Serve, "Register the model",
                   {{"output", "model"},
                    {"note", "the fitted artifact as a Model, with its held-out metrics"}},
                   "register_methodology_model");
    graph.add_edge("read_methodology_rows", "python_methodology");
    graph.add_edge("python_methodology", "register_methodology_model");
    return graph;
}

std::string workspace_methodology_of_graph(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!node || !node->attributes.is_object()) continue;
        if (const auto named = node->attributes.find("methodology");
            named != node->attributes.end() && named->is_string() &&
            is_workspace_methodology_id(named->get<std::string>())) {
            return named->get<std::string>();
        }
        if (const auto kind = node->attributes.find("model_kind");
            kind != node->attributes.end() && kind->is_string()) {
            if (std::string id = workspace_methodology_id_of_model_kind(kind->get<std::string>());
                !id.empty()) {
                return id;
            }
        }
    }
    return {};
}

WorkspaceMethodologyStore::WorkspaceMethodologyStore(
    zetta::cluster::ReplicationManager& replication)
    : replication_(replication) {}

std::optional<nlohmann::json> WorkspaceMethodologyStore::put(const nlohmann::json& value,
                                                             std::string* error) {
    if (std::optional<std::string> problem = workspace_methodology_problem(value)) {
        if (error != nullptr) *error = *problem;
        return std::nullopt;
    }
    replication_.set(catalog::workspace_methodology_key(value.at("id").get<std::string>()),
                     value);
    return std::optional<nlohmann::json>(std::in_place, value);
}

std::optional<nlohmann::json> WorkspaceMethodologyStore::get(const std::string& id) const {
    if (!is_workspace_methodology_id(id)) return std::nullopt;
    const std::shared_ptr<const nlohmann::json> document = replication_.shared_snapshot();
    const auto found = document->find(catalog::workspace_methodology_key(id));
    if (found == document->end() || workspace_methodology_problem(*found)) return std::nullopt;
    return std::optional<nlohmann::json>(std::in_place, *found);
}

std::vector<nlohmann::json> WorkspaceMethodologyStore::list() const {
    std::vector<nlohmann::json> entries;
    const std::shared_ptr<const nlohmann::json> document = replication_.shared_snapshot();
    catalog::for_each_with_prefix(
        *document, catalog::kWorkspaceMethodologyPrefix,
        [&entries](const std::string&, const nlohmann::json& value) {
            if (!workspace_methodology_problem(value)) entries.push_back(value);
        });
    std::sort(entries.begin(), entries.end(),
              [](const nlohmann::json& left, const nlohmann::json& right) {
                  return left.at("id").get<std::string>() < right.at("id").get<std::string>();
              });
    return entries;
}

bool WorkspaceMethodologyStore::remove(const std::string& id) {
    if (!get(id).has_value()) return false;
    replication_.remove(catalog::workspace_methodology_key(id));
    return true;
}

std::string WorkspaceMethodologyStore::new_id() const {
    for (;;) {
        std::string hex;
        for (const char character : zetta::util::new_uuid()) {
            if (is_lower_hex(character)) hex.push_back(character);
            if (hex.size() == 8) break;
        }
        std::string id = "W-" + hex;
        const std::shared_ptr<const nlohmann::json> document = replication_.shared_snapshot();
        if (!document->contains(catalog::workspace_methodology_key(id))) return id;
    }
}

} // namespace zetta::ml
