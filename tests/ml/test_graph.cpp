// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "zetta/ml/graph.hpp"

namespace {

using zetta::ml::GraphEdge;
using zetta::ml::GraphNodeFeature;
using zetta::ml::GraphParams;
using zetta::ml::GraphResult;

const GraphNodeFeature *feature(const GraphResult &result, std::int64_t node) {
  const auto found =
      std::find_if(result.features.begin(), result.features.end(),
                   [node](const GraphNodeFeature &candidate) {
                     return candidate.node == node;
                   });
  return found == result.features.end() ? nullptr : &*found;
}

GraphParams params() {
  GraphParams value;
  value.prediction_time = 10.0;
  value.embedding_dimensions = 4;
  value.walks_per_node = 3;
  value.walk_length = 8;
  value.skip_gram_epochs = 2;
  value.held_out_fraction = 0.0;
  value.seed = 42;
  return value;
}

TEST(PageRankGraph, ADirectedChainMatchesTheReferenceValues) {
  const GraphResult result = zetta::ml::compute_graph_features(
      {{{.source = 1, .destination = 2, .time = 1.0},
        {.source = 2, .destination = 3, .time = 1.0}}},
      params());

  ASSERT_TRUE(result.ok()) << result.error;
  ASSERT_NE(feature(result, 1), nullptr);
  ASSERT_NE(feature(result, 2), nullptr);
  ASSERT_NE(feature(result, 3), nullptr);
  EXPECT_NEAR(feature(result, 1)->page_rank, 0.18441678, 1e-6);
  EXPECT_NEAR(feature(result, 2)->page_rank, 0.34117105, 1e-6);
  EXPECT_NEAR(feature(result, 3)->page_rank, 0.47441217, 1e-6);
}

TEST(ComponentGraph, FindsConnectedComponentsAndLocalClustering) {
  const GraphResult result = zetta::ml::compute_graph_features(
      {{{.source = 1, .destination = 2, .time = 1.0},
        {.source = 2, .destination = 3, .time = 1.0},
        {.source = 3, .destination = 1, .time = 1.0},
        {.source = 8, .destination = 9, .time = 1.0}}},
      params());

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.report.component_count, 2u);
  EXPECT_EQ(result.report.component_sizes, (std::vector<std::size_t>{3, 2}));
  ASSERT_NE(feature(result, 1), nullptr);
  ASSERT_NE(feature(result, 8), nullptr);
  EXPECT_EQ(feature(result, 1)->component, 1);
  EXPECT_EQ(feature(result, 8)->component, 8);
  EXPECT_DOUBLE_EQ(feature(result, 1)->clustering_coefficient, 1.0);
  EXPECT_DOUBLE_EQ(feature(result, 8)->clustering_coefficient, 0.0);
}

TEST(LabelPropGraph, PropagatesFromLabelledNodes) {
  GraphParams value = params();
  value.held_out_fraction = 0.0;
  const GraphResult result = zetta::ml::compute_graph_features(
      {{{.source = 1, .destination = 2, .time = 1.0, .source_label = 1},
        {.source = 2, .destination = 3, .time = 1.0},
        {.source = 9, .destination = 8, .time = 1.0, .source_label = 0},
        {.source = 8, .destination = 7, .time = 1.0}}},
      value);

  ASSERT_TRUE(result.ok()) << result.error;
  ASSERT_NE(feature(result, 3), nullptr);
  ASSERT_NE(feature(result, 7), nullptr);
  EXPECT_EQ(feature(result, 3)->propagated_label, 1);
  EXPECT_EQ(feature(result, 7)->propagated_label, 0);
}

TEST(GraphPartitions, ThreePartitionsMatchOnePartitionExactly) {
  const std::vector<GraphEdge> all = {
      {.source = 1, .destination = 2, .weight = 2.0, .time = 1.0},
      {.source = 2, .destination = 3, .weight = 1.0, .time = 2.0},
      {.source = 3, .destination = 1, .weight = 3.0, .time = 3.0},
      {.source = 3, .destination = 4, .weight = 1.0, .time = 4.0},
      {.source = 4, .destination = 2, .weight = 2.0, .time = 5.0},
      {.source = 5, .destination = 6, .weight = 1.0, .time = 6.0},
  };
  const GraphResult one = zetta::ml::compute_graph_features({all}, params());
  const GraphResult three = zetta::ml::compute_graph_features(
      {{all[0], all[3]}, {all[2], all[5]}, {all[1], all[4]}}, params());

  ASSERT_TRUE(one.ok()) << one.error;
  ASSERT_TRUE(three.ok()) << three.error;
  EXPECT_EQ(one.features, three.features);
  EXPECT_EQ(one.report.edges_read, three.report.edges_read);
  EXPECT_EQ(one.report.component_sizes, three.report.component_sizes);
  EXPECT_EQ(one.report.page_rank_top_nodes.size(),
            three.report.page_rank_top_nodes.size());
  for (std::size_t index = 0; index < one.report.page_rank_top_nodes.size();
       ++index) {
    EXPECT_EQ(one.report.page_rank_top_nodes[index].node,
              three.report.page_rank_top_nodes[index].node);
    EXPECT_DOUBLE_EQ(one.report.page_rank_top_nodes[index].page_rank,
                     three.report.page_rank_top_nodes[index].page_rank);
  }
}

TEST(GraphCutoff, ExcludesAnEdgeAfterThePredictionTimeEverywhere) {
  GraphParams value = params();
  value.prediction_time = 5.0;
  const GraphResult result = zetta::ml::compute_graph_features(
      {{{.source = 1, .destination = 2, .time = 4.0},
        {.source = 2, .destination = 99, .time = 6.0}}},
      value);

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.report.edges_read, 1u);
  EXPECT_EQ(result.report.edges_excluded, 1u);
  EXPECT_EQ(result.report.nodes, 2u);
  EXPECT_EQ(feature(result, 99), nullptr);
}

TEST(GraphEmbedding, IsDeterministicUnderTheSeed) {
  const std::vector<std::vector<GraphEdge>> edges = {
      {{.source = 1, .destination = 2, .time = 1.0},
       {.source = 2, .destination = 3, .time = 1.0},
       {.source = 3, .destination = 1, .time = 1.0}}};
  const GraphResult first = zetta::ml::compute_graph_features(edges, params());
  const GraphResult second = zetta::ml::compute_graph_features(edges, params());

  ASSERT_TRUE(first.ok()) << first.error;
  ASSERT_TRUE(second.ok()) << second.error;
  EXPECT_EQ(first.features, second.features);
}

} // namespace
