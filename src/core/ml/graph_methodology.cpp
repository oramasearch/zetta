#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/graph.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace zetta::ml {
namespace {

std::string hint_string(const ShapeRequest &request, std::string_view key,
                        std::string fallback) {
  const auto found = request.hints.find(key);
  if (found == request.hints.end() || !found->is_string() ||
      found->get<std::string>().empty()) {
    return fallback;
  }
  return found->get<std::string>();
}

double hint_number(const ShapeRequest &request, std::string_view key,
                   double fallback) {
  const auto found = request.hints.find(key);
  if (found == request.hints.end() || !found->is_number()) {
    return fallback;
  }
  return found->get<double>();
}

ir::ExecutionGraph build_graph_shape(const ShapeRequest &request) {
  const std::string source = hint_string(request, "source", "datasets");
  const std::string table = hint_string(request, "table", "edges");
  const std::string source_column =
      hint_string(request, "source_column", "src");
  const std::string destination_column =
      hint_string(request, "destination_column", "dst");
  const std::string weight_column =
      hint_string(request, "weight_column", "weight");
  const std::string time_column = hint_string(request, "time_column", "time");
  const std::string label_column =
      hint_string(request, "label_column", "label");
  const double prediction_time = hint_number(request, "prediction_time", 0.0);

  ir::ExecutionGraph graph;
  graph.add_node(ir::StepKind::Connect, "Load relationship edges",
                 {{"source", source},
                  {"table", table},
                  {"schema",
                   {{source_column, "int64"},
                    {destination_column, "int64"},
                    {weight_column, "float64"},
                    {time_column, "float64"},
                    {label_column, "int64"}}}},
                 "load_graph_edges");
  graph.add_node(ir::StepKind::Clean, "Exclude edges after prediction time",
                 {{"filter_time_column", time_column},
                  {"prediction_time", prediction_time},
                  {"exclude_after_prediction_time", true}},
                 "filter_graph_edges");
  graph.add_node(ir::StepKind::Model, "Learn relationship features",
                 {{"model_kind", "graph"},
                  {"source_column", source_column},
                  {"destination_column", destination_column},
                  {"weight_column", weight_column},
                  {"time_column", time_column},
                  {"label_column", label_column},
                  {"prediction_time", prediction_time},
                  {"requires",
                   {{source_column, "int64"},
                    {destination_column, "int64"},
                    {weight_column, "float64"},
                    {time_column, "float64"},
                    {label_column, "int64"}}}},
                 "train_graph_model");
  graph.add_node(ir::StepKind::Serve, "Write joinable node features",
                 {{"outputs", {"node_features", "graph_summary"}}},
                 "write_graph_features");
  graph.add_edge("load_graph_edges", "filter_graph_edges");
  graph.add_edge("filter_graph_edges", "train_graph_model");
  graph.add_edge("train_graph_model", "write_graph_features");
  return graph;
}

bool graph_owns(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes_of_kind(ir::StepKind::Model)) {
    if (!node || !node->attributes.is_object()) {
      continue;
    }
    const auto kind = node->attributes.find("model_kind");
    if (kind != node->attributes.end() && kind->is_string()) {
      const std::string value = kind->get<std::string>();
      return value == "graph" || value == "graph_ml";
    }
  }
  return false;
}

const ir::Node *graph_model_node(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes_of_kind(ir::StepKind::Model)) {
    if (node && node->attributes.is_object()) {
      return node.get();
    }
  }
  return nullptr;
}

std::string model_string(const ir::Node &node, std::string_view key,
                         std::string fallback) {
  const auto found = node.attributes.find(key);
  if (found == node.attributes.end() || !found->is_string()) {
    return fallback;
  }
  return found->get<std::string>();
}

std::optional<std::string> pipeline_dataset(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes_of_kind(ir::StepKind::Connect)) {
    if (!node || !node->attributes.is_object()) {
      continue;
    }
    const auto source = node->attributes.find("source");
    const auto table = node->attributes.find("table");
    if (source != node->attributes.end() && source->is_string() &&
        table != node->attributes.end() && table->is_string()) {
      return source->get<std::string>() + "." + table->get<std::string>();
    }
  }
  return std::nullopt;
}

