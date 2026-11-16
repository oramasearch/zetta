// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "zetta/ml/anomaly_methodology.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/graph_checks.hpp"

namespace {

zetta::ir::ExecutionGraph
anomaly_shape(nlohmann::json hints = nlohmann::json::object()) {
  return zetta::ml::make_anomaly_methodology().build_shape(
      {"project", "detect anomalies", "S-7", hints});
}

void expect_refusal(zetta::ir::ExecutionGraph graph,
                    const std::string &reason) {
  const auto result = zetta::pipeline::AnomalyGraphCheck{}.evaluate(graph);
  EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
  EXPECT_NE(result.reason.find(reason), std::string::npos) << result.reason;
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}

} // namespace

TEST(AnomalyMethodology, OwnShapeIsRunnableAndLabelFree) {
  const auto *methodology = zetta::ml::find_methodology("S-7");
  ASSERT_NE(methodology, nullptr);
  EXPECT_TRUE(methodology->runnable);
  auto graph = anomaly_shape();
  EXPECT_TRUE(methodology->owns(graph));
  EXPECT_EQ(zetta::ml::methodology_for_graph(graph)->id, "M-7");
  EXPECT_FALSE(zetta::pipeline::default_gate().run(graph).block_deploy);
  std::string error;
  auto plan = zetta::ml::parse_anomaly_plan(graph, error);
  ASSERT_TRUE(plan) << error;
  EXPECT_TRUE(plan->contract.label_column.empty());
  EXPECT_EQ(plan->params.tree_count, 100u);
  EXPECT_EQ(graph.node_count(), 4u);
}

TEST(AnomalyGraphCheck, RefusesMissingAndAmbiguousBudgets) {
  auto graph = anomaly_shape();
  graph.find_node("anomaly_model")->attributes.erase("contamination");
  expect_refusal(graph, "exactly one");
  expect_refusal(anomaly_shape({{"alert_budget", 3}, {"contamination", 0.1}}),
                 "exactly one");
  expect_refusal(anomaly_shape({{"contamination", 0.0}}), "contamination");
  expect_refusal(anomaly_shape({{"alert_budget", -1}}), "alert_budget");
}

TEST(AnomalyGraphCheck, RefusesTrainingOnLabels) {
  expect_refusal(anomaly_shape({{"label_use", "training"}}), "evaluation_only");
  expect_refusal(anomaly_shape({{"label_column", "fraud"},
                                {"feature_columns", {"amount", "fraud"}}}),
                 "label");
}

TEST(AnomalyGraphCheck, RefusesNonRobustScaling) {
  expect_refusal(anomaly_shape({{"scaling", "mean_stddev"}}), "median_mad");
}

TEST(AnomalyGraphCheck, RefusesKpiWithoutTimeOrWithFutureFitting) {
  expect_refusal(anomaly_shape({{"mode", "kpi_time_series"}}), "time_column");
  expect_refusal(anomaly_shape({{"mode", "kpi_time_series"},
                                {"time_column", "day"},
                                {"causal", false}}),
                 "causal=true");
  EXPECT_FALSE(zetta::pipeline::default_gate()
                   .run(anomaly_shape(
                       {{"mode", "kpi_time_series"}, {"time_column", "day"}}))
                   .block_deploy);
}

TEST(AnomalyGraphCheck, RefusesMalformedParametersAndUnsupportedTransforms) {
  expect_refusal(anomaly_shape({{"tree_count", "many"}}), "tree_count");
  expect_refusal(anomaly_shape({{"feature_columns", 42}}), "feature_columns");
  expect_refusal(anomaly_shape({{"group_column", "region"}}), "grouped");
  auto graph = anomaly_shape();
  graph.find_node("anomaly_features")->attributes["operation"] = "random_split";
  expect_refusal(graph, "unsupported transform");
  auto disconnected = anomaly_shape();
  disconnected.remove_edge("anomaly_source", "anomaly_features");
  expect_refusal(disconnected, "connect");
}
