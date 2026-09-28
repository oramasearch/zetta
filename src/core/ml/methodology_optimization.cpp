#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "zetta/data/dataset_path.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/optimization.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::ml {
namespace {

bool m16_model_kind(std::string_view value) {
  return value == "optimization" || value == "operations_research";
}

bool m16_owns(const ir::ExecutionGraph &graph) {
  for (const std::shared_ptr<ir::Node> &node : graph.nodes()) {
    if (node->kind != ir::StepKind::Model || !node->attributes.is_object()) {
      continue;
    }
    const auto kind = node->attributes.find("model_kind");
    if (kind != node->attributes.end() && kind->is_string() &&
        m16_model_kind(kind->get<std::string>())) {
      return true;
    }
  }
  return false;
}

ir::ExecutionGraph build_m16_shape(const ShapeRequest &request) {
  const std::string source =
      request.hints.value("source", std::string("datasets"));
  const std::string table =
      request.hints.value("table", std::string("optimization_input"));

  ir::ExecutionGraph graph;
  graph.add_node(
      ir::StepKind::Connect, "Load scenarios and decision model",
      nlohmann::json{
          {"source", source},
          {"table", table},
          {"prediction_input", "scenario_or_quantile"},
          {"note",
           "variables, objectives, bounds, scenarios, and constraints"}},
      "load_optimization_model");
  graph.add_node(
      ir::StepKind::Clean, "Validate optimization master data",
      nlohmann::json{
          {"validate_master_data", true},
          {"reject_point_forecasts", true},
          {"note", "validate variable metadata and constraint coefficients"}},
      "validate_optimization_model");
  graph.add_node(ir::StepKind::Model, "Solve allocation plan",
                 nlohmann::json{{"model_kind", "optimization"},
                                {"solver", "revised_simplex_branch_and_bound"},
                                {"scenario_input", true},
                                {"verify_constraints", true},
                                {"report_solver_status", true}},
                 "solve_optimization_plan");
  graph.add_node(ir::StepKind::Act, "Publish plan and constraint trace",
                 nlohmann::json{{"outputs", nlohmann::json::array(
                                                {"plan", "constraint_trace",
                                                 "solver_status"})}},
                 "publish_optimization_plan");
  graph.add_edge("load_optimization_model", "validate_optimization_model");
  graph.add_edge("validate_optimization_model", "solve_optimization_plan");
  graph.add_edge("solve_optimization_plan", "publish_optimization_plan");
  return graph;
}

std::optional<std::string>
pipeline_dataset_path(const ir::ExecutionGraph &graph) {
  for (const std::shared_ptr<ir::Node> &node : graph.nodes()) {
    if (node->kind != ir::StepKind::Connect || !node->attributes.is_object()) {
      continue;
    }
    const std::string source = node->attributes.value("source", std::string{});
    const std::string table = node->attributes.value("table", std::string{});
    if (!source.empty() && !table.empty()) {
      return source + "." + table;
    }
  }
  return std::nullopt;
}

OptimizationParams optimization_params(const nlohmann::json &input) {
  OptimizationParams params;
  const auto found = input.find("optimization");
  if (found == input.end() || !found->is_object()) {
    return params;
  }
  const nlohmann::json &value = *found;
  params.maximize = value.value("maximize", params.maximize);
  params.iteration_limit =
      value.value("iteration_limit", params.iteration_limit);
  params.node_limit = value.value("node_limit", params.node_limit);
  params.time_limit = std::chrono::milliseconds(
      value.value("time_limit_ms", params.time_limit.count()));
  params.feasibility_tolerance =
      value.value("feasibility_tolerance", params.feasibility_tolerance);
  params.integrality_tolerance =
      value.value("integrality_tolerance", params.integrality_tolerance);
  params.seed = value.value("seed", params.seed);
  return params;
}

data::FeaturePlanOptions optimization_feature_options(
    Node &node, const data::ResolvedDataset &dataset) {
  data::FeaturePlanOptions options;
  options.entity_column = "row_id";
  options.native_entries = {
      {{"variable"}, {"variable"}, "assemble named decision variables",
       "constraint coefficients must refer to the same decision variable"},
      {{"objective"}, {"objective"}, "native linear objective coefficient",
       "the solver optimizes the declared objective, without scaling its units"},
      {{"integer"}, {"integer"}, "native variable integrality constraint",
       "integer is solver metadata, not a supervised label"}};
  for (const auto &summary : node.datasets()) {
    if (summary.table_id != dataset.table_id) {
      continue;
    }
    for (const auto *column : {"lower_bound", "upper_bound", "current_value",
                               "scenario", "quantile", "prediction_kind",
                               "constraint", "coefficient", "sense", "rhs"}) {
      if (std::find(summary.columns.begin(), summary.columns.end(), column) ==
          summary.columns.end()) {
        continue;
      }
      options.native_entries.push_back(
          {{column}, {column}, "native optimization model assembly and validation",
           "the solver reads this bound, baseline, uncertainty marker, or constraint field without learned encoding"});
    }
  }
  return options;
}

api::ApiResult run_m16(RunRequest &request) {
  if (request.context == nullptr || request.project == nullptr ||
      request.graph == nullptr) {
    return api::ApiResult::failure(
        "engine_error", "M-16 run request is missing its engine context");
  }
  api::ApiContext &context = *request.context;
  std::string dataset_path = request.input.value("dataset_path", std::string{});
  if (dataset_path.empty()) {
    const std::optional<std::string> graph_path =
        pipeline_dataset_path(request.graph->graph());
    if (!graph_path) {
      return api::ApiResult::failure(
          "invalid_input",
          "M-16 needs dataset_path or a connect step with source and table");
    }
    dataset_path = *graph_path;
  }
  const data::DatasetPath path = data::parse_dataset_path(dataset_path);
  if (path.kind != data::DatasetPathKind::Table) {
    return api::ApiResult::failure(
        "invalid_input",
        "M-16 reads a landed table; dataset_path must be database.table");
  }

  data::DatasetContract contract;
  contract.entity_column = "row_id";
  contract.label_column = "integer";
  contract.feature_columns = {"objective"};
  const data::DatasetSampler sampler =
      data::default_dataset_sampler(context.node, context.cluster);
  const data::ResolvedDataset dataset =
      data::resolve_dataset(context.node, path, contract, sampler);
  if (!dataset.ok()) {
    return run_dataset_failure(request, "M-16", dataset);
  }

  // The resolver uses integer as a contract placeholder. The solver consumes
  // it as variable metadata, so there is no supervised target for planning.
  auto planned_dataset = dataset;
  planned_dataset.contract.label_column.clear();
  const auto feature_plan = plan_run_features(
      request, "M-16", planned_dataset,
      optimization_feature_options(context.node, dataset));
  if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
    return api::ApiResult::failure("invalid_input", reason);
  }

