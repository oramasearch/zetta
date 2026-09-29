#include "zetta/ml/forecast_methodology.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>

#include <nlohmann/json-schema.hpp>

namespace zetta::ml {
namespace {

using Json = nlohmann::json;

ir::ExecutionGraph build_forecast_shape(const ShapeRequest &request) {
  Json options = {{"model_kind", "forecast"},
                  {"time_column", "timestamp"},
                  {"target_column", "value"},
                  {"series_column", ""},
                  {"horizon", 7},
                  {"seasonal_period", 7},
                  {"lags", {1, 7, 28}},
                  {"rolling_window", 7},
                  {"backtest_folds", 4},
                  {"interval_level", 0.8},
                  {"maximum_horizon_fraction", 0.25},
                  {"seed", 0},
                  {"num_trees", 100},
                  {"max_depth", 4},
                  {"learning_rate", 0.1},
                  {"temporal_validation", true},
                  {"split", "rolling_origin"},
                  {"holdout_fraction", 0},
                  {"calibrated", false},
                  {"baseline", "seasonal_naive"},
                  {"objective", "squared_error"}};
  Json source = {{"source", "datasets"}, {"table", "history"}};
  // Preserve even invalid supplied options: the correctness gate must refuse
  // them, not quietly replace a requested random split or future feature.
  if (request.hints.is_object()) {
    for (const auto &[key, value] : request.hints.items()) {
      if (key == "source" || key == "table" || key == "source_kind" ||
          key == "snapshot_as_of" || key == "solution_snapshot_as_of") {
        source[key] = value;
      } else if (key == "model_kind" && value != "forecast") {
        options["requested_model_kind"] = value;
      } else {
        options[key] = value;
      }
    }
  }
  ir::ExecutionGraph graph;
  graph.add_node(ir::StepKind::Connect, "Read observation history", source,
                 "forecast_history");
  graph.add_node(ir::StepKind::Clean, "Build strictly past lag features",
                 {{"transform", "past_lags"}, {"strictly_past", true}},
                 "forecast_lags");
  graph.add_node(ir::StepKind::Model, "Backtest and train a forecast", options,
                 "forecast_model");
  graph.add_node(ir::StepKind::Serve,
                 "Write forecasts and prediction intervals",
                 {{"output", "forecast_table"}}, "forecast_output");
  graph.add_edge("forecast_history", "forecast_lags");
  graph.add_edge("forecast_lags", "forecast_model");
  graph.add_edge("forecast_model", "forecast_output");
  return graph;
}

class OptionsError final : public nlohmann::json_schema::error_handler {
public:
  std::string reason;

  void error(const Json::json_pointer &pointer, const Json &,
             const std::string &message) override {
    if (reason.empty()) {
      reason =
          "forecast model attributes " + pointer.to_string() + ": " + message;
    }
  }
};

bool validate_options(const Json &options, std::string &error) {
  nlohmann::json_schema::json_validator validator(forecast_step_schema());
  OptionsError validation;
  validator.validate(options, validation);
  error = validation.reason;
  return error.empty();
}

bool validate_structure(const ir::ExecutionGraph &graph,
                        std::array<std::shared_ptr<ir::Node>, 4> &steps,
                        std::string &error) {
  const std::array kinds = {ir::StepKind::Connect, ir::StepKind::Clean,
                            ir::StepKind::Model, ir::StepKind::Serve};
  if (graph.node_count() != 4 || graph.edge_count() != 3) {
    error = "forecast requires exactly connect -> past lags -> model -> output";
    return false;
  }
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    const auto matching = graph.nodes_of_kind(kinds[index]);
    if (matching.size() != 1) {
      error = "forecast requires one step of each forecast pipeline stage";
      return false;
    }
    steps[index] = matching.front();
  }
  for (std::size_t index = 1; index < steps.size(); ++index) {
    const auto edges = graph.edges_into(steps[index]->id);
    if (edges.size() != 1 || edges.front().from != steps[index - 1]->id ||
        edges.front().kind != ir::EdgeKind::DataFlow) {
      error = "forecast stages must be connected in temporal data-flow order";
      return false;
    }
  }
  if (steps[1]->attributes !=
          Json{{"transform", "past_lags"}, {"strictly_past", true}} ||
      steps[3]->attributes != Json{{"output", "forecast_table"}}) {
    error = "forecast permits only strictly past lag features and "
            "forecast-table output";
    return false;
  }
  return true;
}

} // namespace

