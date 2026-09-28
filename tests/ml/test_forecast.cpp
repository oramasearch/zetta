#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "zetta/ml/forecast.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<double> days(const std::size_t count) {
  std::vector<double> times(count);
  std::iota(times.begin(), times.end(), 0.0);
  return times;
}

std::vector<double> seasonal_trend(const std::size_t count,
                                   const double offset = 0.0) {
  std::vector<double> values(count);
  for (std::size_t day = 0; day < count; ++day) {
    const double noise =
        static_cast<double>(static_cast<int>(day % 5) - 2) * 0.15;
    values[day] = offset + 100.0 + 0.6 * static_cast<double>(day) +
                  20.0 * std::sin(2.0 * kPi * static_cast<double>(day) / 7.0) +
                  noise;
  }
  return values;
}

std::vector<double> random_walk(const std::size_t count) {
  std::vector<double> values(count, 100.0);
  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  for (std::size_t index = 1; index < count; ++index) {
    state ^= state << 7;
    state ^= state >> 9;
    state ^= state << 8;
    const double increment = static_cast<double>(state % 2001) / 500.0 - 2.0;
    values[index] = values[index - 1] + increment;
  }
  return values;
}

zetta::ml::ForecastParams test_params() {
  zetta::ml::ForecastParams params;
  params.gbdt.num_trees = 30;
  params.gbdt.max_depth = 3;
  params.gbdt.learning_rate = 0.15;
  return params;
}

} // namespace

TEST(Forecast, SeasonalTrendBeatsSeasonalNaiveAndReportsFva) {
  const zetta::ml::ForecastParams params = test_params();
  const zetta::ml::ForecastResult result =
      zetta::ml::train_forecast(days(224), seasonal_trend(224), params);

  ASSERT_TRUE(result.ok()) << result.status.message;
  ASSERT_EQ(result.metrics.folds.size(), params.backtest_folds);
  EXPECT_GT(result.metrics.mean.forecast_value_added, 0.0);
  EXPECT_TRUE(result.metrics.beats_seasonal_naive);
  EXPECT_EQ(result.metrics.comparison, "forecast beat seasonal-naive");
  EXPECT_EQ(result.forecast.size(), 7u);
  EXPECT_EQ(result.model.objective, "squared_error");
  EXPECT_FALSE(result.model.calibration.has_value());
}

TEST(Forecast, RandomWalkDoesNotClaimValueAdded) {
  zetta::ml::ForecastParams params = test_params();
  params.lags = {1, 2, 3};
  params.rolling_window = 3;
  params.seasonal_period = 1;
  const zetta::ml::ForecastResult result =
      zetta::ml::train_forecast(days(240), random_walk(240), params);

  ASSERT_TRUE(result.ok()) << result.status.message;
  EXPECT_LE(result.metrics.mean.forecast_value_added, 0.0);
  EXPECT_FALSE(result.metrics.beats_seasonal_naive);
  EXPECT_EQ(result.metrics.comparison, "forecast did not beat seasonal-naive");
}

TEST(Forecast, AFeatureThatNamesThePresentIsRefused) {
  zetta::ml::ForecastParams params = test_params();
  params.lags = {0, 7};

  const zetta::ml::ForecastResult result =
      zetta::ml::train_forecast(days(120), seasonal_trend(120), params);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.status.message.find("lag zero"), std::string::npos);
}

TEST(Forecast, PredictionIntervalsHaveNominalBacktestCoverage) {
  zetta::ml::ForecastParams params = test_params();
  params.backtest_folds = 8;
  params.interval_level = 0.8;
  const zetta::ml::ForecastResult result =
      zetta::ml::train_forecast(days(280), seasonal_trend(280), params);

  ASSERT_TRUE(result.ok()) << result.status.message;
  EXPECT_GE(result.metrics.mean.interval_coverage, 0.7);
  EXPECT_LE(result.metrics.mean.interval_coverage, 1.0);
  for (const zetta::ml::ForecastPoint &row : result.forecast) {
    EXPECT_LT(row.lower, row.upper);
    EXPECT_GE(row.point, row.lower);
    EXPECT_LE(row.point, row.upper);
  }
}

TEST(Forecast, MultipleSeriesProduceOneHorizonEach) {
  zetta::ml::ForecastParams params = test_params();
  const zetta::ml::AllReduce identity =
      [](const std::string &, const std::vector<double> &local,
         const zetta::ml::GbdtReduce) { return local; };
  const std::vector<zetta::ml::ForecastSeries> series = {
      {.key = 11, .times = days(224), .values = seasonal_trend(224)},
      {.key = 22, .times = days(224), .values = seasonal_trend(224, 500.0)},
  };

  const zetta::ml::ForecastResult result =
      zetta::ml::train_forecast_partitions(series, params, identity);
  ASSERT_TRUE(result.ok()) << result.status.message;
  ASSERT_EQ(result.forecast.size(), 14u);
  EXPECT_EQ(result.forecast.front().series, 11);
  EXPECT_EQ(result.forecast.back().series, 22);
}

TEST(Forecast, HorizonLongerThanTheHistoryFractionIsRefused) {
  zetta::ml::ForecastParams params = test_params();
  params.horizon = 30;
  params.maximum_horizon_fraction = 0.2;

  const zetta::ml::ForecastResult result =
      zetta::ml::train_forecast(days(100), seasonal_trend(100), params);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.status.message.find("maximum_horizon_fraction"),
            std::string::npos);
}

TEST(Forecast, RegressionModelRoundTripsAtItsBoundary) {
  const zetta::ml::ForecastParams params = test_params();
  std::string error;
  const zetta::ml::ForecastResult result =
      zetta::ml::train_forecast(days(160), seasonal_trend(160), params);
  ASSERT_TRUE(result.ok()) << result.status.message;
  const std::optional<zetta::ml::GbdtModel> model =
      zetta::ml::GbdtModel::from_json(result.model.to_json(), &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->objective, "squared_error");
}

TEST(Forecast, RegressionBaseScoreIsTheContinuousTargetMean) {
  zetta::ml::GbdtParams params;
  params.objective = "squared_error";
  params.num_trees = 1;
  const zetta::ml::GbdtTrainResult result = zetta::ml::train_gbdt(
      {{0.0, 1.0, 2.0, 3.0}}, {"lag_1"}, {10.0, 20.0, 30.0, 40.0}, params);

  ASSERT_TRUE(result.ok()) << result.status.message;
  EXPECT_DOUBLE_EQ(result.model.base_score, 25.0);
}

TEST(Forecast, MismatchedAndEmptyInputsAreRefused) {
  const zetta::ml::ForecastParams params = test_params();
  EXPECT_FALSE(
      zetta::ml::train_forecast(days(10), seasonal_trend(9), params).ok());
  EXPECT_FALSE(zetta::ml::train_forecast({}, {}, params).ok());
}
