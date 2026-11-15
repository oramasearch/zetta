// SPDX-License-Identifier: MIT

#include "zetta/ml/forecast.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <ranges>
#include <utility>

namespace zetta::ml {
namespace {

constexpr std::size_t kResidualBins = 512;
constexpr double kMetricEpsilon = 1e-12;

struct TrainingRows {
  std::vector<std::vector<double>> columns;
  std::vector<double> labels;
};

struct FoldPartial {
  std::vector<double> residuals;
  double absolute_error = 0.0;
  double scaled_absolute_error = 0.0;
  double smape = 0.0;
  double naive_absolute_error = 0.0;
  std::uint64_t rows = 0;
};

struct EvaluatedFold {
  std::size_t index = 0;
  FoldPartial partial;
};

std::size_t horizon_steps(const ForecastParams &params) {
  if (!std::isfinite(params.horizon) || params.horizon < 1.0)
    return 0;
  const double rounded = std::round(params.horizon);
  if (std::abs(params.horizon - rounded) > kMetricEpsilon)
    return 0;
  return static_cast<std::size_t>(rounded);
}

std::vector<std::string> make_feature_names(const ForecastParams &params) {
  std::vector<std::string> names;
  names.reserve(params.lags.size() + (params.rolling_window > 0 ? 1 : 0));
  for (const std::size_t lag : params.lags) {
    names.push_back("lag_" + std::to_string(lag));
  }
  if (params.rolling_window > 0) {
    names.push_back("rolling_mean_" + std::to_string(params.rolling_window));
  }
  return names;
}

std::size_t history_needed(const ForecastParams &params) {
  std::size_t needed = params.seasonal_period;
  needed = std::max(needed, params.rolling_window);
  for (const std::size_t lag : params.lags)
    needed = std::max(needed, lag);
  return needed;
}

bool validate_params(const ForecastParams &params, std::string &error) {
  if (horizon_steps(params) == 0) {
    error = "train_forecast: horizon must be a positive integer number of "
            "observations";
    return false;
  }
  if (params.backtest_folds == 0) {
    error = "train_forecast: backtest_folds must be positive";
    return false;
  }
  if (params.seasonal_period == 0) {
    error = "train_forecast: seasonal_period must be positive";
    return false;
  }
  if (!(params.interval_level > 0.0 && params.interval_level < 1.0)) {
    error = "train_forecast: interval_level must be between zero and one";
    return false;
  }
  if (!(params.maximum_horizon_fraction > 0.0 &&
        params.maximum_horizon_fraction <= 1.0)) {
    error = "train_forecast: maximum_horizon_fraction must be in (0, 1]";
    return false;
  }
  if (params.lags.empty() && params.rolling_window == 0) {
    error = "train_forecast: no lag or rolling features requested";
    return false;
  }
  if (std::ranges::any_of(params.lags,
                          [](const std::size_t lag) { return lag == 0; })) {
    error = "train_forecast: lag zero would expose the target as a feature";
    return false;
  }
  return true;
}

bool normalize_series(ForecastSeries &series, std::string &error) {
  if (series.times.size() != series.values.size() || series.times.empty()) {
    error = "train_forecast: each series needs equally sized, non-empty times "
            "and values";
    return false;
  }
  std::vector<std::size_t> order(series.times.size());
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(),
                   [&](const std::size_t left, const std::size_t right) {
                     return series.times[left] < series.times[right];
                   });
  std::vector<double> times;
  std::vector<double> values;
  times.reserve(order.size());
  values.reserve(order.size());
  for (const std::size_t row : order) {
    if (!std::isfinite(series.times[row]) ||
        !std::isfinite(series.values[row])) {
      error = "train_forecast: timestamps and target values must be finite";
      return false;
    }
    if (!times.empty() && series.times[row] <= times.back()) {
      error = "train_forecast: timestamps must be unique within each series";
      return false;
    }
    times.push_back(series.times[row]);
    values.push_back(series.values[row]);
  }
  series.times = std::move(times);
  series.values = std::move(values);
  return true;
}

double next_step(const ForecastSeries &series) {
  if (series.times.size() < 2)
    return 1.0;
  std::vector<double> differences;
  differences.reserve(series.times.size() - 1);
  for (std::size_t index = 1; index < series.times.size(); ++index) {
    differences.push_back(series.times[index] - series.times[index - 1]);
  }
  const auto middle =
      differences.begin() + static_cast<std::ptrdiff_t>(differences.size() / 2);
  std::nth_element(differences.begin(), middle, differences.end());
  return *middle;
}

std::vector<double> features_for_history(const std::vector<double> &history,
                                         const ForecastParams &params) {
  std::vector<double> row;
  row.reserve(params.lags.size() + (params.rolling_window > 0 ? 1 : 0));
  for (const std::size_t lag : params.lags) {
    row.push_back(history[history.size() - lag]);
  }
  if (params.rolling_window > 0) {
    const auto begin =
        history.end() - static_cast<std::ptrdiff_t>(params.rolling_window);
    row.push_back(std::accumulate(begin, history.end(), 0.0) /
                  static_cast<double>(params.rolling_window));
  }
  return row;
}

TrainingRows make_training_rows(const std::vector<ForecastSeries> &series,
                                const ForecastParams &params,
                                const std::vector<std::size_t> &limits) {
  const std::size_t columns_count =
      params.lags.size() + (params.rolling_window > 0 ? 1 : 0);
  TrainingRows rows;
  rows.columns.resize(columns_count);
  const std::size_t warmup = history_needed(params);
  for (std::size_t series_index = 0; series_index < series.size();
       ++series_index) {
    const ForecastSeries &one = series[series_index];
    const std::size_t limit = std::min(limits[series_index], one.values.size());
    for (std::size_t position = warmup; position < limit; ++position) {
      std::vector<double> history(one.values.begin(),
                                  one.values.begin() +
                                      static_cast<std::ptrdiff_t>(position));
      const std::vector<double> features =
          features_for_history(history, params);
      for (std::size_t column = 0; column < features.size(); ++column) {
        rows.columns[column].push_back(features[column]);
      }
      const double naive = history[history.size() - params.seasonal_period];
      rows.labels.push_back(one.values[position] - naive);
    }
  }
  return rows;
}

double mase_scale(const ForecastSeries &series, const std::size_t limit,
                  const std::size_t seasonal_period) {
  double total = 0.0;
  std::size_t count = 0;
  for (std::size_t position = seasonal_period; position < limit; ++position) {
    total += std::abs(series.values[position] -
                      series.values[position - seasonal_period]);
    ++count;
  }
  return count == 0 ? 0.0 : total / static_cast<double>(count);
}

std::pair<std::vector<double>, std::vector<double>>
recursive_forecast(const ForecastSeries &series, const std::size_t origin,
                   const std::size_t steps, const ForecastParams &params,
                   const GbdtModel &model) {
  std::vector<double> model_history(series.values.begin(),
                                    series.values.begin() +
                                        static_cast<std::ptrdiff_t>(origin));
  std::vector<double> naive_history = model_history;
  std::vector<double> predictions;
  std::vector<double> naive_predictions;
  predictions.reserve(steps);
  naive_predictions.reserve(steps);
  for (std::size_t step = 0; step < steps; ++step) {
    const std::vector<double> features =
        features_for_history(model_history, params);
    const double seasonal_naive =
        model_history[model_history.size() - params.seasonal_period];
    const double prediction = seasonal_naive + model.predict_proba(features);
    predictions.push_back(prediction);
    model_history.push_back(prediction);

    const double naive =
        naive_history[naive_history.size() - params.seasonal_period];
    naive_predictions.push_back(naive);
    naive_history.push_back(naive);
  }
  return {std::move(predictions), std::move(naive_predictions)};
}

std::vector<double> reduce_or_empty(const AllReduce &allreduce,
                                    const std::string &tag,
                                    const std::vector<double> &local,
                                    const GbdtReduce operation) {
  if (!allreduce)
    return local;
  return allreduce(tag, local, operation);
}

double residual_quantile(const std::vector<double> &residuals,
                         const double level, const AllReduce &allreduce,
                         const std::string &tag, std::string &error) {
  double local_maximum = 0.0;
  for (const double residual : residuals) {
    local_maximum = std::max(local_maximum, std::abs(residual));
  }
  const std::vector<double> maximum = reduce_or_empty(
      allreduce, tag + ":maximum", {local_maximum}, GbdtReduce::Max);
  if (maximum.size() != 1) {
    error = "train_forecast: residual maximum allreduce failed";
    return 0.0;
  }
  const double global_maximum = maximum.front();
  std::vector<double> histogram(kResidualBins + 1, 0.0);
  for (const double residual : residuals) {
    const double magnitude = std::abs(residual);
    const std::size_t bin =
        global_maximum <= 0.0
            ? 0
            : std::min(
                  kResidualBins - 1,
                  static_cast<std::size_t>(magnitude / global_maximum *
                                           static_cast<double>(kResidualBins)));
    histogram[bin] += 1.0;
    histogram.back() += 1.0;
  }
  const std::vector<double> global = reduce_or_empty(
      allreduce, tag + ":histogram", histogram, GbdtReduce::Sum);
  if (global.size() != histogram.size() || global.back() <= 0.0) {
    error = "train_forecast: residual histogram allreduce failed";
    return 0.0;
  }
  const double wanted = std::ceil(level * global.back());
  double cumulative = 0.0;
  for (std::size_t bin = 0; bin < kResidualBins; ++bin) {
    cumulative += global[bin];
    if (cumulative >= wanted) {
      return global_maximum * static_cast<double>(bin + 1) /
             static_cast<double>(kResidualBins);
    }
  }
  return global_maximum;
}

ForecastFoldMetrics fold_metrics(const FoldPartial &local,
                                 const AllReduce &allreduce,
                                 const std::string &tag, std::string &error) {
  const std::vector<double> partial = {
      local.absolute_error,       local.scaled_absolute_error,     local.smape,
      local.naive_absolute_error, static_cast<double>(local.rows),
  };
  const std::vector<double> global =
      reduce_or_empty(allreduce, tag, partial, GbdtReduce::Sum);
  ForecastFoldMetrics metrics;
  if (global.size() != partial.size() || global[4] <= 0.0) {
    error = "train_forecast: fold metric allreduce failed";
    return metrics;
  }
  const double rows = global[4];
  metrics.rows = static_cast<std::uint64_t>(rows);
  metrics.mae = global[0] / rows;
  metrics.mase = global[1] / rows;
  metrics.smape = global[2] / rows;
  metrics.naive_mae = global[3] / rows;
  metrics.forecast_value_added =
      metrics.naive_mae <= kMetricEpsilon
          ? (metrics.mae <= kMetricEpsilon ? 0.0 : -1.0)
          : 1.0 - metrics.mae / metrics.naive_mae;
  return metrics;
}

ForecastFoldMetrics
mean_metrics(const std::vector<ForecastFoldMetrics> &folds) {
  ForecastFoldMetrics mean;
  if (folds.empty())
    return mean;
  for (const ForecastFoldMetrics &fold : folds) {
    mean.rows += fold.rows;
    mean.mae += fold.mae;
    mean.mase += fold.mase;
    mean.smape += fold.smape;
    mean.naive_mae += fold.naive_mae;
    mean.forecast_value_added += fold.forecast_value_added;
    mean.interval_coverage += fold.interval_coverage;
  }
  const double count = static_cast<double>(folds.size());
  mean.mae /= count;
  mean.mase /= count;
  mean.smape /= count;
  mean.naive_mae /= count;
  mean.forecast_value_added /= count;
  mean.interval_coverage /= count;
  return mean;
}

bool normalize_local_series(std::vector<ForecastSeries> &series,
                            const ForecastParams &params, std::string &error) {
  for (ForecastSeries &one : series) {
    if (!normalize_series(one, error))
      return false;
    const std::size_t maximum_horizon = static_cast<std::size_t>(
        std::floor(static_cast<double>(one.values.size()) *
                   params.maximum_horizon_fraction));
    if (horizon_steps(params) > maximum_horizon) {
      error = "train_forecast: horizon exceeds maximum_horizon_fraction of "
              "series history";
      return false;
    }
  }
  return true;
}

std::vector<std::size_t> fold_origins(const std::vector<ForecastSeries> &series,
                                      const ForecastParams &params,
                                      const std::size_t fold_index) {
  const std::size_t offset =
      horizon_steps(params) * (params.backtest_folds - fold_index);
  std::vector<std::size_t> origins(series.size(), 0);
  for (std::size_t index = 0; index < series.size(); ++index) {
    if (series[index].values.size() > offset)
      origins[index] = series[index].values.size() - offset;
  }
  return origins;
}

FoldPartial evaluate_local_fold(const std::vector<ForecastSeries> &series,
                                const std::vector<std::size_t> &origins,
                                const ForecastParams &params,
                                const GbdtModel &model) {
  FoldPartial partial;
  const std::size_t steps = horizon_steps(params);
  for (std::size_t index = 0; index < series.size(); ++index) {
    const ForecastSeries &one = series[index];
    const std::size_t origin = origins[index];
    if (origin < history_needed(params) || origin + steps > one.values.size())
      continue;
    const auto [predictions, naive_predictions] =
        recursive_forecast(one, origin, steps, params, model);
    const double scale = mase_scale(one, origin, params.seasonal_period);
    for (std::size_t step = 0; step < steps; ++step) {
      const double actual = one.values[origin + step];
      const double residual = actual - predictions[step];
      const double absolute = std::abs(residual);
      partial.residuals.push_back(residual);
      partial.absolute_error += absolute;
      partial.scaled_absolute_error +=
          scale <= kMetricEpsilon
              ? (absolute <= kMetricEpsilon ? 0.0 : absolute / kMetricEpsilon)
              : absolute / scale;
      const double denominator = std::abs(actual) + std::abs(predictions[step]);
      if (denominator > kMetricEpsilon)
        partial.smape += 200.0 * absolute / denominator;
      partial.naive_absolute_error +=
          std::abs(actual - naive_predictions[step]);
      ++partial.rows;
    }
  }
  return partial;
}

bool append_backtest_fold(const std::vector<ForecastSeries> &series,
                          const ForecastParams &params,
                          const std::vector<std::string> &feature_names,
                          const AllReduce &allreduce, const std::string &tag,
                          const std::size_t fold_index, ForecastResult &result,
                          std::vector<EvaluatedFold> &evaluated) {
  const std::vector<std::size_t> origins =
      fold_origins(series, params, fold_index);
  TrainingRows rows = make_training_rows(series, params, origins);
  const std::string fold_tag = tag + ":fold:" + std::to_string(fold_index);
  const std::vector<double> counts = reduce_or_empty(
      allreduce, fold_tag + ":counts",
      {static_cast<double>(rows.labels.size())}, GbdtReduce::Sum);
  if (counts.size() != 1) {
    result.status = compute::ComputeStatus::backend_error(
        "train_forecast: fold row-count allreduce failed");
    return false;
  }
  if (counts.front() == 0.0)
    return true;

  GbdtParams gbdt = params.gbdt;
  gbdt.objective = "squared_error";
  gbdt.holdout_fraction = 0.0;
  const GbdtTrainResult trained = train_gbdt(
      rows.columns, feature_names, rows.labels, gbdt, allreduce, fold_tag);
  if (!trained.ok()) {
    result.status = trained.status;
    return false;
  }
  FoldPartial partial =
      evaluate_local_fold(series, origins, params, trained.model);
  std::string error;
  ForecastFoldMetrics metrics =
      fold_metrics(partial, allreduce, fold_tag + ":metrics", error);
  if (!error.empty()) {
    result.status = compute::ComputeStatus::backend_error(std::move(error));
    return false;
  }
  metrics.fold = fold_index;
  result.metrics.folds.push_back(metrics);
  evaluated.push_back(
      EvaluatedFold{.index = fold_index, .partial = std::move(partial)});
  return true;
}

bool finish_backtest(const ForecastParams &params, const AllReduce &allreduce,
                     const std::string &tag,
                     const std::vector<EvaluatedFold> &evaluated,
                     ForecastResult &result) {
  std::vector<double> residuals;
  for (const EvaluatedFold &fold : evaluated) {
    residuals.insert(residuals.end(), fold.partial.residuals.begin(),
                     fold.partial.residuals.end());
  }
  std::string error;
  result.metrics.interval_level = params.interval_level;
  result.metrics.interval_half_width = residual_quantile(
      residuals, params.interval_level, allreduce, tag + ":interval", error);
  if (!error.empty()) {
    result.status = compute::ComputeStatus::backend_error(std::move(error));
    return false;
  }
  for (std::size_t index = 0; index < evaluated.size(); ++index) {
    std::uint64_t covered = 0;
    for (const double residual : evaluated[index].partial.residuals) {
      if (std::abs(residual) <= result.metrics.interval_half_width)
        ++covered;
    }
    const std::vector<double> coverage = reduce_or_empty(
        allreduce,
        tag + ":fold:" + std::to_string(evaluated[index].index) + ":coverage",
        {static_cast<double>(covered),
         static_cast<double>(evaluated[index].partial.residuals.size())},
        GbdtReduce::Sum);
    if (coverage.size() != 2 || coverage[1] <= 0.0) {
      result.status = compute::ComputeStatus::backend_error(
          "train_forecast: interval coverage allreduce failed");
      return false;
    }
    result.metrics.folds[index].interval_coverage = coverage[0] / coverage[1];
  }
  result.metrics.mean = mean_metrics(result.metrics.folds);
  result.metrics.beats_seasonal_naive =
      result.metrics.mean.forecast_value_added > 0.0;
  result.metrics.comparison = result.metrics.beats_seasonal_naive
                                  ? "forecast beat seasonal-naive"
                                  : "forecast did not beat seasonal-naive";
  return true;
}

bool train_final_model(const std::vector<ForecastSeries> &series,
                       const ForecastParams &params,
                       const std::vector<std::string> &feature_names,
                       const AllReduce &allreduce, const std::string &tag,
                       ForecastResult &result) {
  std::vector<std::size_t> limits;
  limits.reserve(series.size());
  for (const ForecastSeries &one : series)
    limits.push_back(one.values.size());
  TrainingRows rows = make_training_rows(series, params, limits);
  GbdtParams gbdt = params.gbdt;
  gbdt.objective = "squared_error";
  gbdt.holdout_fraction = 0.0;
  const GbdtTrainResult trained =
      train_gbdt(rows.columns, feature_names, rows.labels, gbdt, allreduce,
                 tag + ":final");
  if (!trained.ok()) {
    result.status = trained.status;
    return false;
  }
  result.model = trained.model;
  result.model.calibration.reset();
  return true;
}

void append_forecasts(const std::vector<ForecastSeries> &series,
                      const ForecastParams &params, ForecastResult &result) {
  const std::size_t steps = horizon_steps(params);
  for (const ForecastSeries &one : series) {
    const auto [predictions, ignored_naive] =
        recursive_forecast(one, one.values.size(), steps, params, result.model);
    (void)ignored_naive;
    const double cadence = next_step(one);
    for (std::size_t step = 0; step < steps; ++step) {
      const double point = predictions[step];
      result.forecast.push_back(ForecastPoint{
          .series = one.key,
          .timestamp =
              one.times.back() + cadence * static_cast<double>(step + 1),
          .point = point,
          .lower = point - result.metrics.interval_half_width,
          .upper = point + result.metrics.interval_half_width,
      });
    }
  }
}

void populate_legacy_backtest(ForecastResult &result) {
  result.backtest.folds_evaluated = result.metrics.folds.size();
  result.backtest.mean_error = result.metrics.mean.mae;
  for (const ForecastFoldMetrics &fold : result.metrics.folds)
    result.backtest.fold_errors.push_back(fold.mae);
}

} // namespace

