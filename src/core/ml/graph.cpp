#include "zetta/ml/graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

using Matrix = std::vector<std::vector<double>>;

double sigmoid(double value) {
  if (value >= 0.0) {
    const double exponential = std::exp(-value);
    return 1.0 / (1.0 + exponential);
  }
  const double exponential = std::exp(value);
  return exponential / (1.0 + exponential);
}

std::uint64_t mixed_hash(std::int64_t node, std::uint64_t seed) {
  std::uint64_t value = static_cast<std::uint64_t>(node) ^ seed;
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

bool held_out(std::int64_t node, const GraphParams &params) {
  if (params.held_out_fraction <= 0.0) {
    return false;
  }
  if (params.held_out_fraction >= 1.0) {
    return true;
  }
  const double unit =
      static_cast<double>(mixed_hash(node, params.seed) >> 11U) /
      static_cast<double>(1ULL << 53U);
  return unit < params.held_out_fraction;
}

std::vector<double> page_rank(const Matrix &directed,
                              const GraphParams &params) {
  const std::size_t count = directed.size();
  if (count == 0) {
    return {};
  }
  std::vector<double> outgoing(count, 0.0);
  for (std::size_t source = 0; source < count; ++source) {
    outgoing[source] =
        std::accumulate(directed[source].begin(), directed[source].end(), 0.0);
  }
  std::vector<double> rank(count, 1.0 / static_cast<double>(count));
  std::vector<double> next(count, 0.0);
  for (std::size_t iteration = 0; iteration < params.page_rank_iterations;
       ++iteration) {
    double dangling = 0.0;
    for (std::size_t source = 0; source < count; ++source) {
      if (outgoing[source] == 0.0) {
        dangling += rank[source];
      }
    }
    const double base =
        (1.0 - params.page_rank_damping) / static_cast<double>(count) +
        params.page_rank_damping * dangling / static_cast<double>(count);
    std::fill(next.begin(), next.end(), base);
    for (std::size_t source = 0; source < count; ++source) {
      if (outgoing[source] == 0.0) {
        continue;
      }
      for (std::size_t destination = 0; destination < count; ++destination) {
        if (directed[source][destination] == 0.0) {
          continue;
        }
        next[destination] += params.page_rank_damping * rank[source] *
                             directed[source][destination] / outgoing[source];
      }
    }
    rank.swap(next);
  }
  return rank;
}

std::vector<std::int64_t> components(const Matrix &undirected,
                                     const std::vector<std::int64_t> &nodes) {
  std::vector<std::int64_t> labels = nodes;
  for (std::size_t iteration = 0; iteration < nodes.size(); ++iteration) {
    std::vector<std::int64_t> next = labels;
    for (std::size_t source = 0; source < nodes.size(); ++source) {
      for (std::size_t destination = 0; destination < nodes.size();
           ++destination) {
        if (undirected[source][destination] > 0.0) {
          next[source] = std::min(next[source], labels[destination]);
        }
      }
    }
    if (next == labels) {
      break;
    }
    labels.swap(next);
  }
  return labels;
}

std::pair<std::vector<int>, std::vector<double>>
propagate_labels(const Matrix &undirected,
                 const std::vector<std::int64_t> &nodes,
                 const std::map<std::int64_t, int> &known_labels,
                 const GraphParams &params) {
  const std::size_t count = nodes.size();
  Matrix scores(count, std::vector<double>(2, 0.0));
  std::vector<bool> seed(count, false);
  for (std::size_t index = 0; index < count; ++index) {
    const auto label = known_labels.find(nodes[index]);
    if (label == known_labels.end() || held_out(nodes[index], params)) {
      continue;
    }
    scores[index][static_cast<std::size_t>(label->second)] = 1.0;
    seed[index] = true;
  }
  for (std::size_t iteration = 0; iteration < params.label_iterations;
       ++iteration) {
    Matrix next(count, std::vector<double>(2, 0.0));
    for (std::size_t node = 0; node < count; ++node) {
      if (seed[node]) {
        next[node] = scores[node];
        continue;
      }
      double total_weight = 0.0;
      for (std::size_t neighbor = 0; neighbor < count; ++neighbor) {
        const double weight = undirected[node][neighbor];
        if (weight == 0.0) {
          continue;
        }
        total_weight += weight;
        next[node][0] += weight * scores[neighbor][0];
        next[node][1] += weight * scores[neighbor][1];
      }
      if (total_weight > 0.0) {
        next[node][0] /= total_weight;
        next[node][1] /= total_weight;
      }
    }
    scores.swap(next);
  }

  std::vector<int> labels(count, 0);
  std::vector<double> probabilities(count, 0.5);
  for (std::size_t index = 0; index < count; ++index) {
    const double total = scores[index][0] + scores[index][1];
    if (total > 0.0) {
      probabilities[index] = scores[index][1] / total;
    }
    labels[index] = probabilities[index] >= 0.5 ? 1 : 0;
  }
  return {std::move(labels), std::move(probabilities)};
}

std::vector<double> clustering_coefficients(const Matrix &undirected) {
  const std::size_t count = undirected.size();
  std::vector<double> result(count, 0.0);
  for (std::size_t node = 0; node < count; ++node) {
    std::vector<std::size_t> neighbors;
    for (std::size_t candidate = 0; candidate < count; ++candidate) {
      if (candidate != node && undirected[node][candidate] > 0.0) {
        neighbors.push_back(candidate);
      }
    }
    if (neighbors.size() < 2) {
      continue;
    }
    std::size_t links = 0;
    for (std::size_t left = 0; left < neighbors.size(); ++left) {
      for (std::size_t right = left + 1; right < neighbors.size(); ++right) {
        if (undirected[neighbors[left]][neighbors[right]] > 0.0) {
          ++links;
        }
      }
    }
    result[node] =
        2.0 * static_cast<double>(links) /
        static_cast<double>(neighbors.size() * (neighbors.size() - 1));
  }
  return result;
}

std::size_t weighted_choice(const std::vector<double> &weights,
                            std::mt19937_64 &random) {
  const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
  if (total <= 0.0) {
    return weights.size();
  }
  std::uniform_real_distribution<double> distribution(0.0, total);
  double wanted = distribution(random);
  for (std::size_t index = 0; index < weights.size(); ++index) {
    wanted -= weights[index];
    if (wanted <= 0.0) {
      return index;
    }
  }
  return weights.size() - 1;
}

std::vector<std::vector<std::size_t>> random_walks(const Matrix &undirected,
                                                   const GraphParams &params) {
  const std::size_t count = undirected.size();
  std::mt19937_64 random(params.seed);
  std::vector<std::vector<std::size_t>> walks;
  walks.reserve(count * params.walks_per_node);
  for (std::size_t start = 0; start < count; ++start) {
    for (std::size_t walk_index = 0; walk_index < params.walks_per_node;
         ++walk_index) {
      std::vector<std::size_t> walk;
      walk.reserve(params.walk_length);
      walk.push_back(start);
      while (walk.size() < params.walk_length) {
        const std::size_t current = walk.back();
        const std::size_t next = weighted_choice(undirected[current], random);
        if (next == count) {
          break;
        }
        walk.push_back(next);
      }
      walks.push_back(std::move(walk));
    }
  }
  return walks;
}

void skip_gram_update(Matrix &inputs, Matrix &outputs, std::size_t center,
                      std::size_t context, double target, double rate) {
  double dot = 0.0;
  for (std::size_t dimension = 0; dimension < inputs[center].size();
       ++dimension) {
    dot += inputs[center][dimension] * outputs[context][dimension];
  }
  const double gradient = rate * (target - sigmoid(dot));
  for (std::size_t dimension = 0; dimension < inputs[center].size();
       ++dimension) {
    const double input = inputs[center][dimension];
    const double output = outputs[context][dimension];
    inputs[center][dimension] += gradient * output;
    outputs[context][dimension] += gradient * input;
  }
}

Matrix node2vec_embeddings(const Matrix &undirected,
                           const GraphParams &params) {
  const std::size_t count = undirected.size();
  Matrix inputs(count, std::vector<double>(params.embedding_dimensions, 0.0));
  Matrix outputs = inputs;
  if (count == 0 || params.embedding_dimensions == 0) {
    return inputs;
  }
  std::mt19937_64 random(params.seed ^ 0x6e6f646532766563ULL);
  const double scale = 0.5 / static_cast<double>(params.embedding_dimensions);
  std::uniform_real_distribution<double> initialization(-scale, scale);
  for (std::vector<double> &embedding : inputs) {
    for (double &value : embedding) {
      value = initialization(random);
    }
  }
  std::uniform_int_distribution<std::size_t> negative(0, count - 1);
  const std::vector<std::vector<std::size_t>> walks =
      random_walks(undirected, params);
  for (std::size_t epoch = 0; epoch < params.skip_gram_epochs; ++epoch) {
    for (const std::vector<std::size_t> &walk : walks) {
      for (std::size_t position = 0; position < walk.size(); ++position) {
        const std::size_t begin = position > params.context_window
                                      ? position - params.context_window
                                      : 0;
        const std::size_t end =
            std::min(walk.size(), position + params.context_window + 1);
        for (std::size_t context = begin; context < end; ++context) {
          if (context == position) {
            continue;
          }
          skip_gram_update(inputs, outputs, walk[position], walk[context], 1.0,
                           params.skip_gram_learning_rate);
          for (std::size_t sample = 0; sample < 2; ++sample) {
            skip_gram_update(inputs, outputs, walk[position], negative(random),
                             0.0, params.skip_gram_learning_rate);
          }
        }
      }
    }
  }
  return inputs;
}

std::string validate_params(const GraphParams &params) {
  if (!std::isfinite(params.prediction_time)) {
    return "graph.prediction_time: must be finite";
  }
  if (!(params.page_rank_damping > 0.0 && params.page_rank_damping < 1.0)) {
    return "graph.page_rank_damping: must be between zero and one";
  }
  if (params.page_rank_iterations == 0) {
    return "graph.page_rank_iterations: must be greater than zero";
  }
  if (params.held_out_fraction < 0.0 || params.held_out_fraction > 1.0) {
    return "graph.held_out_fraction: must be between zero and one";
  }
  return {};
}

} // namespace

