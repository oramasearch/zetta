#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zetta {
class Node;
}
namespace zetta::data {
struct ResolvedDataset;
}

// Native graph learning for M-12 (#1051).
//
// Edge rows are divided into owned partitions. The algorithm combines
// partition-local statistics in stable node-id order, making a one-partition
// and multi-partition run identical. No foreign ML runtime or pretrained
// weights are used.
//
// See docs/ml/graph.md.
namespace zetta::ml {

struct GraphEdge {
  std::int64_t source = 0;
  std::int64_t destination = 0;
  double weight = 1.0;
  double time = 0.0;
  std::optional<int> source_label;
  std::size_t observations = 1;
};

struct GraphParams {
  double prediction_time = 0.0;
  double page_rank_damping = 0.85;
  std::size_t page_rank_iterations = 40;
  std::size_t label_iterations = 30;
  double held_out_fraction = 0.2;
  std::size_t embedding_dimensions = 8;
  std::size_t walks_per_node = 4;
  std::size_t walk_length = 12;
  std::size_t context_window = 2;
  std::size_t skip_gram_epochs = 3;
  double skip_gram_learning_rate = 0.025;
  std::uint64_t seed = 1051;
};

struct GraphNodeFeature {
  std::int64_t node = 0;
  double in_degree = 0.0;
  double out_degree = 0.0;
  double weighted_in_degree = 0.0;
  double weighted_out_degree = 0.0;
  double page_rank = 0.0;
  std::int64_t component = 0;
  int propagated_label = 0;
  double label_score = 0.0;
  double clustering_coefficient = 0.0;
  std::vector<double> embedding;

  bool operator==(const GraphNodeFeature &) const = default;
};

struct GraphDegreeDistribution {
  double minimum = 0.0;
  double maximum = 0.0;
  double mean = 0.0;
};

struct GraphRankedNode {
  std::int64_t node = 0;
  double page_rank = 0.0;
};

struct GraphReport {
  std::size_t nodes = 0;
  std::size_t edges_read = 0;
  std::size_t edges_excluded = 0;
  std::size_t component_count = 0;
  std::vector<std::size_t> component_sizes;
  GraphDegreeDistribution degree_distribution;
  std::vector<GraphRankedNode> page_rank_top_nodes;
  std::size_t held_out_nodes = 0;
  double held_out_accuracy = 0.0;
};

struct GraphResult {
  std::vector<GraphNodeFeature> features;
  GraphReport report;
  std::string error;

  bool ok() const noexcept { return error.empty(); }
};

struct GraphRunOptions {
  GraphParams params;
  std::string project;
  std::string graph;
  std::string model_name = "graph_model";
};

struct GraphRunResult {
  GraphResult graph;
  std::string run_id;
  std::string model_id;
  std::string node_features_table;
  std::string summary_table;
  std::string error;

  bool ok() const noexcept { return error.empty(); }
};

// Compute M-12 features from disjoint edge partitions. Edges after
// `prediction_time` are excluded before any statistic, walk, embedding update,
// or evaluation sees them. Invalid weights/times and inconsistent labels are
// refused rather than repaired silently.
GraphResult
compute_graph_features(const std::vector<std::vector<GraphEdge>> &partitions,
                       const GraphParams &params = {});

// Resolve the edge rows named by `dataset`, execute the native graph core, and
// materialize a node-feature table plus a summary table with Run and Model
// lineage. The dataset contract maps entity=source, features[0]=destination,
// features[1]=weight, time=time, and label=source node label.
GraphRunResult run_graph(Node &node, const data::ResolvedDataset &dataset,
                         const GraphRunOptions &options = {});

} // namespace zetta::ml
