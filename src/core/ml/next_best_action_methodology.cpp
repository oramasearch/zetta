// SPDX-License-Identifier: MIT

#include "zetta/ml/next_best_action_methodology.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>
#include <string_view>

#include <nlohmann/json-schema.hpp>

#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 3> kKinds = {
    "next_best_action", "recommendation", "recommender"};

class StepSchemaError final : public nlohmann::json_schema::error_handler {
public:
  std::string reason;

  void error(const Json::json_pointer &pointer, const Json &,
             const std::string &message) override {
    if (reason.empty()) {
      reason = "next-best-action model attributes " + pointer.to_string() +
               ": " + message;
    }
  }
};

bool is_kind(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return std::ranges::find(kKinds, value) != kKinds.end();
}

Json model_defaults() {
  return {{"model_kind", "next_best_action"},
          {"entity_column", "entity_id"},
          {"action_column", "action_id"},
          {"label_column", "response"},
          {"feature_columns", {"feature"}},
          {"value_column", "action_value"},
          {"cost_column", "action_cost"},
          {"eligibility_column", "eligible"},
          {"contacts_column", "contacts"},
          {"contact_cap_column", "contact_cap"},
          {"interaction_count_column", "interaction_count"},
          {"interaction_time_column", "interaction_time"},
          {"as_of", 0.0},
          {"logging_propensity_column", "logging_propensity"},
          {"num_trees", 40},
          {"max_depth", 3},
          {"max_bins", 32},
          {"learning_rate", 0.2},
          {"factors", 4},
          {"als_iterations", 8},
          {"hit_rate_k", 5},
          {"implicit_alpha", 20.0},
          {"als_regularization", 0.1},
          {"als_weight", 0.25},
          {"minimum_logging_propensity", 0.01},
          {"maximum_importance_weight", 20.0},
          {"seed", 1047},
          {"time_based_holdout", true},
          {"offline_evaluation", "logging_propensities_required"},
          {"propensity_interpretation", "predictive_not_causal"}};
}

Json policy_defaults() {
  return {{"hard_constraints", true},
          {"apply_eligibility", true},
          {"apply_contact_caps", true},
          {"selection", "best_eligible_action_per_entity"},
          {"budget", 1.0e12}};
}

ir::ExecutionGraph build_next_best_action_shape(const ShapeRequest &request) {
  Json source = {{"source", "datasets"}, {"table", "interactions"}};
  Json model = model_defaults();
  Json policy = policy_defaults();
  if (request.hints.is_object()) {
    for (const auto &[key, value] : request.hints.items()) {
      if (key == "source" || key == "table") {
        source[key] = value;
      } else if (key == "budget" || key == "hard_constraints" ||
                 key == "apply_eligibility" || key == "apply_contact_caps" ||
                 key == "selection") {
        policy[key] = value;
      } else if (key == "model_kind" &&
                 (!value.is_string() || !is_kind(value.get<std::string>()))) {
        model["requested_model_kind"] = value;
      } else if (key == "time_column") {
        if (!request.hints.contains("interaction_time_column")) {
          model["interaction_time_column"] = value;
        }
      } else {
        model[key] = value;
      }
    }
  }

  ir::ExecutionGraph graph;
  graph.add_node(ir::StepKind::Connect, "Read candidate interactions", source,
                 "recommendation_source");
  graph.add_node(ir::StepKind::Clean,
                 "Validate candidate rows and numeric features",
                 {{"transform", "candidate_features"},
                  {"preserve_interaction_time", true}},
                 "recommendation_features");
  graph.add_node(ir::StepKind::Model,
                 "Train propensity and implicit-feedback models", model,
                 "recommendation_model");
  graph.add_node(ir::StepKind::Act,
                 "Apply eligibility, contact caps and budget", policy,
                 "recommendation_policy");
  graph.add_node(ir::StepKind::Serve, "Write ranked actions and policy report",
                 {{"outputs", {"recommendation_table", "policy_report"}}},
                 "recommendation_outputs");
  graph.add_edge("recommendation_source", "recommendation_features");
  graph.add_edge("recommendation_features", "recommendation_model");
  graph.add_edge("recommendation_model", "recommendation_policy");
  graph.add_edge("recommendation_policy", "recommendation_outputs");
  return graph;
}

