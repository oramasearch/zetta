// zetta::util timestamp round-trip + boundary tests (#629): now_iso8601 /
// iso8601_seconds_ago write the fixed-width UTC shape, and
// parse_iso8601_seconds is the non-throwing inverse that powers duration math
// between entity stamps (the Home dashboard's velocity KPI).

#include "zetta/util/timestamp.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>

namespace {

TEST(Timestamp, ParsesTheShapeNowIso8601Writes) {
    const std::optional<double> parsed =
        zetta::util::parse_iso8601_seconds("1970-01-01T00:00:00.000Z");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_DOUBLE_EQ(*parsed, 0.0);

    // A known fixed point: 2026-06-25T14:03:21.123Z.
    const std::optional<double> fixed =
        zetta::util::parse_iso8601_seconds("2026-06-25T14:03:21.123Z");
    ASSERT_TRUE(fixed.has_value());
    EXPECT_NEAR(*fixed, 1782396201.123, 1e-6);

    // The seconds-precision variant (no fractional part) parses too.
    const std::optional<double> whole =
        zetta::util::parse_iso8601_seconds("2026-06-25T14:03:21Z");
    ASSERT_TRUE(whole.has_value());
    EXPECT_NEAR(*fixed - *whole, 0.123, 1e-6);
}

TEST(Timestamp, RoundTripsNowAndAgoStamps) {
    const std::optional<double> now =
        zetta::util::parse_iso8601_seconds(zetta::util::now_iso8601());
    const std::optional<double> hour_ago = zetta::util::parse_iso8601_seconds(
        zetta::util::iso8601_seconds_ago(3600));
    ASSERT_TRUE(now.has_value());
    ASSERT_TRUE(hour_ago.has_value());
    // The two calls straddle "now" by microseconds; the delta is ~3600s.
    EXPECT_NEAR(*now - *hour_ago, 3600.0, 2.0);
}

TEST(Timestamp, RejectsMalformedStamps) {
    EXPECT_FALSE(zetta::util::parse_iso8601_seconds("").has_value());
    EXPECT_FALSE(zetta::util::parse_iso8601_seconds("not a stamp").has_value());
    EXPECT_FALSE(  // missing the trailing Z
        zetta::util::parse_iso8601_seconds("2026-06-25T14:03:21.123").has_value());
    EXPECT_FALSE(  // date separators in the wrong places
        zetta::util::parse_iso8601_seconds("2026/06/25T14:03:21Z").has_value());
    EXPECT_FALSE(  // a bare dot with no fraction digits
        zetta::util::parse_iso8601_seconds("2026-06-25T14:03:21.Z").has_value());
    EXPECT_FALSE(  // an impossible calendar date
        zetta::util::parse_iso8601_seconds("2026-02-30T00:00:00.000Z").has_value());
    EXPECT_FALSE(  // an impossible time of day
        zetta::util::parse_iso8601_seconds("2026-06-25T24:00:00.000Z").has_value());
    EXPECT_FALSE(  // non-digits inside the fraction
        zetta::util::parse_iso8601_seconds("2026-06-25T14:03:21.1x3Z").has_value());
}

TEST(Timestamp, NormalizesIsoAndEpochMethodologyTimes) {
    const std::optional<double> iso = zetta::util::parse_time_seconds(
        nlohmann::json("2026-06-25T14:03:21.123Z"));
    const std::optional<double> epoch =
        zetta::util::parse_time_seconds(nlohmann::json(1782396201.123));
    const std::optional<double> date =
        zetta::util::parse_time_seconds(nlohmann::json("1970-01-02"));
    ASSERT_TRUE(iso);
    ASSERT_TRUE(epoch);
    ASSERT_TRUE(date);
    EXPECT_NEAR(*iso, *epoch, 1e-6);
    EXPECT_DOUBLE_EQ(*date, 86400.0);
    EXPECT_FALSE(zetta::util::parse_time_seconds(nlohmann::json("not-time")));
    EXPECT_FALSE(zetta::util::parse_time_seconds(nlohmann::json::object()));
}

}  // namespace