api::ApiResult run_graph_methodology(RunRequest &request) {
  if (request.context == nullptr || request.project == nullptr ||
      request.graph == nullptr) {
    return api::ApiResult::failure(
        "engine_error", "M-12 run request is missing its engine context");
  }
  const ir::Node *model = graph_model_node(request.graph->graph());
  if (model == nullptr) {
    return api::ApiResult::failure("engine_error",
                                   "M-12 graph has no model step");
  }

  std::string dataset_path = request.input.value("dataset_path", std::string{});
  if (dataset_path.empty()) {
    const std::optional<std::string> connected =
        pipeline_dataset(request.graph->graph());
    if (!connected) {
      return api::ApiResult::failure(
          "invalid_input",
          "M-12 needs dataset_path or a connect step with source and table");
    }
    dataset_path = *connected;
  }
  const data::DatasetPath path = data::parse_dataset_path(dataset_path);
  if (path.kind != data::DatasetPathKind::Table) {
    return api::ApiResult::failure("invalid_input",
                                   "M-12 reads a database.table edge dataset");
  }

  data::DatasetContract contract;
  if (const auto given = request.input.find("dataset");
      given != request.input.end()) {
    std::string reason;
    const std::optional<data::DatasetContract> parsed =
        data::dataset_contract_from_json(*given, &reason);
    if (!parsed) {
      return api::ApiResult::failure("invalid_input", reason);
    }
    contract = *parsed;
  } else {
    contract.entity_column = model_string(*model, "source_column", "src");
    contract.label_column = model_string(*model, "label_column", "label");
    contract.time_column = model_string(*model, "time_column", "time");
    contract.feature_columns = {
        model_string(*model, "destination_column", "dst"),
        model_string(*model, "weight_column", "weight")};
    contract.as_of = model->attributes.value("prediction_time", 0.0);
  }

  api::ApiContext &context = *request.context;
  data::ResolvedDataset resolved = data::resolve_dataset(
      context.node, path, contract,
      data::default_dataset_sampler(context.node, context.cluster));
  if (!resolved.ok()) {
    return run_dataset_failure(request, "M-12", resolved);
  }

  GraphRunOptions options;
  options.params.prediction_time = resolved.contract.as_of;
  options.params.held_out_fraction =
      request.input.value("holdout_fraction", 0.2);
  options.project = request.project->id();
  options.graph = request.graph->id();
  options.model_name = request.input.value("model_name", "graph_model");
  data::FeaturePlanOptions feature_options;
  if (resolved.contract.feature_columns.size() >= 2) {
    std::vector<std::string> names = {
        "in_degree", "out_degree", "weighted_in_degree", "weighted_out_degree",
        "page_rank", "component", "clustering_coefficient"};
    for (std::size_t dimension = 0; dimension < options.params.embedding_dimensions; ++dimension) {
      names.push_back("embedding_" + std::to_string(dimension));
    }
    feature_options.native_entries.push_back({
        {resolved.contract.entity_column, resolved.contract.feature_columns[0],
         resolved.contract.feature_columns[1], resolved.contract.time_column},
        std::move(names),
        "time-bounded weighted graph statistics and native node embeddings",
        "relationship structure is computed only from edges before prediction_time"});
  }
  const auto feature_plan = plan_run_features(request, "M-12", resolved, feature_options);
  if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
    return api::ApiResult::failure("invalid_input", error);
  }
  GraphRunResult result = run_graph(context.node, resolved, options);
  if (!result.ok()) {
    return api::ApiResult::failure("engine_error", result.error);
  }

  nlohmann::json output;
  record_run_features(context.node, result.run_id, feature_plan);
  output["run_id"] = result.run_id;
  output["engine"] = "graph";
  output["status"] = "succeeded";
  output["dataset"] = data::to_json(resolved.contract);
  output["dataset_notes"] = resolved.notes;
  return api::ApiResult::success(std::move(output));
}

} // namespace

Methodology make_graph_methodology() {
  Methodology methodology;
  methodology.id = "M-12";
  methodology.name = "Graph ML";
  methodology.family = "Identity & structure";
  methodology.model_kinds = {"graph", "graph_ml"};
  methodology.solutions = {};
  methodology.keywords = {"graph", "network", "relationships",
                          "link prediction", "connected accounts"};
  methodology.inputs = {
      {"entity", true, "source node identifier"},
      {"group", true, "destination node identifier"},
      {"feature", true, "edge weight"},
      {"time", true, "edge observation time and prediction cutoff"},
      {"label", true, "source node task label"},
  };
  methodology.outputs = {"node feature table", "graph summary", "node scores",
                         "graph embeddings"};
  methodology.metrics = {"held-out node accuracy", "component count",
                         "degree distribution", "PageRank top nodes"};
  methodology.issue = "https://github.com/oramasearch/zetta/issues/1051";
  methodology.runnable = true;
  methodology.build_shape = build_graph_shape;
  methodology.owns = graph_owns;
  methodology.run = run_graph_methodology;
  return methodology;
}

} // namespace zetta::ml