bool validate_structure(const ir::ExecutionGraph &graph,
                        std::array<std::shared_ptr<ir::Node>, 5> &steps,
                        std::string &error) {
  const std::array kinds = {ir::StepKind::Connect, ir::StepKind::Clean,
                            ir::StepKind::Model, ir::StepKind::Act,
                            ir::StepKind::Serve};
  if (graph.node_count() != steps.size() ||
      graph.edge_count() != steps.size() - 1) {
    error = "next-best-action requires exactly connect -> features -> model "
            "-> policy -> outputs";
    return false;
  }
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    const auto matches = graph.nodes_of_kind(kinds[index]);
    if (matches.size() != 1) {
      error = "next-best-action requires one step of every pipeline stage";
      return false;
    }
    steps[index] = matches.front();
  }
  for (std::size_t index = 1; index < steps.size(); ++index) {
    const auto edges = graph.edges_into(steps[index]->id);
    if (edges.size() != 1 || edges.front().from != steps[index - 1]->id ||
        edges.front().kind != ir::EdgeKind::DataFlow) {
      error = "next-best-action stages must be connected in policy order";
      return false;
    }
  }
  if (steps[1]->attributes != Json{{"transform", "candidate_features"},
                                   {"preserve_interaction_time", true}} ||
      steps[4]->attributes !=
          Json{{"outputs", {"recommendation_table", "policy_report"}}}) {
    error = "next-best-action permits only candidate features and documented "
            "recommendation outputs";
    return false;
  }
  return true;
}

bool validate_source(const Json &source, std::string &database,
                     std::string &table, std::string &error) {
  const std::set<std::string> allowed = {
      "solution_snapshot_as_of", "snapshot_as_of", "source", "source_kind",
      "table"};
  if (!source.is_object() || !source.contains("source") ||
      !source.contains("table") || !source["source"].is_string() ||
      !source["table"].is_string()) {
    error = "next-best-action connect must name source and table strings";
    return false;
  }
  for (const auto &[key, value] : source.items()) {
    (void)value;
    if (!allowed.contains(key)) {
      error = "next-best-action connect contains an unsupported attribute";
      return false;
    }
  }
  database = source["source"].get<std::string>();
  table = source["table"].get<std::string>();
  if (database.empty() || table.empty()) {
    error = "next-best-action source and table must not be empty";
    return false;
  }
  return true;
}

bool valid_integer(const Json &value, std::uint64_t minimum,
                   std::uint64_t maximum) {
  if (!value.is_number_unsigned() && !value.is_number_integer()) {
    return false;
  }
  if (value.is_number_integer() && value.get<std::int64_t>() < 0) {
    return false;
  }
  const std::uint64_t number = value.get<std::uint64_t>();
  return number >= minimum && number <= maximum;
}

bool valid_number(const Json &value, double minimum, bool strict_minimum,
                  double maximum) {
  if (!value.is_number()) {
    return false;
  }
  const double number = value.get<double>();
  const bool above = strict_minimum ? number > minimum : number >= minimum;
  return std::isfinite(number) && above && number <= maximum;
}

bool validate_model_keys(const Json &options, std::string &error) {
  nlohmann::json_schema::json_validator validator(
      next_best_action_step_schema());
  StepSchemaError validation;
  validator.validate(options, validation);
  error = validation.reason;
  return error.empty();
}

bool read_columns(const Json &options, NextBestActionPipeline &pipeline,
                  std::string &error) {
  const std::array keys = {"entity_column",
                           "action_column",
                           "label_column",
                           "value_column",
                           "cost_column",
                           "eligibility_column",
                           "contacts_column",
                           "contact_cap_column",
                           "interaction_count_column",
                           "interaction_time_column",
                           "logging_propensity_column"};
  std::set<std::string> used;
  std::array<std::string, keys.size()> values;
  for (std::size_t index = 0; index < keys.size(); ++index) {
    const Json &value = options[keys[index]];
    if (!value.is_string()) {
      error = "next-best-action column roles must be strings";
      return false;
    }
    values[index] = value.get<std::string>();
    const bool optional_logging = index + 1 == keys.size();
    if (values[index].empty() && !optional_logging) {
      error = "next-best-action required column roles must not be empty";
      return false;
    }
    if (!values[index].empty() && !used.insert(values[index]).second) {
      error = "next-best-action column roles must be distinct";
      return false;
    }
  }
  const Json &features = options["feature_columns"];
  if (!features.is_array() || features.empty()) {
    error = "next-best-action requires at least one feature column";
    return false;
  }
  const std::set<std::string> protected_columns = {
      values[0], values[1], values[2], values[9]};
  std::set<std::string> declared_features;
  std::vector<std::string> feature_columns;
  for (const Json &feature : features) {
    if (!feature.is_string() || feature.get<std::string>().empty()) {
      error = "next-best-action feature columns must be nonempty strings";
      return false;
    }
    std::string name = feature.get<std::string>();
    if (protected_columns.contains(name) ||
        !declared_features.insert(name).second) {
      error = "next-best-action features must be distinct from entity, action, "
              "label, and time columns";
      return false;
    }
    feature_columns.push_back(std::move(name));
  }
  auto &params = pipeline.params;
  pipeline.contract.entity_column = values[0];
  pipeline.contract.label_column = values[2];
  pipeline.contract.time_column = values[9];
  const std::optional<double> cutoff =
      zetta::util::parse_time_seconds(options.at("as_of"));
  if (!cutoff) {
    error = "next-best-action as_of must be a finite epoch number or "
            "ISO-8601 timestamp";
    return false;
  }
  pipeline.contract.as_of = *cutoff;
  pipeline.contract.feature_columns = feature_columns;
  params.action_column = values[1];
  params.value_column = values[3];
  params.cost_column = values[4];
  params.eligibility_column = values[5];
  params.contacts_column = values[6];
  params.contact_cap_column = values[7];
  params.interaction_count_column = values[8];
  params.interaction_time_column = values[9];
  params.logging_propensity_column = values[10];
  return true;
}

