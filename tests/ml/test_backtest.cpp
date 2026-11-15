// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <vector>

#include "zetta/ml/backtest.hpp"

using zetta::ml::BacktestFold;
using zetta::ml::backtest;
using zetta::ml::rolling_origins;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<double> days(std::size_t count) {
    std::vector<double> times(count);
    std::iota(times.begin(), times.end(), 0.0);
    return times;
}

// A weekly cycle. Predictable if you use the seasonality, not predictable from the mean.
std::vector<double> seasonal(std::size_t count) {
    std::vector<double> values(count);
    for (std::size_t day = 0; day < count; ++day) {
        values[day] = 100.0 + 20.0 * std::sin(2.0 * kPi * static_cast<double>(day) / 7.0);
    }
    return values;
}

}  // namespace

TEST(Backtest, ASeasonalForecastBeatsAConstantOne) {
    const std::vector<double> times = days(140);
    const std::vector<double> actuals = seasonal(140);
    const std::vector<BacktestFold> folds = rolling_origins(times, 4, 7.0);
    ASSERT_GE(folds.size(), 3u) << "140 days at a 7-day horizon must support several folds";

    // Baseline: the mean of what it has seen. Knows nothing about the cycle.
    const auto constant = [&](const BacktestFold& fold) {
        double sum = 0.0;
        for (const std::size_t row : fold.train) sum += actuals[row];
        const double mean = fold.train.empty() ? 0.0 : sum / static_cast<double>(fold.train.size());
        return std::vector<double>(fold.evaluate.size(), mean);
    };
    // Seasonal: the value from one week earlier. Uses only rows it is allowed to see.
    const auto seasonal_naive = [&](const BacktestFold& fold) {
        std::vector<double> predictions;
        predictions.reserve(fold.evaluate.size());
        for (const std::size_t row : fold.evaluate) {
            predictions.push_back(row >= 7 ? actuals[row - 7] : actuals[row]);
        }
        return predictions;
    };

    const double constant_error = backtest(times, actuals, folds, constant).mean_error;
    const double seasonal_error = backtest(times, actuals, folds, seasonal_naive).mean_error;

    EXPECT_LT(seasonal_error, constant_error)
        << "a forecast that uses the cycle must beat one that does not — if it does not, the "
           "harness is not measuring forecasting skill";
}

// The number this reports must be an average over honest attempts. A fold with nothing to train
// on or nothing to score is not an attempt, and including it would pad the count and move the
// mean while measuring nothing.
TEST(Backtest, DegenerateFoldsAreDroppedRatherThanPadded) {
    const std::vector<double> times = days(10);
    const std::vector<BacktestFold> folds = rolling_origins(times, 20, 7.0);

    EXPECT_LT(folds.size(), 20u) << "a 10-day series cannot support 20 seven-day folds";
    for (const BacktestFold& fold : folds) {
        EXPECT_FALSE(fold.train.empty());
        EXPECT_FALSE(fold.evaluate.empty());
    }
}

// Every fold must be honest on its own: nothing it trains on may be dated at or after anything it
// scores. This is the property that separates a backtest from a random split with extra steps.
TEST(Backtest, NoFoldTrainsOnItsOwnEvaluationWindow) {
    const std::vector<double> times = days(140);
    const std::vector<BacktestFold> folds = rolling_origins(times, 4, 7.0);
    ASSERT_FALSE(folds.empty());

    for (const BacktestFold& fold : folds) {
        double earliest_evaluation = std::numeric_limits<double>::infinity();
        for (const std::size_t row : fold.evaluate) {
            earliest_evaluation = std::min(earliest_evaluation, times[row]);
        }
        for (const std::size_t row : fold.train) {
            EXPECT_LT(times[row], earliest_evaluation)
                << "a training row dated at or after the evaluation window is the leak the "
                   "whole methodology exists to prevent";
        }
    }
}

TEST(Backtest, TheNewestFoldAlwaysExists) {
    // Origins walk backwards from the end, so the most recent window is always covered. A
    // backtest whose newest fold is missing is measuring the past and calling it the present.
    const std::vector<double> times = days(60);
    const std::vector<BacktestFold> folds = rolling_origins(times, 3, 7.0);
    ASSERT_FALSE(folds.empty());

    double latest_cutoff = -std::numeric_limits<double>::infinity();
    for (const BacktestFold& fold : folds) latest_cutoff = std::max(latest_cutoff, fold.cutoff);
    EXPECT_GE(latest_cutoff, times.back() - 7.0);
}
