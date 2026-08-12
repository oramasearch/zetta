#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <vector>

#include "zetta/ml/forecast.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<double> days(std::size_t count) {
    std::vector<double> times(count);
    std::iota(times.begin(), times.end(), 0.0);
    return times;
}

// A weekly cycle on a rising trend — predictable from its own past, not from its mean.
std::vector<double> seasonal(std::size_t count) {
    std::vector<double> values(count);
    for (std::size_t day = 0; day < count; ++day) {
        values[day] = 100.0 + 0.5 * static_cast<double>(day) +
                      20.0 * std::sin(2.0 * kPi * static_cast<double>(day) / 7.0);
    }
    return values;
}

}  // namespace

TEST(Forecast, ForecastsASeasonalSeriesBetterThanItsOwnMean) {
    const std::vector<double> times = days(200);
    const std::vector<double> values = seasonal(200);

    const zetta::ml::ForecastResult result = zetta::ml::train_forecast(times, values);
    ASSERT_TRUE(result.ok()) << result.status.message;
    ASSERT_GT(result.backtest.folds_evaluated, 0u);

    // The baseline anyone would reach for: predict the mean. A forecast that cannot beat it has
    // learned nothing about the series, whatever its loss curve says.
    const double mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double baseline_error = 0.0;
    for (const double value : values) baseline_error += std::abs(value - mean);
    baseline_error /= static_cast<double>(values.size());

    EXPECT_LT(result.backtest.mean_error, baseline_error)
        << "backtest error " << result.backtest.mean_error << " vs predicting the mean "
        << baseline_error;
}

// The model must be a REGRESSION. If the objective were binary logistic, predict_proba would
// squash a forecast of 143.7 units to 1.0 and the error would be enormous but the run would look
// successful.
TEST(Forecast, TheModelIsARegressionNotAClassifier) {
    const std::vector<double> result_times = days(120);
    const zetta::ml::ForecastResult result =
        zetta::ml::train_forecast(result_times, seasonal(120));
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.model.objective, "squared_error");
}

// A row whose feature history is incomplete is DROPPED, never imputed. Back-filling a lag invents
// history, which is the one thing a forecast may not do.
TEST(Forecast, RowsWithNoHistoryAreDroppedRatherThanImputed) {
    const std::vector<double> times = days(60);
    const std::vector<double> values = seasonal(60);

    zetta::ml::ForecastParams params;
    params.lags = {28};          // the first 28 rows have no 28-lag
    params.rolling_window = 0;
    params.backtest_folds = 2;

    const zetta::ml::ForecastResult result = zetta::ml::train_forecast(times, values, params);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.feature_names.size(), 1u);
}

// Failing loudly beats returning a model with no honest quality figure.
TEST(Forecast, ASeriesTooShortToBacktestFailsRatherThanReturningAModel) {
    const std::vector<double> times = days(5);
    const std::vector<double> values = seasonal(5);

    const zetta::ml::ForecastResult result = zetta::ml::train_forecast(times, values);
    EXPECT_FALSE(result.ok())
        << "a forecast with no scoreable backtest fold has no quality figure and must not be "
           "reported as trained";
}

TEST(Forecast, MismatchedInputsAreRefusedAtTheBoundary) {
    EXPECT_FALSE(zetta::ml::train_forecast(days(10), seasonal(9)).ok());
    EXPECT_FALSE(zetta::ml::train_forecast({}, {}).ok());
}
