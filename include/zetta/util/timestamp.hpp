#pragma once

#include <string>

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

}  // namespace zetta::util