nlohmann::json ForecastParams::to_json() const {
  return nlohmann::json{{"lags", lags},
                        {"rolling_window", rolling_window},
                        {"backtest_folds", backtest_folds},
                        {"horizon", horizon},
                        {"seasonal_period", seasonal_period},
                        {"interval_level", interval_level},
                        {"maximum_horizon_fraction", maximum_horizon_fraction},
                        {"seed", seed},
                        {"gbdt", gbdt.to_json()}};
}

ForecastResult
train_forecast_partitions(const std::vector<ForecastSeries> &local_input,
                          const ForecastParams &params,
                          const AllReduce &allreduce, const std::string &tag) {
  ForecastResult result;
  std::string error;
  if (!validate_params(params, error)) {
    result.status = compute::ComputeStatus::invalid_argument(std::move(error));
    return result;
  }
  std::vector<ForecastSeries> local_series = local_input;
  const bool local_valid = normalize_local_series(local_series, params, error);
  const std::vector<double> preflight =
      reduce_or_empty(allreduce, tag + ":preflight", {local_valid ? 1.0 : 0.0},
                      GbdtReduce::Min);
  if (preflight.size() != 1) {
    result.status = compute::ComputeStatus::backend_error(
        "train_forecast: preflight allreduce failed");
    return result;
  }
  if (preflight.front() != 1.0) {
    result.status = compute::ComputeStatus::invalid_argument(
        error.empty()
            ? "train_forecast: another member rejected its local series"
            : std::move(error));
    return result;
  }
  result.feature_names = make_feature_names(params);
  std::vector<EvaluatedFold> evaluated;
  for (std::size_t fold_index = 0; fold_index < params.backtest_folds;
       ++fold_index) {
    if (!append_backtest_fold(local_series, params, result.feature_names,
                              allreduce, tag, fold_index, result, evaluated))
      return result;
  }
  if (result.metrics.folds.empty()) {
    result.status = compute::ComputeStatus::invalid_argument(
        "train_forecast: no temporal backtest fold could be evaluated");
    return result;
  }

  if (!finish_backtest(params, allreduce, tag, evaluated, result))
    return result;
  if (!train_final_model(local_series, params, result.feature_names, allreduce,
                         tag, result))
    return result;
  append_forecasts(local_series, params, result);
  populate_legacy_backtest(result);
  result.status = compute::ComputeStatus::success();
  return result;
}

ForecastResult train_forecast(const std::vector<double> &times,
                              const std::vector<double> &values,
                              const ForecastParams &params) {
  const AllReduce identity = [](const std::string &,
                                const std::vector<double> &local,
                                const GbdtReduce) { return local; };
  return train_forecast_partitions(
      {ForecastSeries{.key = 0, .times = times, .values = values}}, params,
      identity, "forecast:single");
}

} // namespace zetta::ml
