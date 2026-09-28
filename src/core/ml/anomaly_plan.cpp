#include "zetta/ml/anomaly_methodology.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "zetta/pipeline/graph_checks.hpp"

namespace zetta::ml {
namespace {

bool text_field(const nlohmann::json &attributes, const char *key,
                std::string &value, std::string &error) {
  const auto found = attributes.find(key);
  if (found == attributes.end() || !found->is_string()) {
    error = std::string("anomaly: /") + key + " must be a string";
    return false;
  }
  value = found->get<std::string>();
  return true;
}

bool size_field(const nlohmann::json &attributes, const char *key,
                std::size_t minimum, std::size_t maximum, std::size_t &value,
                std::string &error) {
  const auto found = attributes.find(key);
  if (found == attributes.end() || !found->is_number_integer() ||
      found->get<double>() < static_cast<double>(minimum) ||
      found->get<double>() > static_cast<double>(maximum)) {
    error = std::string("anomaly: /") + key + " must be an integer in [" +
            std::to_string(minimum) + ", " + std::to_string(maximum) + "]";
    return false;
  }
  value = found->get<std::size_t>();
  return true;
}

bool read_params(const nlohmann::json &attributes, AnomalyPlan &plan,
                 std::string &error) {
  std::string mode;
  if (!text_field(attributes, "mode", mode, error)) {
    return false;
  }
  if (mode != "tabular" && mode != "kpi_time_series") {
    error = "anomaly: /mode must be tabular or kpi_time_series";
    return false;
  }
  plan.params.mode =
      mode == "tabular" ? AnomalyMode::Tabular : AnomalyMode::KpiTimeSeries;
  if (attributes.value("scaling", nlohmann::json{}) != "median_mad") {
    error = "anomaly: /scaling must be median_mad (robust statistics)";
    return false;
  }
  if (attributes.value("label_use", nlohmann::json{}) != "evaluation_only") {
    error = "anomaly: /label_use must be evaluation_only; labels never train";
    return false;
  }
  const bool contamination = attributes.contains("contamination");
  const bool budget = attributes.contains("alert_budget");
  if (contamination == budget) {
    error = "anomaly: set exactly one contamination or alert_budget";
    return false;
  }
  plan.params.threshold.contamination.reset();
  if (contamination) {
    const auto &value = attributes.at("contamination");
    if (!value.is_number() || !std::isfinite(value.get<double>()) ||
        value.get<double>() <= 0 || value.get<double>() > 1) {
      error = "anomaly: /contamination must be in (0, 1]";
      return false;
    }
    plan.params.threshold.contamination = value.get<double>();
  } else {
    std::size_t count = 0;
    if (!size_field(attributes, "alert_budget", 1, 1000000000, count, error)) {
      return false;
    }
    plan.params.threshold.alert_budget = count;
  }
  std::size_t seed = 0;
  if (!size_field(attributes, "seed", 0, 0xffffffffULL, seed, error) ||
      !size_field(attributes, "tree_count", 1, 1000, plan.params.tree_count,
                  error) ||
      !size_field(attributes, "subsample_size", 2, 4096,
                  plan.params.subsample_size, error)) {
    return false;
  }
  plan.params.seed = seed;
  if (plan.params.mode == AnomalyMode::KpiTimeSeries) {
    if (plan.contract.time_column.empty() ||
        attributes.value("causal", nlohmann::json{}) != true) {
      error =
          "anomaly: KPI fitting needs an explicit time_column and causal=true";
      return false;
    }
    if (!size_field(attributes, "seasonal_period", 2, 4096,
                    plan.params.seasonal_period, error) ||
        !size_field(attributes, "trend_window", 2, 4096,
                    plan.params.trend_window, error) ||
        !size_field(attributes, "residual_window", 4, 65536,
                    plan.params.residual_window, error) ||
        !size_field(attributes, "seasonal_cycles", 1, 1024,
                    plan.params.seasonal_cycles, error)) {
      return false;
    }
    if (plan.params.residual_window < 2 * plan.params.seasonal_period) {
      error = "anomaly: residual_window must cover two seasonal periods";
      return false;
    }
  }
  return true;
}

bool read_contract(const nlohmann::json &attributes, AnomalyPlan &plan,
                   std::string &error) {
  if (!text_field(attributes, "entity_column", plan.contract.entity_column,
                  error) ||
      !text_field(attributes, "label_column", plan.contract.label_column,
                  error) ||
      !text_field(attributes, "time_column", plan.contract.time_column,
                  error)) {
    return false;
  }
  if (attributes.contains("group_column") &&
      attributes.at("group_column") != "") {
    error = "anomaly: grouped series are not supported; select one series per "
            "table";
    return false;
  }
  std::set<std::string> roles;
  for (const auto &role :
       {plan.contract.entity_column, plan.contract.label_column,
        plan.contract.time_column}) {
    if (!role.empty() && !roles.insert(role).second) {
      error = "anomaly: entity, label, and time roles must be distinct";
      return false;
    }
  }
  const auto features = attributes.find("feature_columns");
  if (features == attributes.end() || !features->is_array()) {
    error = "anomaly: /feature_columns must be an array";
    return false;
  }
  for (const auto &value : *features) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
      error = "anomaly: feature_columns entries must be nonempty strings";
      return false;
    }
    const std::string feature = value.get<std::string>();
    if (!roles.insert(feature).second) {
      error = "anomaly: feature '" + feature +
              "' duplicates a feature or an entity, label, or time role";
      return false;
    }
    plan.contract.feature_columns.push_back(feature);
  }
  return true;
}

} // namespace

