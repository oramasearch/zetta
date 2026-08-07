#include <gtest/gtest.h>

#include <cstring>
#include <cmath>
#include <numeric>
#include <vector>

#include "zetta/ml/temporal_split.hpp"

using zetta::ml::TemporalSplit;
using zetta::ml::temporal_cutoff;
using zetta::ml::temporal_split;
using zetta::ml::temporal_split_leaks;

namespace {

// Days 0..99, one row each.
std::vector<double> daily_series(std::size_t days) {
    std::vector<double> times(days);
    std::iota(times.begin(), times.end(), 0.0);
    return times;
}

// The EXISTING holdout rule (gbdt_train_job.cpp) in miniature: a content hash of the row decides
// which side it lands on. Correct for churn — sharding-invariant — and the thing this file exists
// to show is wrong for anything time-ordered.
TemporalSplit hash_split(const std::vector<double>& times, double fraction) {
    TemporalSplit split;
    for (std::size_t row = 0; row < times.size(); ++row) {
        std::uint64_t hash = 1469598103934665603ull;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &times[row], sizeof(bits));
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (bits >> (byte * 8)) & 0xffull;
            hash *= 1099511628211ull;
        }
        const double unit =
            static_cast<double>(hash >> 11) / static_cast<double>(1ull << 53);
        (unit < fraction ? split.evaluate : split.train).push_back(row);
    }
    return split;
}

}  // namespace

TEST(TemporalSplit, NoTrainingRowIsDatedAtOrAfterAnyEvaluationRow) {
    const std::vector<double> times = daily_series(100);
    const std::optional<double> cutoff = temporal_cutoff(times, 0.3);
    ASSERT_TRUE(cutoff.has_value());

    const TemporalSplit split = temporal_split(times, *cutoff);
    ASSERT_FALSE(split.train.empty());
    ASSERT_FALSE(split.evaluate.empty());

    for (const std::size_t row : split.train) {
        EXPECT_LT(times[row], *cutoff) << "row " << row << " is dated at or after the cutoff";
    }
    EXPECT_FALSE(temporal_split_leaks(times, split));
}

// The negative control, and the reason this primitive exists. The hash split scatters future rows
// into training: the model learns from data dated after the rows it is scored on, and the figure
// that comes out is not optimistic, it is meaningless. On a time series this is the leak that
// reads as a great result.
TEST(TemporalSplit, AHashSplitLeaksTheFutureIntoTraining) {
    const std::vector<double> times = daily_series(100);
    const TemporalSplit hashed = hash_split(times, 0.3);
    ASSERT_FALSE(hashed.train.empty());
    ASSERT_FALSE(hashed.evaluate.empty());

    EXPECT_TRUE(temporal_split_leaks(times, hashed))
        << "a content-hash split must be detectable as leaky on time-ordered data — if this "
           "passes, the check cannot tell a forecast you can quote from one you cannot";
}

TEST(TemporalSplit, TiedTimestampsLandOnTheSameSide) {
    // Ten rows sharing one timestamp, then ten later. If ties straddle the cutoff, two identical
    // rows land on opposite sides — a leak at a granularity nobody would notice.
    std::vector<double> times(10, 5.0);
    times.insert(times.end(), 10, 9.0);

    const std::optional<double> cutoff = temporal_cutoff(times, 0.5);
    ASSERT_TRUE(cutoff.has_value());
    const TemporalSplit split = temporal_split(times, *cutoff);

    for (const std::size_t row : split.train) {
        for (const std::size_t other : split.evaluate) {
            EXPECT_NE(times[row], times[other])
                << "identical timestamps must not straddle the cutoff";
        }
    }
}

TEST(TemporalSplit, AnUndatedRowIsDroppedRatherThanTreatedAsPast) {
    std::vector<double> times = daily_series(10);
    times.push_back(std::nan(""));

    std::size_t dropped = 0;
    const TemporalSplit split = temporal_split(times, 5.0, &dropped);
    EXPECT_EQ(dropped, 1u);
    EXPECT_EQ(split.train.size() + split.evaluate.size(), 10u)
        << "a row with no timestamp cannot be placed in time; silently calling it 'before' is "
           "how future rows reach training";
}

TEST(TemporalSplit, ANonsenseFractionRefusesRatherThanGuessing) {
    const std::vector<double> times = daily_series(10);
    EXPECT_FALSE(temporal_cutoff(times, 0.0).has_value());
    EXPECT_FALSE(temporal_cutoff(times, 1.0).has_value());
    EXPECT_FALSE(temporal_cutoff({}, 0.3).has_value())
        << "no dated rows means the caller must decide, not receive a cutoff that means nothing";
}
