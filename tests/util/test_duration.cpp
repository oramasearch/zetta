#include <gtest/gtest.h>

#include <chrono>

#include "zetta/util/duration.hpp"

using zetta::util::format_duration;
using namespace std::chrono;

TEST(FormatDuration, PicksUnitFromNanosecondsToWeeks) {
    EXPECT_EQ(format_duration(nanoseconds(0)), "0 ns");
    EXPECT_EQ(format_duration(nanoseconds(523)), "523 ns");
    EXPECT_EQ(format_duration(nanoseconds(1500)), "1.500 µs");
    EXPECT_EQ(format_duration(microseconds(250)), "250.000 µs");
    EXPECT_EQ(format_duration(milliseconds(5)), "5.000 ms");
    EXPECT_EQ(format_duration(milliseconds(1500)), "1.500 s");
    EXPECT_EQ(format_duration(seconds(90)), "1.50 min");
    EXPECT_EQ(format_duration(minutes(150)), "2.50 h");
    EXPECT_EQ(format_duration(hours(36)), "1.50 d");
    EXPECT_EQ(format_duration(hours(24 * 14)), "2.00 wk");
}

TEST(FormatDuration, BoundariesPickTheLargerUnit) {
    EXPECT_EQ(format_duration(microseconds(1)), "1.000 µs");  // 1000 ns -> µs
    EXPECT_EQ(format_duration(milliseconds(1)), "1.000 ms");
    EXPECT_EQ(format_duration(seconds(1)), "1.000 s");
    EXPECT_EQ(format_duration(minutes(1)), "1.00 min");
    EXPECT_EQ(format_duration(hours(1)), "1.00 h");
    EXPECT_EQ(format_duration(hours(24)), "1.00 d");
    EXPECT_EQ(format_duration(hours(24 * 7)), "1.00 wk");
}