const nlohmann::json &forecast_step_schema() {
  static const Json schema = [] {
    const Json positive_integer = {
        {"type", "integer"}, {"minimum", 1}, {"maximum", 1000000}};
    const Json column = {{"type", "string"}, {"minLength", 1}};
    Json properties = {
        {"model_kind", {{"enum", {"forecast", "forecasting", "time_series"}}}},
        {"time_column", column},
        {"target_column", column},
        {"series_column", {{"type", "string"}}},
        {"horizon", positive_integer},
        {"seasonal_period", positive_integer},
        {"rolling_window", positive_integer},
        {"backtest_folds", positive_integer},
        {"lags",
         {{"type", "array"},
          {"minItems", 1},
          {"uniqueItems", true},
          {"items", positive_integer}}},
        {"interval_level",
         {{"type", "number"},
          {"exclusiveMinimum", 0},
          {"exclusiveMaximum", 1}}},
        {"maximum_horizon_fraction",
         {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
        {"seed",
         {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
        {"num_trees", positive_integer},
        {"max_depth", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
        {"learning_rate",
         {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
        {"temporal_validation", {{"const", true}}},
        {"split", {{"const", "rolling_origin"}}},
        {"holdout_fraction", {{"const", 0}}},
        {"calibrated", {{"const", false}}},
        {"baseline", {{"const", "seasonal_naive"}}},
        {"objective", {{"const", "squared_error"}}}};
    Json required = Json::array();
    for (const auto &[key, ignored] : properties.items()) {
      (void)ignored;
      required.push_back(key);
    }
    properties["history_length"] = positive_integer;
    return Json{{"$schema", "http://json-schema.org/draft-07/schema#"},
                {"type", "object"},
                {"additionalProperties", false},
                {"required", required},
                {"properties", properties}};
  }();
  return schema;
}

bool owns_forecast(const ir::ExecutionGraph &graph) {
  for (const auto &step : graph.nodes_of_kind(ir::StepKind::Model)) {
    const auto found = step->attributes.find("model_kind");
    if (found == step->attributes.end() || !found->is_string()) {
      continue;
    }
    std::string kind = found->get<std::string>();
    std::ranges::transform(kind, kind.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    if (kind == "forecast" || kind == "forecasting" || kind == "time_series") {
      return true;
    }
  }
  return false;
}

std::optional<ForecastPipeline>
parse_forecast_pipeline(const ir::ExecutionGraph &graph, std::string &error) {
  std::array<std::shared_ptr<ir::Node>, 4> steps;
  if (!validate_structure(graph, steps, error) ||
      !validate_options(steps[2]->attributes, error)) {
    return std::nullopt;
  }
  const Json &source = steps[0]->attributes;
  const std::set<std::string> permitted_source_fields = {
      "source", "table", "source_kind", "snapshot_as_of",
      "solution_snapshot_as_of"};
  if (!source.is_object() || !source.contains("source") ||
      !source.contains("table") || !source["source"].is_string() ||
      !source["table"].is_string() ||
      source["source"].get<std::string>().empty() ||
      source["table"].get<std::string>().empty()) {
    error = "forecast connect must name its source database and table";
    return std::nullopt;
  }
  for (const auto &[key, value] : source.items()) {
    (void)value;
    if (!permitted_source_fields.contains(key)) {
      error = "forecast connect contains unsupported attribute '" + key + "'";
      return std::nullopt;
    }
  }
  if (source.contains("source_kind") && source["source_kind"] != "snapshot") {
    error = "forecast solution sources must be materialized snapshots";
    return std::nullopt;
  }
  ForecastPipeline pipeline;
  pipeline.database = source["source"].get<std::string>();
  pipeline.table = source["table"].get<std::string>();
  const Json &options = steps[2]->attributes;
  ForecastRunParams &params = pipeline.params;
  params.time_column = options["time_column"].get<std::string>();
  params.target_column = options["target_column"].get<std::string>();
  params.series_column = options["series_column"].get<std::string>();
  if (params.time_column == params.target_column ||
      params.series_column == params.time_column ||
      params.series_column == params.target_column) {
    error = "forecast time, target and optional series roles must be distinct";
    return std::nullopt;
  }
  ForecastParams &forecast = params.forecast;
  forecast.horizon = options["horizon"].get<double>();
  forecast.seasonal_period = options["seasonal_period"].get<std::size_t>();
  forecast.lags = options["lags"].get<std::vector<std::size_t>>();
  forecast.rolling_window = options["rolling_window"].get<std::size_t>();
  forecast.backtest_folds = options["backtest_folds"].get<std::size_t>();
  forecast.interval_level = options["interval_level"].get<double>();
  forecast.maximum_horizon_fraction =
      options["maximum_horizon_fraction"].get<double>();
  forecast.seed = options["seed"].get<unsigned>();
  forecast.gbdt.num_trees = options["num_trees"].get<std::size_t>();
  forecast.gbdt.max_depth = options["max_depth"].get<std::size_t>();
  forecast.gbdt.learning_rate = options["learning_rate"].get<double>();
  if (options.contains("history_length") &&
      forecast.horizon > options["history_length"].get<double>() *
                             forecast.maximum_horizon_fraction) {
    error = "forecast horizon exceeds the stated fraction of history";
    return std::nullopt;
  }
  return pipeline;
}

Methodology make_m4_methodology() {
  Methodology methodology;
  methodology.id = "M-4";
  methodology.name = "Time-series forecasting";
  methodology.family = "Time & vigilance";
  methodology.model_kinds = {"forecast", "forecasting", "time_series"};
  methodology.solutions = {"S-5", "S-6"};
  methodology.keywords = {"forecast", "demand", "predict next week",
                          "predict next month"};
  methodology.inputs = {{"time", true, "numeric observation time"},
                        {"label", true, "numeric value to forecast"},
                        {"group", false, "integer series identifier"}};
  methodology.outputs = {"forecast table with prediction intervals",
                         "forecast model"};
  methodology.metrics = {"MAE",
                         "MASE",
                         "sMAPE",
                         "seasonal-naive MAE",
                         "forecast value added",
                         "interval coverage"};
  methodology.issue = "https://github.com/oramasearch/zetta/issues/1043";
  methodology.runnable = true;
  methodology.build_shape = build_forecast_shape;
  methodology.owns = owns_forecast;
  methodology.run = run_forecast_methodology;
  return methodology;
}

} // namespace zetta::ml
