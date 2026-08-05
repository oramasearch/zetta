#include <gtest/gtest.h>

#include <vector>

#include "zetta/agent/advantage.hpp"

namespace {

using zetta::agent::group_advantages;
using zetta::agent::group_stats;

TEST(Advantage, MeanCenteredSumsToZero) {
    const std::vector<double> advantages =
        group_advantages({1.0, 0.5, 0.0, 0.25});
    double sum = 0.0;
    for (double advantage : advantages) {
        sum += advantage;
    }
    EXPECT_NEAR(sum, 0.0, 1e-9);
}

TEST(Advantage, AboveMeanIsPositiveBelowIsNegative) {
    const std::vector<double> advantages = group_advantages({1.0, 0.0, 0.0});
    EXPECT_GT(advantages[0], 0.0);
    EXPECT_LT(advantages[1], 0.0);
    EXPECT_LT(advantages[2], 0.0);
}

TEST(Advantage, NormalizedToUnitScale) {
    // mean 0.5, stddev 0.5 -> advantages are +1 / -1 (within epsilon).
    const std::vector<double> advantages = group_advantages({1.0, 0.0});
    EXPECT_NEAR(advantages[0], 1.0, 1e-6);
    EXPECT_NEAR(advantages[1], -1.0, 1e-6);
}

TEST(Advantage, EqualRewardsGiveNoSignal) {
    const std::vector<double> advantages = group_advantages({0.5, 0.5, 0.5});
    for (double advantage : advantages) {
        EXPECT_NEAR(advantage, 0.0, 1e-6);
    }
}

TEST(Advantage, SingleRewardHasZeroAdvantage) {
    const std::vector<double> advantages = group_advantages({0.7});
    ASSERT_EQ(advantages.size(), 1u);
    EXPECT_NEAR(advantages[0], 0.0, 1e-6);
}

TEST(Advantage, EmptyIsEmpty) {
    EXPECT_TRUE(group_advantages({}).empty());
}

TEST(Advantage, GroupStatsReportMeanAndSpread) {
    const auto stats = group_stats({1.0, 0.0});
    EXPECT_NEAR(stats.mean, 0.5, 1e-12);
    EXPECT_NEAR(stats.stddev, 0.5, 1e-12);
}

}  // namespace
