#pragma once

#include <string>

namespace zetta::util {

// The current UTC time as an ISO-8601 string with millisecond precision and a
// trailing 'Z', e.g. "2026-06-25T14:03:21.123Z". Fixed-width, so the strings
// also sort chronologically as plain text. Used for the created/updated stamps
// on replicated entities (e.g. zetta::pipeline::Project).
std::string now_iso8601();

}  // namespace zetta::util