#include "zetta/ml/graph.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kResultsDatabase = "graph_results";

std::string table_suffix(std::string value) {
  if (value.empty()) {
    return "pipeline";
  }
  for (char &character : value) {
    const unsigned char candidate = static_cast<unsigned char>(character);
    if (std::isalnum(candidate) == 0) {
      character = '_';
    }
  }
  value.resize(std::min<std::size_t>(value.size(), 16));
  return value;
}

std::optional<double> number(const nlohmann::json &row,
                             const std::string &column) {
  const auto found = row.find(column);
  if (found == row.end() || !found->is_number()) {
    return std::nullopt;
  }
  return found->get<double>();
}

std::optional<std::int64_t> integer(const nlohmann::json &row,
                                    const std::string &column) {
  const auto found = row.find(column);
  if (found == row.end() || !found->is_number_integer()) {
    return std::nullopt;
  }
  return found->get<std::int64_t>();
}

std::optional<int> label(const nlohmann::json &row, const std::string &column) {
  if (column.empty()) {
    return std::nullopt;
  }
  const std::optional<std::int64_t> value = integer(row, column);
  if (!value || (*value != 0 && *value != 1)) {
    return std::nullopt;
  }
  return static_cast<int>(*value);
}

std::string validate_contract(const data::ResolvedDataset &dataset) {
  if (!dataset.ok()) {
    return dataset.error;
  }
  if (dataset.contract.entity_column.empty()) {
    return "graph dataset: entity_column must name the edge source";
  }
  if (dataset.contract.feature_columns.size() < 2) {
    return "graph dataset: feature_columns must start with destination and "
           "weight";
  }
  if (dataset.contract.time_column.empty()) {
    return "graph dataset: time_column is required; graph runs never infer the "
           "prediction cutoff";
  }
  if (dataset.contract.label_column.empty()) {
    return "graph dataset: label_column is required for label propagation and "
           "held-out evaluation";
  }
  return {};
}

nlohmann::json model_metrics(const GraphReport &report) {
  return {{"nodes", static_cast<double>(report.nodes)},
          {"edges_read", static_cast<double>(report.edges_read)},
          {"edges_excluded", static_cast<double>(report.edges_excluded)},
          {"component_count", static_cast<double>(report.component_count)},
          {"degree_min", report.degree_distribution.minimum},
          {"degree_max", report.degree_distribution.maximum},
          {"degree_mean", report.degree_distribution.mean},
          {"held_out_nodes", static_cast<double>(report.held_out_nodes)},
          {"held_out_accuracy", report.held_out_accuracy}};
}

nlohmann::json model_weights(const GraphResult &graph,
                             const GraphParams &params) {
  nlohmann::json nodes = nlohmann::json::array();
  for (const GraphNodeFeature &feature : graph.features) {
    nodes.push_back({{"node", feature.node}, {"embedding", feature.embedding}});
  }
  return {{"kind", "graph"},
          {"seed", params.seed},
          {"prediction_time", params.prediction_time},
          {"embedding_dimensions", params.embedding_dimensions},
          {"nodes", std::move(nodes)}};
}

std::string fail_run(const std::shared_ptr<pipeline::Run> &run,
                     std::string reason) {
  if (run) {
    run->add_log({util::now_iso8601(), "error", reason});
    run->set_status(pipeline::RunStatus::Failed);
  }
  return reason;
}

} // namespace

