// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <vector>

#include "zetta/ml/lag_features.hpp"

using zetta::ml::feature_is_strictly_past;
using zetta::ml::lag_feature;
using zetta::ml::rolling_mean_before;

namespace {

std::vector<double> days(std::size_t count) {
    std::vector<double> times(count);
    std::iota(times.begin(), times.end(), 0.0);
    return times;
}

}  // namespace

TEST(LagFeatures, ALagTakesTheValueFromEarlierInTime) {
    const std::vector<double> times = days(5);
    const std::vector<double> values = {10.0, 20.0, 30.0, 40.0, 50.0};

    const std::vector<double> lag_one = lag_feature(times, values, 1);
    EXPECT_TRUE(std::isnan(lag_one[0])) << "the first row has no predecessor";
    EXPECT_DOUBLE_EQ(lag_one[1], 10.0);
    EXPECT_DOUBLE_EQ(lag_one[4], 40.0);
}

// The single most valuable assertion in this file. A lag of 0 is the row's own value — the
// target. Allowing it produces a model that scores perfectly and forecasts nothing, and the
// metric gives no hint.
TEST(LagFeatures, ALagOfZeroIsRefusedRatherThanReturningTheTarget) {
    const std::vector<double> times = days(5);
    const std::vector<double> values = {10.0, 20.0, 30.0, 40.0, 50.0};

    const std::vector<double> lag_zero = lag_feature(times, values, 0);
    for (const double entry : lag_zero) {
        EXPECT_TRUE(std::isnan(entry))
            << "a lag of 0 is the target, not a feature; returning it is a perfect model that "
               "predicts nothing";
    }
}

// Rows arrive in whatever order the store hands them over. A lag computed on ARRIVAL order rather
// than TIME order silently pairs each row with an arbitrary other row.
TEST(LagFeatures, OrderingIsByTimeNotByArrival) {
    const std::vector<double> times = {3.0, 1.0, 4.0, 2.0};
    const std::vector<double> values = {30.0, 10.0, 40.0, 20.0};

    const std::vector<double> lag_one = lag_feature(times, values, 1);
    // In time order the series is 10, 20, 30, 40 — so day 3 (value 30) follows day 2 (value 20).
    EXPECT_DOUBLE_EQ(lag_one[0], 20.0);
    EXPECT_TRUE(std::isnan(lag_one[1])) << "the earliest row has no predecessor";
    EXPECT_DOUBLE_EQ(lag_one[2], 30.0);
}

// The most common way a forecast leaks its target, and invisible in the resulting metric.
TEST(LagFeatures, ARollingWindowNeverIncludesItsOwnRow) {
    const std::vector<double> times = days(6);
    const std::vector<double> values = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};

    const std::vector<double> mean_of_three = rolling_mean_before(times, values, 3);
    EXPECT_TRUE(std::isnan(mean_of_three[2])) << "not enough history for a full window";
    // Row 3 sees rows 0,1,2 -> mean 2.0. If it included itself the mean would be 2.5.
    EXPECT_DOUBLE_EQ(mean_of_three[3], 2.0);
    EXPECT_DOUBLE_EQ(mean_of_three[4], 3.0);
}

TEST(LagFeatures, APartialWindowIsNaNRatherThanASmallerMean) {
    const std::vector<double> times = days(4);
    const std::vector<double> values = {1.0, 2.0, 3.0, 4.0};

    const std::vector<double> mean_of_three = rolling_mean_before(times, values, 3);
    for (std::size_t row = 0; row < 3; ++row) {
        EXPECT_TRUE(std::isnan(mean_of_three[row]))
            << "a mean over fewer rows than the window is a different feature wearing the same "
               "name, and it changes meaning across the series";
    }
    EXPECT_DOUBLE_EQ(mean_of_three[3], 2.0);
}

TEST(LagFeatures, AGeneratedLagIsStrictlyPastAndTheTargetIsNot) {
    const std::vector<double> times = days(6);
    const std::vector<double> values = {5.0, 15.0, 25.0, 35.0, 45.0, 55.0};

    EXPECT_TRUE(feature_is_strictly_past(times, values, lag_feature(times, values, 1)));
    EXPECT_FALSE(feature_is_strictly_past(times, values, values))
        << "the series itself is not a legal feature for forecasting it";
}