GraphResult
compute_graph_features(const std::vector<std::vector<GraphEdge>> &partitions,
                       const GraphParams &params) {
  GraphResult result;
  result.error = validate_params(params);
  if (!result.error.empty()) {
    return result;
  }

  std::vector<GraphEdge> edges;
  for (const std::vector<GraphEdge> &partition : partitions) {
    for (const GraphEdge &edge : partition) {
      if (!std::isfinite(edge.time)) {
        result.error = "graph.edge.time: must be finite";
        return result;
      }
      if (!std::isfinite(edge.weight) || edge.weight < 0.0) {
        result.error = "graph.edge.weight: must be finite and non-negative";
        return result;
      }
      if (edge.observations == 0) {
        result.error = "graph.edge.observations: must be greater than zero";
        return result;
      }
      if (edge.source_label && *edge.source_label != 0 &&
          *edge.source_label != 1) {
        result.error = "graph.edge.source_label: must be zero or one";
        return result;
      }
      if (edge.time > params.prediction_time) {
        result.report.edges_excluded += edge.observations;
        continue;
      }
      edges.push_back(edge);
    }
  }
  std::sort(edges.begin(), edges.end(),
            [](const GraphEdge &left, const GraphEdge &right) {
              return std::tie(left.source, left.destination, left.time,
                              left.weight, left.source_label) <
                     std::tie(right.source, right.destination, right.time,
                              right.weight, right.source_label);
            });
  for (const GraphEdge &edge : edges) {
    result.report.edges_read += edge.observations;
  }
  if (edges.empty()) {
    result.error = "graph: no edges exist at or before the prediction cutoff";
    return result;
  }

  std::set<std::int64_t> node_set;
  std::map<std::int64_t, int> known_labels;
  for (const GraphEdge &edge : edges) {
    node_set.insert(edge.source);
    node_set.insert(edge.destination);
    if (!edge.source_label) {
      continue;
    }
    const auto existing = known_labels.find(edge.source);
    if (existing != known_labels.end() &&
        existing->second != *edge.source_label) {
      result.error = "graph: node " + std::to_string(edge.source) +
                     " has inconsistent labels";
      return result;
    }
    known_labels[edge.source] = *edge.source_label;
  }
  const std::vector<std::int64_t> nodes(node_set.begin(), node_set.end());
  result.report.nodes = nodes.size();
  std::map<std::int64_t, std::size_t> index_by_node;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    index_by_node[nodes[index]] = index;
  }

  const std::size_t count = nodes.size();
  Matrix directed(count, std::vector<double>(count, 0.0));
  Matrix undirected(count, std::vector<double>(count, 0.0));
  std::vector<double> in_degree(count, 0.0);
  std::vector<double> out_degree(count, 0.0);
  std::vector<double> weighted_in(count, 0.0);
  std::vector<double> weighted_out(count, 0.0);
  for (const GraphEdge &edge : edges) {
    const std::size_t source = index_by_node.at(edge.source);
    const std::size_t destination = index_by_node.at(edge.destination);
    directed[source][destination] += edge.weight;
    undirected[source][destination] += edge.weight;
    undirected[destination][source] += edge.weight;
    out_degree[source] += static_cast<double>(edge.observations);
    in_degree[destination] += static_cast<double>(edge.observations);
    weighted_out[source] += edge.weight;
    weighted_in[destination] += edge.weight;
  }

  const std::vector<double> ranks = page_rank(directed, params);
  const std::vector<std::int64_t> component = components(undirected, nodes);
  const auto [propagated, label_scores] =
      propagate_labels(undirected, nodes, known_labels, params);
  const std::vector<double> clustering = clustering_coefficients(undirected);
  Matrix embeddings = node2vec_embeddings(undirected, params);

  result.features.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result.features.push_back(
        GraphNodeFeature{.node = nodes[index],
                         .in_degree = in_degree[index],
                         .out_degree = out_degree[index],
                         .weighted_in_degree = weighted_in[index],
                         .weighted_out_degree = weighted_out[index],
                         .page_rank = ranks[index],
                         .component = component[index],
                         .propagated_label = propagated[index],
                         .label_score = label_scores[index],
                         .clustering_coefficient = clustering[index],
                         .embedding = std::move(embeddings[index])});
  }

  std::map<std::int64_t, std::size_t> component_sizes;
  for (std::int64_t label : component) {
    ++component_sizes[label];
  }
  result.report.component_count = component_sizes.size();
  for (const auto &[label, size] : component_sizes) {
    static_cast<void>(label);
    result.report.component_sizes.push_back(size);
  }
  std::sort(result.report.component_sizes.begin(),
            result.report.component_sizes.end(), std::greater<>());

  std::vector<double> total_degree(count, 0.0);
  for (std::size_t index = 0; index < count; ++index) {
    total_degree[index] = in_degree[index] + out_degree[index];
  }
  result.report.degree_distribution.minimum =
      *std::min_element(total_degree.begin(), total_degree.end());
  result.report.degree_distribution.maximum =
      *std::max_element(total_degree.begin(), total_degree.end());
  result.report.degree_distribution.mean =
      std::accumulate(total_degree.begin(), total_degree.end(), 0.0) /
      static_cast<double>(count);

  std::vector<std::size_t> ranked_indices(count);
  std::iota(ranked_indices.begin(), ranked_indices.end(), 0);
  std::stable_sort(ranked_indices.begin(), ranked_indices.end(),
                   [&](std::size_t left, std::size_t right) {
                     if (ranks[left] != ranks[right]) {
                       return ranks[left] > ranks[right];
                     }
                     return nodes[left] < nodes[right];
                   });
  ranked_indices.resize(std::min<std::size_t>(10, ranked_indices.size()));
  for (std::size_t index : ranked_indices) {
    result.report.page_rank_top_nodes.push_back(
        {.node = nodes[index], .page_rank = ranks[index]});
  }

  std::size_t correct = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const auto label = known_labels.find(nodes[index]);
    if (label == known_labels.end() || !held_out(nodes[index], params)) {
      continue;
    }
    ++result.report.held_out_nodes;
    if (propagated[index] == label->second) {
      ++correct;
    }
  }
  if (result.report.held_out_nodes > 0) {
    result.report.held_out_accuracy =
        static_cast<double>(correct) /
        static_cast<double>(result.report.held_out_nodes);
  }
  return result;
}

} // namespace zetta::ml
