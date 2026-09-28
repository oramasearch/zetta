#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ml/backtest.hpp"
#include "zetta/ml/gbdt.hpp"

// Native time-series forecasting (#1043, M-4).
//
// Forecasting reuses the distributed squared-error GBDT, but its data contract
// and evaluation are time ordered: lag features use only values before an
// origin, every backtest fold refits at that origin, and multi-step predictions
// feed prior predictions back into later lag features. A seasonal-naive
// forecast is always measured beside the model so the result states whether the
// learned correction added value.
//
// See docs/ml/forecast.md.
namespace zetta::ml {

struct ForecastParams {
  std::vector<std::size_t> lags = {1, 7, 28};
  std::size_t rolling_window = 7;
  std::size_t backtest_folds = 4;
  double horizon = 7.0; // integer number of future observations
  std::size_t seasonal_period = 7;
  double interval_level = 0.8;
  double maximum_horizon_fraction = 0.25;
  std::uint64_t seed = 0;
  GbdtParams gbdt;

  nlohmann::json to_json() const;
};

struct ForecastSeries {
  std::int64_t key = 0;
  std::vector<double> times;
  std::vector<double> values;
};

struct ForecastPoint {
  std::int64_t series = 0;
  double timestamp = 0.0;
  double point = 0.0;
  double lower = 0.0;
  double upper = 0.0;
};

struct ForecastFoldMetrics {
  std::size_t fold = 0;
  std::uint64_t rows = 0;
  double mae = 0.0;
  double mase = 0.0;
  double smape = 0.0;
  double naive_mae = 0.0;
  double forecast_value_added = 0.0;
  double interval_coverage = 0.0;
};

struct ForecastMetrics {
  std::vector<ForecastFoldMetrics> folds;
  ForecastFoldMetrics mean;
  double interval_level = 0.0;
  double interval_half_width = 0.0;
  bool beats_seasonal_naive = false;
  std::string comparison;
};

struct ForecastResult {
  GbdtModel model;
  BacktestResult backtest; // compatibility view: fold MAE and mean MAE
  ForecastMetrics metrics;
  std::vector<ForecastPoint> forecast;
  std::vector<std::string> feature_names;
  compute::ComputeStatus status;

  bool ok() const noexcept { return status.ok(); }
};

ForecastResult train_forecast(const std::vector<double> &times,
                              const std::vector<double> &values,
                              const ForecastParams &params = {});

// Train one global lag model over this member's complete series. Each series
// key must be owned by exactly one member; the table runner verifies that
// invariant. The injected allreduce combines GBDT histograms, fold metric
// partials, and the residual-quantile histogram. Identity reduction gives the
// single-member path.
ForecastResult
train_forecast_partitions(const std::vector<ForecastSeries> &local_series,
                          const ForecastParams &params,
                          const AllReduce &allreduce,
                          const std::string &tag = "forecast");

} // namespace zetta::ml
