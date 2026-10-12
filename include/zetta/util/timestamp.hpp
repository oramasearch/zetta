#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::util {

// The current UTC time as an ISO-8601 string with millisecond precision and a
// trailing 'Z', e.g. "2026-06-25T14:03:21.123Z". Fixed-width, so the strings
// also sort chronologically as plain text. Used for the created/updated stamps
// on replicated entities (e.g. zetta::pipeline::Project).
std::string now_iso8601();

// The UTC time `seconds_ago` seconds before now, in the same fixed-width
// ISO-8601 shape — a cutoff stamp for time-window filters ("last hour", #622):
// compare it lexicographically against entity stamps.
std::string iso8601_seconds_ago(long long seconds_ago);

// Parse a UTC ISO-8601 stamp in the shape now_iso8601() writes
// ("2026-06-25T14:03:21.123Z"; the fractional part is optional) into seconds
// since the Unix epoch. The inverse needed for duration math between entity
// stamps (e.g. Home's intent→deployed velocity KPI, #629). Non-throwing
// boundary: nullopt when the text doesn't match the shape or names an
// impossible date/time.
std::optional<double> parse_iso8601_seconds(std::string_view stamp);

// Parse a methodology time value as finite epoch seconds. Numeric epoch values
// pass through unchanged; UTC ISO-8601 dates and date-times are normalized to
// the same numeric representation (#1199). Non-throwing at JSON boundaries.
std::optional<double> parse_time_seconds(const nlohmann::json& value);

}  // namespace zetta::util
