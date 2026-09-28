#include "zetta/ml/graph_job.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {
namespace {

using EdgeKey = std::pair<std::int64_t, std::int64_t>;
using Outcome = compute::JobCoordinator::Outcome;

void encode_integer(std::vector<double> &values, std::int64_t value) {
  const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
  values.push_back(static_cast<double>(bits >> 32U));
  values.push_back(static_cast<double>(bits & 0xffffffffULL));
}

std::optional<std::int64_t> decode_integer(const std::vector<double> &values,
                                           std::size_t offset) {
  if (offset + 1 >= values.size()) {
    return std::nullopt;
  }
  const auto high = static_cast<std::uint64_t>(values[offset]);
  const auto low = static_cast<std::uint64_t>(values[offset + 1]);
  if (high > 0xffffffffULL || low > 0xffffffffULL) {
    return std::nullopt;
  }
  return std::bit_cast<std::int64_t>((high << 32U) | low);
}

std::vector<double> encode_keys(const std::set<EdgeKey> &keys) {
  std::vector<double> encoded;
  encoded.reserve(keys.size() * 4);
  for (const auto &[source, destination] : keys) {
    encode_integer(encoded, source);
    encode_integer(encoded, destination);
  }
  return encoded;
}

std::optional<std::set<EdgeKey>>
decode_keys(const std::vector<double> &encoded) {
  if (encoded.size() % 4 != 0) {
    return std::nullopt;
  }
  std::set<EdgeKey> keys;
  for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
    const std::optional<std::int64_t> source = decode_integer(encoded, offset);
    const std::optional<std::int64_t> destination =
        decode_integer(encoded, offset + 2);
    if (!source || !destination) {
      return std::nullopt;
    }
    keys.emplace(*source, *destination);
  }
  return keys;
}

std::string validate_local_edges(const std::vector<GraphEdge> &edges,
                                 const GraphParams &params) {
  if (!std::isfinite(params.prediction_time)) {
    return "graph.prediction_time: must be finite";
  }
  for (const GraphEdge &edge : edges) {
    if (!std::isfinite(edge.time)) {
      return "graph.edge.time: must be finite";
    }
    if (!std::isfinite(edge.weight) || edge.weight < 0.0) {
      return "graph.edge.weight: must be finite and non-negative";
    }
    if (edge.observations == 0) {
      return "graph.edge.observations: must be greater than zero";
    }
    if (edge.source_label && *edge.source_label != 0 &&
        *edge.source_label != 1) {
      return "graph.edge.source_label: must be zero or one";
    }
  }
  return {};
}

std::vector<double>
distributed_page_rank(compute::JobRound &round,
                      const std::vector<GraphEdge> &local_edges,
                      const std::vector<std::int64_t> &nodes,
                      const std::map<std::int64_t, std::size_t> &index_by_node,
                      const std::vector<double> &global_outgoing,
                      const GraphParams &params, const std::string &tag,
                      std::chrono::milliseconds timeout, bool &collective_ok) {
  const std::size_t count = nodes.size();
  std::vector<double> rank(count, 1.0 / static_cast<double>(count));
  const std::vector<std::string> participants = round.participants();
  const auto participant =
      std::find(participants.begin(), participants.end(), round.node_id());
  const std::size_t participant_index =
      static_cast<std::size_t>(participant - participants.begin());

  for (std::size_t iteration = 0; iteration < params.page_rank_iterations;
       ++iteration) {
    std::vector<double> local(count + 1, 0.0);
    for (const GraphEdge &edge : local_edges) {
      if (edge.time > params.prediction_time) {
        continue;
      }
      const std::size_t source = index_by_node.at(edge.source);
      const std::size_t destination = index_by_node.at(edge.destination);
      if (global_outgoing[source] > 0.0) {
        local[destination] +=
            rank[source] * edge.weight / global_outgoing[source];
      }
    }
    for (std::size_t index = participant_index; index < count;
         index += participants.size()) {
      if (global_outgoing[index] == 0.0) {
        local[count] += rank[index];
      }
    }
    const std::optional<std::vector<double>> reduced =
        round.allreduce(tag + ".pagerank." + std::to_string(iteration), local,
                        cluster::CollectiveOp::Sum, timeout);
    if (!reduced || reduced->size() != count + 1) {
      collective_ok = false;
      return {};
    }
    const double base =
        (1.0 - params.page_rank_damping) / static_cast<double>(count) +
        params.page_rank_damping * reduced->back() / static_cast<double>(count);
    for (std::size_t index = 0; index < count; ++index) {
      rank[index] = base + params.page_rank_damping * (*reduced)[index];
    }
  }
  return rank;
}