bool validate_fixed_rules(const Json &model, const Json &policy,
                          std::string &error) {
  if (!model["model_kind"].is_string() ||
      !is_kind(model["model_kind"].get<std::string>())) {
    error = "next-best-action model_kind is invalid";
    return false;
  }
  if (model["time_based_holdout"] != true ||
      model["offline_evaluation"] != "logging_propensities_required" ||
      model["propensity_interpretation"] != "predictive_not_causal") {
    error = "next-best-action requires a time-based holdout, explicit logging "
            "propensities, and predictive-not-causal interpretation";
    return false;
  }
  if (!policy.is_object() || policy.size() != 5 ||
      !policy.contains("hard_constraints") ||
      policy["hard_constraints"] != true ||
      !policy.contains("apply_eligibility") ||
      policy["apply_eligibility"] != true ||
      !policy.contains("apply_contact_caps") ||
      policy["apply_contact_caps"] != true || !policy.contains("selection") ||
      policy["selection"] != "best_eligible_action_per_entity" ||
      !policy.contains("budget") ||
      !valid_number(policy["budget"], 0.0, false, 1.0e18)) {
    error = "next-best-action policy must enforce eligibility and contact "
            "caps before budgeted best-action selection";
    return false;
  }
  return true;
}

bool read_parameters(const Json &options, NextBestActionPipeline &pipeline,
                     std::string &error) {
  const auto integer = [&](const char *key, std::uint64_t minimum,
                           std::uint64_t maximum) {
    if (valid_integer(options[key], minimum, maximum)) {
      return true;
    }
    error = "next-best-action attribute '" + std::string(key) +
            "' is outside its integer range";
    return false;
  };
  const auto number = [&](const char *key, double minimum, bool strict,
                          double maximum) {
    if (valid_number(options[key], minimum, strict, maximum)) {
      return true;
    }
    error = "next-best-action attribute '" + std::string(key) +
            "' is outside its numeric range";
    return false;
  };
  if (!integer("num_trees", 1, 1000) || !integer("max_depth", 1, 16) ||
      !integer("max_bins", 2, 1024) || !integer("factors", 1, 64) ||
      !integer("als_iterations", 1, 1000) || !integer("hit_rate_k", 1, 10000) ||
      !integer("seed", 0, 4294967295ULL) ||
      !number("learning_rate", 0.0, true, 1.0) ||
      !number("implicit_alpha", 0.0, false, 1.0e9) ||
      !number("als_regularization", 0.0, true, 1.0e9) ||
      !number("als_weight", 0.0, false, 1.0e9) ||
      !number("minimum_logging_propensity", 0.0, true, 1.0) ||
      !number("maximum_importance_weight", 0.0, true, 1.0e12)) {
    return false;
  }
  auto &model = pipeline.params.model;
  model.propensity.num_trees = options["num_trees"].get<int>();
  model.propensity.max_depth = options["max_depth"].get<int>();
  model.propensity.max_bins = options["max_bins"].get<int>();
  model.propensity.learning_rate = options["learning_rate"].get<double>();
  model.factors = options["factors"].get<std::size_t>();
  model.als_iterations = options["als_iterations"].get<std::size_t>();
  model.hit_rate_k = options["hit_rate_k"].get<std::size_t>();
  model.implicit_alpha = options["implicit_alpha"].get<double>();
  model.als_regularization = options["als_regularization"].get<double>();
  model.als_weight = options["als_weight"].get<double>();
  model.minimum_logging_propensity =
      options["minimum_logging_propensity"].get<double>();
  model.maximum_importance_weight =
      options["maximum_importance_weight"].get<double>();
  model.seed = options["seed"].get<std::uint64_t>();
  return true;
}

} // namespace