  OptimizationRunOptions options;
  options.project = request.project->id();
  options.graph = request.graph->id();
  options.model_name =
      request.input.value("model_name", std::string("optimization-plan"));
  const OptimizationRunResult run =
      run_optimization(context.node, dataset, sampler,
                       optimization_params(request.input), options);
  if (!run.ok()) {
    return api::ApiResult::failure("engine_error", run.error);
  }
  record_run_features(context.node, run.run->id(), feature_plan);

  nlohmann::json output{
      {"run_id", run.run->id()},
      {"engine", "optimization"},
      {"status", std::string(to_string(run.run->status()))},
      {"solver_status", std::string(to_string(run.solution.status))},
      {"model_id", run.model_id}};
  if (!run.plan_table.empty()) {
    output["plan_table"] = run.plan_table;
  }
  if (!run.constraint_table.empty()) {
    output["constraint_table"] = run.constraint_table;
  }
  output["dataset"] = data::to_json(dataset.contract);
  output["dataset_notes"] = dataset.notes;
  return api::ApiResult::success(std::move(output));
}

} // namespace

Methodology make_m16_methodology() {
  Methodology methodology;
  methodology.id = "M-16";
  methodology.name = "Optimization / operations research";
  methodology.family = "Action & decision";
  methodology.model_kinds = {"optimization", "operations_research"};
  methodology.solutions = {};
  methodology.keywords = {"optimize",    "optimise",     "schedule",
                          "allocate",    "allocation",   "capacity",
                          "constraints", "replenishment"};
  methodology.inputs = {
      {"variable", true, "decision variable and bounds"},
      {"objective", true, "cost or value coefficient"},
      {"constraint", true, "resource or policy constraint"},
      {"scenario", true, "scenario or quantile prediction input"},
      {"group", false, "allocation group or resource pool"},
  };
  methodology.outputs = {"plan", "constraint trace", "solver status"};
  methodology.metrics = {"objective value", "current plan objective",
                         "objective delta", "constraint violations",
                         "optimality gap"};
  methodology.issue = "https://github.com/oramasearch/zetta/issues/1055";
  methodology.runnable = true;
  methodology.build_shape = build_m16_shape;
  methodology.owns = m16_owns;
  methodology.run = run_m16;
  return methodology;
}

} // namespace zetta::ml
