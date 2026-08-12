#include "zetta/ml/forecast.hpp"

#include <algorithm>
#include <cmath>

namespace zetta::ml {
namespace {

// Rows usable for training: every lag and the rolling window must have produced a real value.
// A row with a NaN feature is not "mostly fine" - imputing it would invent history, which is the
// one thing a forecast may never do.
bool row_is_complete(const std::vector<std::vector<double>>& columns, std::size_t row) {
    return std::none_of(columns.begin(), columns.end(),
                        [row](const std::vector<double>& column) {
                            return std::isnan(column[row]);
                        });
}

}  // namespace

ForecastResult train_forecast(const std::vector<double>& times,
                              const std::vector<double>& values,
                              const ForecastParams& params) {
    ForecastResult result;
    if (times.size() != values.size() || times.empty()) {
        result.status = compute::ComputeStatus::invalid_argument(
            "train_forecast: times and values must be the same non-empty length");
        return result;
    }

    // 1. Features, all strictly past. `lag_feature` refuses a lag of 0 and orders by TIME rather
    //    than arrival, and `rolling_mean_before` never includes its own row.
    std::vector<std::vector<double>> columns;
    for (const std::size_t lag : params.lags) {
        columns.push_back(lag_feature(times, values, lag));
        result.feature_names.push_back("lag_" + std::to_string(lag));
    }
    if (params.rolling_window > 0) {
        columns.push_back(rolling_mean_before(times, values, params.rolling_window));
        result.feature_names.push_back("rolling_mean_" +
                                       std::to_string(params.rolling_window));
    }
    if (columns.empty()) {
        result.status = compute::ComputeStatus::invalid_argument(
            "train_forecast: no features requested (empty lags and zero rolling window)");
        return result;
    }

    // 2. Keep only rows whose whole feature vector exists.
    std::vector<std::size_t> usable;
    for (std::size_t row = 0; row < values.size(); ++row) {
        if (!std::isnan(times[row]) && !std::isnan(values[row]) && row_is_complete(columns, row)) {
            usable.push_back(row);
        }
    }
    if (usable.size() < 4) {
        result.status = compute::ComputeStatus::invalid_argument(
            "train_forecast: too few rows with a complete feature history to forecast from");
        return result;
    }

    // 3. Backtest over rolling origins - the honest evaluation. A held-out SAMPLE would score the
    //    model on rows interleaved with its training data; a backtest scores it on the future.
    std::vector<double> usable_times;
    usable_times.reserve(usable.size());
    for (const std::size_t row : usable) usable_times.push_back(times[row]);
    const std::vector<BacktestFold> folds =
        rolling_origins(usable_times, params.backtest_folds, params.horizon);

    // 4. Fit on everything usable, with the squared-error objective - a forecast predicts a value,
    //    not a class, and predict_proba on a regression returns the value rather than squashing it.
    std::vector<std::vector<double>> fit_columns(columns.size());
    std::vector<double> labels;
    labels.reserve(usable.size());
    for (std::size_t index = 0; index < usable.size(); ++index) {
        const std::size_t row = usable[index];
        for (std::size_t feature = 0; feature < columns.size(); ++feature) {
            fit_columns[feature].push_back(columns[feature][row]);
        }
        labels.push_back(values[row]);
    }

    GbdtParams gbdt;
    gbdt.objective = "squared_error";
    const GbdtTrainResult trained = train_gbdt(fit_columns, result.feature_names, labels, gbdt);
    if (!trained.ok()) {
        result.status = trained.status;
        return result;
    }
    result.model = trained.model;

    const auto predict = [&](const BacktestFold& fold) {
        std::vector<double> predictions;
        predictions.reserve(fold.evaluate.size());
        for (const std::size_t index : fold.evaluate) {
            std::vector<double> row;
            row.reserve(fit_columns.size());
            for (const std::vector<double>& column : fit_columns) row.push_back(column[index]);
            predictions.push_back(result.model.predict_proba(row));
        }
        return predictions;
    };
    result.backtest = backtest(usable_times, labels, folds, predict);
    if (result.backtest.folds_evaluated == 0) {
        result.status = compute::ComputeStatus::invalid_argument(
            "train_forecast: no backtest fold could be scored, so the model has no honest "
            "quality figure and must not be reported as trained");
        return result;
    }
    return result;
}

}  // namespace zetta::ml