const nlohmann::json &next_best_action_step_schema() {
  static const Json schema = [] {
    const Json required_column = {{"type", "string"}, {"minLength", 1}};
    const Json positive_integer = {
        {"type", "integer"}, {"minimum", 1}, {"maximum", 1000000}};
    const Json nonnegative_number = {
        {"type", "number"}, {"minimum", 0}, {"maximum", 1.0e9}};
    Json properties = {
        {"model_kind",
         {{"enum", {"next_best_action", "recommendation", "recommender"}}}},
        {"entity_column", required_column},
        {"action_column", required_column},
        {"label_column", required_column},
        {"feature_columns",
         {{"type", "array"},
          {"minItems", 1},
          {"uniqueItems", true},
          {"items", required_column}}},
        {"value_column", required_column},
        {"cost_column", required_column},
        {"eligibility_column", required_column},
        {"contacts_column", required_column},
        {"contact_cap_column", required_column},
        {"interaction_count_column", required_column},
        {"interaction_time_column", required_column},
        {"as_of",
         {{"oneOf",
           Json::array(
               {{{"type", "number"}},
                {{"type", "string"},
                 {"pattern",
                  "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"}}})}}},
        {"logging_propensity_column", {{"type", "string"}}},
        {"num_trees", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
        {"max_depth", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
        {"max_bins", {{"type", "integer"}, {"minimum", 2}, {"maximum", 1024}}},
        {"learning_rate",
         {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
        {"factors", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
        {"als_iterations", positive_integer},
        {"hit_rate_k",
         {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
        {"implicit_alpha", nonnegative_number},
        {"als_regularization",
         {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1.0e9}}},
        {"als_weight", nonnegative_number},
        {"minimum_logging_propensity",
         {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
        {"maximum_importance_weight",
         {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1.0e12}}},
        {"seed",
         {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
        {"time_based_holdout", {{"const", true}}},
        {"offline_evaluation", {{"const", "logging_propensities_required"}}},
        {"propensity_interpretation", {{"const", "predictive_not_causal"}}}};
    Json required = Json::array();
    for (const auto &[key, ignored] : properties.items()) {
      (void)ignored;
      required.push_back(key);
    }
    return Json{{"$schema", "http://json-schema.org/draft-07/schema#"},
                {"$id", "https://oramasearch.github.io/zetta/schemas/"
                        "next_best_action_step.schema.json"},
                {"title", "zetta.NextBestActionStep"},
                {"type", "object"},
                {"additionalProperties", false},
                {"required", std::move(required)},
                {"properties", std::move(properties)}};
  }();
  return schema;
}

bool owns_next_best_action(const ir::ExecutionGraph &graph) {
  for (const auto &step : graph.nodes_of_kind(ir::StepKind::Model)) {
    const auto found = step->attributes.find("model_kind");
    if (found != step->attributes.end() && found->is_string() &&
        is_kind(found->get<std::string>())) {
      return true;
    }
  }
  return false;
}

std::optional<NextBestActionPipeline>
parse_next_best_action_pipeline(const ir::ExecutionGraph &graph,
                                std::string &error) {
  std::array<std::shared_ptr<ir::Node>, 5> steps;
  if (!validate_structure(graph, steps, error)) {
    return std::nullopt;
  }
  NextBestActionPipeline pipeline;
  if (!validate_source(steps[0]->attributes, pipeline.database, pipeline.table,
                       error) ||
      !validate_model_keys(steps[2]->attributes, error) ||
      !read_columns(steps[2]->attributes, pipeline, error) ||
      !validate_fixed_rules(steps[2]->attributes, steps[3]->attributes,
                            error) ||
      !read_parameters(steps[2]->attributes, pipeline, error)) {
    return std::nullopt;
  }
  const Json &options = steps[2]->attributes;
  pipeline.params.model.budget = steps[3]->attributes["budget"].get<double>();
  pipeline.params.model_name = "next_best_action";
  return pipeline;
}

Methodology make_m8_methodology() {
  Methodology methodology;
  methodology.id = "M-8";
  methodology.name = "Recommendation / next-best-action";
  methodology.family = "Action & decision";
  methodology.model_kinds = {"next_best_action", "recommendation",
                             "recommender"};
  methodology.solutions = {"S-11"};
  methodology.keywords = {"recommend", "next best action", "next-best-action"};
  methodology.inputs = {
      {"entity", true, "integer or text recipient identifier"},
      {"treatment", true, "integer candidate action or item"},
      {"label", true, "binary response or conversion outcome"},
      {"time", true, "interaction time for chronological holdout"},
      {"feature", true, "numeric propensity features"},
      {"group", false, "implicit interaction count by entity and action"}};
  methodology.outputs = {"recommendation table", "policy report",
                         "next-best-action model"};
  methodology.metrics = {"hit rate at k",
                         "popularity hit rate at k",
                         "inverse propensity value",
                         "doubly robust value",
                         "effective sample size",
                         "maximum clipped weight"};
  methodology.issue = "https://github.com/oramasearch/zetta/issues/1047";
  methodology.runnable = true;
  methodology.build_shape = build_next_best_action_shape;
  methodology.owns = owns_next_best_action;
  methodology.run = run_next_best_action_methodology;
  return methodology;
}

} // namespace zetta::ml