void replace_page_rank(GraphResult &graph,
                       const std::vector<std::int64_t> &nodes,
                       const std::vector<double> &rank) {
  for (std::size_t index = 0; index < graph.features.size(); ++index) {
    graph.features[index].page_rank = rank[index];
  }
  std::vector<std::size_t> order(nodes.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    order[index] = index;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t left, std::size_t right) {
                     if (rank[left] != rank[right]) {
                       return rank[left] > rank[right];
                     }
                     return nodes[left] < nodes[right];
                   });
  order.resize(std::min<std::size_t>(10, order.size()));
  graph.report.page_rank_top_nodes.clear();
  for (std::size_t index : order) {
    graph.report.page_rank_top_nodes.push_back(
        {.node = nodes[index], .page_rank = rank[index]});
  }
}

} // namespace

DistributedGraphResult run_graph_job(compute::JobCoordinator &coordinator,
                                     compute::ComputeJob &job,
                                     const std::vector<GraphEdge> &local_edges,
                                     const GraphParams &params,
                                     const std::string &round_tag,
                                     std::chrono::milliseconds timeout) {
  DistributedGraphResult output;
  const std::string tag = round_tag.empty() ? "ml.graph" : round_tag;

  output
      .status = coordinator.run(job, [&](compute::JobRound &round) -> Outcome {
    output.is_root = round.is_root();
    const std::string local_error = validate_local_edges(local_edges, params);
    const std::optional<std::vector<double>> validation =
        round.allreduce(tag + ".validation", {local_error.empty() ? 0.0 : 1.0},
                        cluster::CollectiveOp::Sum, timeout);
    if (!validation || validation->size() != 1) {
      return {.ok = false, .detail = "graph: validation allreduce failed"};
    }
    if ((*validation)[0] != 0.0) {
      return {.ok = false,
              .detail = local_error.empty()
                            ? "graph: another partition failed validation"
                            : local_error};
    }

    std::set<EdgeKey> local_keys;
    std::size_t local_excluded = 0;
    for (const GraphEdge &edge : local_edges) {
      if (edge.time > params.prediction_time) {
        local_excluded += edge.observations;
      } else {
        local_keys.emplace(edge.source, edge.destination);
      }
    }
    const std::optional<std::vector<std::vector<double>>> gathered =
        round.gather(tag + ".edge_keys.gather", encode_keys(local_keys),
                     timeout);
    if (!gathered) {
      return {.ok = false, .detail = "graph: edge-key gather failed"};
    }
    std::set<EdgeKey> global_keys;
    if (round.is_root()) {
      for (const std::vector<double> &encoded : *gathered) {
        const std::optional<std::set<EdgeKey>> decoded = decode_keys(encoded);
        if (!decoded) {
          return {.ok = false, .detail = "graph: invalid gathered edge keys"};
        }
        global_keys.insert(decoded->begin(), decoded->end());
      }
    }
    const std::optional<std::vector<double>> broadcast = round.broadcast(
        tag + ".edge_keys.broadcast", encode_keys(global_keys), timeout);
    if (!broadcast) {
      return {.ok = false, .detail = "graph: edge-key broadcast failed"};
    }
    const std::optional<std::set<EdgeKey>> decoded_keys =
        decode_keys(*broadcast);
    if (!decoded_keys || decoded_keys->empty()) {
      return {.ok = false,
              .detail =
                  "graph: no edges exist at or before the prediction cutoff"};
    }
    const std::vector<EdgeKey> keys(decoded_keys->begin(), decoded_keys->end());
    std::map<EdgeKey, std::size_t> index_by_key;
    std::set<std::int64_t> node_set;
    for (std::size_t index = 0; index < keys.size(); ++index) {
      index_by_key[keys[index]] = index;
      node_set.insert(keys[index].first);
      node_set.insert(keys[index].second);
    }
    const std::vector<std::int64_t> nodes(node_set.begin(), node_set.end());
    std::map<std::int64_t, std::size_t> index_by_node;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      index_by_node[nodes[index]] = index;
    }

    const std::size_t edge_count = keys.size();
    const std::size_t node_count = nodes.size();
    std::vector<double> partial(edge_count * 2 + node_count * 2 + 2, 0.0);
    for (const GraphEdge &edge : local_edges) {
      if (edge.time > params.prediction_time) {
        continue;
      }
      const std::size_t edge_index =
          index_by_key.at({edge.source, edge.destination});
      partial[edge_index] += static_cast<double>(edge.observations);
      partial[edge_count + edge_index] += edge.weight;
      if (edge.source_label) {
        const std::size_t label_index = index_by_node.at(edge.source);
        const std::size_t offset = edge_count * 2 + label_index * 2 +
                                   static_cast<std::size_t>(*edge.source_label);
        partial[offset] += static_cast<double>(edge.observations);
      }
      partial.back() += static_cast<double>(edge.observations);
    }
    partial[partial.size() - 2] = static_cast<double>(local_excluded);
    const std::optional<std::vector<double>> totals = round.allreduce(
        tag + ".edge_statistics", partial, cluster::CollectiveOp::Sum, timeout);
    if (!totals || totals->size() != partial.size()) {
      return {.ok = false, .detail = "graph: edge-statistics allreduce failed"};
    }

    std::map<std::int64_t, int> labels;
    for (std::size_t index = 0; index < node_count; ++index) {
      const std::size_t offset = edge_count * 2 + index * 2;
      if ((*totals)[offset] > 0.0 && (*totals)[offset + 1] > 0.0) {
        return {.ok = false,
                .detail = "graph: node " + std::to_string(nodes[index]) +
                          " has inconsistent labels"};
      }
      if ((*totals)[offset] > 0.0 || (*totals)[offset + 1] > 0.0) {
        labels[nodes[index]] = (*totals)[offset + 1] > 0.0 ? 1 : 0;
      }
    }

    std::vector<GraphEdge> global_edges;
    global_edges.reserve(edge_count);
    for (std::size_t index = 0; index < edge_count; ++index) {
      const std::size_t observations =
          static_cast<std::size_t>(std::llround((*totals)[index]));
      GraphEdge edge{.source = keys[index].first,
                     .destination = keys[index].second,
                     .weight = (*totals)[edge_count + index],
                     .time = params.prediction_time,
                     .observations = observations};
      const auto label = labels.find(edge.source);
      if (label != labels.end()) {
        edge.source_label = label->second;
      }
      global_edges.push_back(std::move(edge));
    }
    output.graph = compute_graph_features({global_edges}, params);
    if (!output.graph.ok()) {
      return {.ok = false, .detail = output.graph.error};
    }
    output.graph.report.edges_excluded =
        static_cast<std::size_t>(std::llround((*totals)[totals->size() - 2]));
    output.graph.report.edges_read =
        static_cast<std::size_t>(std::llround(totals->back()));

    std::vector<double> outgoing(node_count, 0.0);
    for (std::size_t index = 0; index < edge_count; ++index) {
      outgoing[index_by_node.at(keys[index].first)] +=
          (*totals)[edge_count + index];
    }
    bool collective_ok = true;
    const std::vector<double> rank =
        distributed_page_rank(round, local_edges, nodes, index_by_node,
                              outgoing, params, tag, timeout, collective_ok);
    if (!collective_ok) {
      return {.ok = false, .detail = "graph: PageRank allreduce failed"};
    }
    replace_page_rank(output.graph, nodes, rank);
    return {.ok = true,
            .detail = "graph features=" +
                      std::to_string(output.graph.features.size())};
  });
  return output;
}

} // namespace zetta::ml