GraphRunResult run_graph(Node &node, const data::ResolvedDataset &dataset,
                         const GraphRunOptions &options) {
  GraphRunResult output;
  const std::shared_ptr<pipeline::Run> run =
      node.create_run(options.project, options.graph);
  if (!run) {
    output.error = "graph: could not create the Run record";
    return output;
  }
  output.run_id = run->id();
  run->set_status(pipeline::RunStatus::Running);
  run->add_log({util::now_iso8601(), "info",
                "graph: resolving the edge table at the prediction cutoff"});

  if (const std::string contract_error = validate_contract(dataset);
      !contract_error.empty()) {
    output.error = fail_run(run, contract_error);
    return output;
  }

  std::string sample_error;
  const data::DatasetSampler sampler = data::default_dataset_sampler(node);
  const std::vector<nlohmann::json> rows =
      sampler(dataset.database, dataset.table,
              std::numeric_limits<std::size_t>::max(), sample_error);
  if (!sample_error.empty()) {
    output.error = fail_run(run, "graph dataset: " + sample_error);
    return output;
  }

  const std::string &source_column = dataset.contract.entity_column;
  const std::string &destination_column = dataset.contract.feature_columns[0];
  const std::string &weight_column = dataset.contract.feature_columns[1];
  std::vector<GraphEdge> edges;
  edges.reserve(rows.size());
  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    const nlohmann::json &row = rows[row_index];
    const std::optional<std::int64_t> source = integer(row, source_column);
    const std::optional<std::int64_t> destination =
        integer(row, destination_column);
    const std::optional<double> weight = number(row, weight_column);
    const std::optional<double> time =
        number(row, dataset.contract.time_column);
    if (!source || !destination || !weight || !time) {
      output.error = fail_run(
          run, "graph dataset row " + std::to_string(row_index) +
                   ": source and destination must be integers; weight and time "
                   "must be numeric");
      return output;
    }
    const auto label_value = row.find(dataset.contract.label_column);
    if (label_value == row.end() || label_value->is_null()) {
      edges.push_back({.source = *source,
                       .destination = *destination,
                       .weight = *weight,
                       .time = *time});
      continue;
    }
    const std::optional<int> source_label =
        label(row, dataset.contract.label_column);
    if (!source_label) {
      output.error =
          fail_run(run, "graph dataset row " + std::to_string(row_index) +
                            ": label '" + dataset.contract.label_column +
                            "' must be zero, one, or null");
      return output;
    }
    edges.push_back({.source = *source,
                     .destination = *destination,
                     .weight = *weight,
                     .time = *time,
                     .source_label = *source_label});
  }

  GraphParams params = options.params;
  params.prediction_time = dataset.contract.as_of;
  output.graph = compute_graph_features({std::move(edges)}, params);
  if (!output.graph.ok()) {
    output.error = fail_run(run, output.graph.error);
    return output;
  }

  const std::string suffix = table_suffix(options.graph);
  const std::string feature_name = "node_features_" + suffix;
  const std::string summary_name = "summary_" + suffix;
  std::shared_ptr<data::Database> database =
      node.find_database(std::string(kResultsDatabase));
  if (!database) {
    database = node.create_database(std::string(kResultsDatabase));
  }
  if (!database) {
    output.error =
        fail_run(run, "graph: could not create the results database");
    return output;
  }
  database->drop_table(feature_name);
  database->drop_table(summary_name);
  const std::shared_ptr<data::Table> feature_table =
      database->create_table(feature_name);
  const std::shared_ptr<data::Table> summary_table =
      database->create_table(summary_name);
  if (!feature_table || !summary_table) {
    output.error = fail_run(run, "graph: could not create the output tables");
    return output;
  }

  for (const GraphNodeFeature &feature : output.graph.features) {
    nlohmann::json row{
        {"node", feature.node},
        {"in_degree", feature.in_degree},
        {"out_degree", feature.out_degree},
        {"weighted_in_degree", feature.weighted_in_degree},
        {"weighted_out_degree", feature.weighted_out_degree},
        {"page_rank", feature.page_rank},
        {"component", feature.component},
        {"propagated_label", feature.propagated_label},
        {"label_score", feature.label_score},
        {"clustering_coefficient", feature.clustering_coefficient}};
    for (std::size_t dimension = 0; dimension < feature.embedding.size();
         ++dimension) {
      row["embedding_" + std::to_string(dimension)] =
          feature.embedding[dimension];
    }
    feature_table->add_row(std::move(row));
  }

  nlohmann::json component_sizes = nlohmann::json::array();
  for (std::size_t size : output.graph.report.component_sizes) {
    component_sizes.push_back(size);
  }
  nlohmann::json top_nodes = nlohmann::json::array();
  for (const GraphRankedNode &ranked :
       output.graph.report.page_rank_top_nodes) {
    top_nodes.push_back(
        {{"node", ranked.node}, {"page_rank", ranked.page_rank}});
  }
  summary_table->add_row(
      {{"component_count", output.graph.report.component_count},
       {"component_sizes", std::move(component_sizes)},
       {"degree_min", output.graph.report.degree_distribution.minimum},
       {"degree_max", output.graph.report.degree_distribution.maximum},
       {"degree_mean", output.graph.report.degree_distribution.mean},
       {"page_rank_top_nodes", std::move(top_nodes)},
       {"edges_read", output.graph.report.edges_read},
       {"edges_excluded", output.graph.report.edges_excluded},
       {"held_out_nodes", output.graph.report.held_out_nodes},
       {"held_out_accuracy", output.graph.report.held_out_accuracy}});

  ml::ModelValue model_value;
  model_value.name = options.model_name;
  model_value.kind = "graph";
  model_value.project = options.project;
  model_value.run = run->id();
  nlohmann::json feature_names = {
      "in_degree",           "out_degree",  "weighted_in_degree",
      "weighted_out_degree", "page_rank",   "component",
      "propagated_label",    "label_score", "clustering_coefficient"};
  for (std::size_t dimension = 0; dimension < params.embedding_dimensions;
       ++dimension) {
    feature_names.push_back("embedding_" + std::to_string(dimension));
  }
  model_value.feature_schema = {{"node_key", "node"},
                                {"features", std::move(feature_names)}};
  model_value.metrics = model_metrics(output.graph.report);
  const std::shared_ptr<ml::Model> model = node.create_model(model_value);
  if (!model ||
      !model->set_weights(model_weights(output.graph, params).dump())) {
    output.error = fail_run(run, "graph: could not register the graph model");
    return output;
  }
  output.model_id = model->id();
  output.node_features_table =
      std::string(kResultsDatabase) + "." + feature_name;
  output.summary_table = std::string(kResultsDatabase) + "." + summary_name;

  run->set_outputs({{"model", "graph model", output.model_id},
                    {"dataset", "node features", output.node_features_table},
                    {"dataset", "graph summary", output.summary_table},
                    {"report", "run report", run->id()}});
  pipeline::RunLineage lineage;
  lineage.sources = {"dataset:" + dataset.database + "." + dataset.table};
  lineage.steps = {"filter_edges_at_prediction_time",
                   "graph_features",
                   "page_rank",
                   "connected_components",
                   "label_propagation",
                   "node2vec"};
  lineage.freshness = util::now_iso8601();
  lineage.confidence = output.graph.report.held_out_accuracy;
  run->set_lineage(std::move(lineage));
  run->add_log({util::now_iso8601(), "info",
                "graph: read " +
                    std::to_string(output.graph.report.edges_read) +
                    " edges and excluded " +
                    std::to_string(output.graph.report.edges_excluded) +
                    " after the prediction cutoff"});
  run->set_status(pipeline::RunStatus::Succeeded);
  return output;
}

} // namespace zetta::ml
