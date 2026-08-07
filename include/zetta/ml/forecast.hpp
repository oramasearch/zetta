#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "zetta/ml/backtest.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/lag_features.hpp"
#include "zetta/ml/temporal_split.hpp"

// The forecast run path (#984, M-4) - the composition that makes forecasting EXECUTABLE rather
// than merely expressible.
//
// Every piece already existed and had never been joined: lag featurization, the temporal split,
// rolling-origin backtesting, and the squared-error objective. This is the join, and it is where
// the methodology's correctness profile stops being a set of facts on a graph and becomes what
// actually runs.
//
// The order is not incidental. Features are built from strictly-past values, the split is by TIME
// rather than by content hash, evaluation is a backtest rather than a held-out sample of rows, and
// the model is a regression rather than a classifier. Get any one wrong and the pipeline still
// produces a number - it is just a number about the past, dressed as a prediction.
namespace zetta::ml {

struct ForecastParams {
    std::vector<std::size_t> lags = {1, 7, 28};  // in row-order positions, time-sorted
    std::size_t rolling_window = 7;              // rows summarized before each row
    std::size_t backtest_folds = 4;
    double horizon = 7.0;                        // one fold's evaluation window, in time units
};

struct ForecastResult {
    GbdtModel model;
    BacktestResult backtest;          // per-fold error and the mean across folds
    std::vector<std::string> feature_names;
    compute::ComputeStatus status;

    bool ok() const noexcept { return status.ok(); }
};

// Train a forecast over `times` (the row timestamps) and `values` (the series being forecast).
//
// Non-throwing per the zetta-core boundary rule. Fails with a reason rather than a bad model when
// the series cannot support a forecast: too few dated rows, no usable lag, or a backtest with no
// scoreable fold.
ForecastResult train_forecast(const std::vector<double>& times,
                              const std::vector<double>& values,
                              const ForecastParams& params = {});

}  // namespace zetta::ml
