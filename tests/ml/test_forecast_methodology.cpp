#include <gtest/gtest.h>

#include "zetta/ml/forecast_methodology.hpp"
#include "zetta/pipeline/graph_checks.hpp"

namespace {

zetta::ir::ExecutionGraph
shape(nlohmann::json hints = nlohmann::json::object()) {
  zetta::ml::ShapeRequest request;
  request.hints = std::move(hints);
  return zetta::ml::find_methodology("M-4")->build_shape(request);
}

void refuses(nlohmann::json hints) {
  const auto graph = shape(std::move(hints));
  const auto result = zetta::pipeline::ForecastGraphCheck{}.evaluate(graph);
  EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
  EXPECT_FALSE(result.reason.empty());
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}

} // namespace

TEST(ForecastMethodology, OwnPipelineIsRunnableAndNotTheChurnShape) {
  const auto *methodology = zetta::ml::find_methodology("S-5");
  ASSERT_NE(methodology, nullptr);
  EXPECT_EQ(methodology->id, "M-4");
  EXPECT_TRUE(methodology->runnable);
  const auto graph = shape({{"source", "demand"}, {"table", "daily"}});
  EXPECT_EQ(graph.node_count(), 4u);
  EXPECT_TRUE(methodology->owns(graph));
  EXPECT_FALSE(zetta::ml::find_methodology("M-1")->owns(graph));
  EXPECT_FALSE(zetta::pipeline::default_gate().run(graph).block_deploy);
  std::string error;
  const auto parsed = zetta::ml::parse_forecast_pipeline(graph, error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(parsed->database, "demand");
  EXPECT_EQ(parsed->table, "daily");
}

TEST(ForecastGraphCheck, RefusesRandomOrMissingTemporalValidation) {
  refuses({{"split", "random"}});
  refuses({{"holdout_fraction", 0.2}});
  refuses({{"temporal_validation", false}});
}

TEST(ForecastGraphCheck, RefusesFutureFeatures) {
  refuses({{"lags", {0, 7}}});
  refuses({{"lags", {-1, 7}}});
  refuses({{"feature_columns", {"tomorrow_sales"}}});
  auto graph = shape();
  graph.find_node("forecast_lags")->attributes["strictly_past"] = false;
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}

TEST(ForecastGraphCheck, RefusesProbabilityCalibration) {
  refuses({{"calibrated", true}});
  refuses({{"objective", "binary_logistic"}});
}

TEST(ForecastGraphCheck, RefusesMissingSeasonalNaiveBaseline) {
  refuses({{"baseline", "none"}});
  refuses({{"seasonal_period", 0}});
  auto graph = shape();
  graph.find_node("forecast_model")->attributes.erase("baseline");
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}

TEST(ForecastGraphCheck, RefusesExcessiveHorizonAndInvalidFraction) {
  refuses({{"horizon", 0}});
  refuses({{"horizon", 2.5}});
  refuses({{"maximum_horizon_fraction", 0}});
  refuses({{"maximum_horizon_fraction", 1.01}});
  refuses({{"horizon", 7},
           {"history_length", 20},
           {"maximum_horizon_fraction", 0.25}});
}

TEST(ForecastGraphCheck, RefusesMalformedAndOverlappingRoles) {
  refuses({{"time_column", ""}});
  refuses({{"target_column", "timestamp"}});
  refuses({{"series_column", "value"}});
  refuses({{"horizon", "seven"}});
  refuses({{"source", 42}});
  refuses({{"model_kind", "gbdt"}});
}

TEST(ForecastGraphCheck, RefusesDisconnectedOrExtraStages) {
  auto graph = shape();
  graph.remove_edge("forecast_lags", "forecast_model");
  graph.add_edge("forecast_history", "forecast_model");
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
  graph = shape();
  graph.add_node(zetta::ir::StepKind::Clean, "future values");
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}