bool is_anomaly_kind(std::string_view kind) {
  return kind == "anomaly" || kind == "anomaly_detection" || kind == "outlier";
}

std::optional<AnomalyPlan> parse_anomaly_plan(const ir::ExecutionGraph &graph,
                                              std::string &error) {
  error.clear();
  const auto connects = graph.nodes_of_kind(ir::StepKind::Connect);
  const auto transforms = graph.nodes_of_kind(ir::StepKind::Clean);
  const auto models = graph.nodes_of_kind(ir::StepKind::Model);
  const auto outputs = graph.nodes_of_kind(ir::StepKind::Act);
  if (graph.node_count() != 4 || connects.size() != 1 ||
      transforms.size() != 1 || models.size() != 1 || outputs.size() != 1 ||
      graph.edge_count() != 3) {
    error = "anomaly: expected connect -> robust features -> anomaly model -> "
            "output tables";
    return std::nullopt;
  }
  const std::vector<ir::NodeId> ordered = {connects[0]->id, transforms[0]->id,
                                           models[0]->id, outputs[0]->id};
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    const auto found = std::find_if(
        graph.edges().begin(), graph.edges().end(), [&](const ir::Edge &edge) {
          return edge.from == ordered[index - 1] && edge.to == ordered[index] &&
                 edge.kind == ir::EdgeKind::DataFlow;
        });
    if (found == graph.edges().end()) {
      error = "anomaly: pipeline stages must be connected in data-flow order";
      return std::nullopt;
    }
  }
  for (const auto &step : graph.nodes()) {
    if (!step->attributes.is_object()) {
      error = "anomaly: each pipeline stage needs object attributes";
      return std::nullopt;
    }
  }
  AnomalyPlan plan;
  std::string kind;
  if (!text_field(connects[0]->attributes, "source", plan.database, error) ||
      !text_field(connects[0]->attributes, "table", plan.table, error) ||
      !text_field(models[0]->attributes, "model_kind", kind, error) ||
      !is_anomaly_kind(kind)) {
    if (error.empty()) {
      error = "anomaly: model_kind must name anomaly detection";
    }
    return std::nullopt;
  }
  if (plan.database.empty() || plan.table.empty()) {
    error = "anomaly: connect source and table must identify the input table";
    return std::nullopt;
  }
  if (!read_contract(models[0]->attributes, plan, error) ||
      !read_params(models[0]->attributes, plan, error)) {
    return std::nullopt;
  }
  const std::string operation = plan.params.mode == AnomalyMode::Tabular
                                    ? "robust_features"
                                    : "causal_kpi_residuals";
  if (transforms[0]->attributes.value("operation", nlohmann::json{}) !=
          operation ||
      outputs[0]->attributes.value("operation", nlohmann::json{}) !=
          "anomaly_tables") {
    error = "anomaly: unsupported transform or output operation";
    return std::nullopt;
  }
  return plan;
}

} // namespace zetta::ml

namespace zetta::pipeline {

CheckResult AnomalyGraphCheck::evaluate(const ir::ExecutionGraph &graph) const {
  for (const auto &step : graph.nodes_of_kind(ir::StepKind::Model)) {
    if (!step->attributes.is_object()) {
      continue;
    }
    const auto kind = step->attributes.find("model_kind");
    if (kind != step->attributes.end() && kind->is_string() &&
        ml::is_anomaly_kind(kind->get<std::string>())) {
      std::string error;
      if (!ml::parse_anomaly_plan(graph, error)) {
        return {name(), CheckStatus::Fail, error};
      }
    }
  }
  return {name(), CheckStatus::Pass,
          "anomaly labels, threshold, robust scaling, and causal windows are "
          "valid"};
}

} // namespace zetta::pipeline
