#pragma once

#include <chrono>
#include <string>

namespace zetta::util {

// Format a duration with a human-friendly unit chosen by magnitude, from
// nanoseconds up to weeks (ns, µs, ms, s, min, h, d, wk). Sub-second units show
// 3 decimals, larger units 2, whole nanoseconds none — e.g. "523 ns",
// "1.500 µs", "5.000 ms", "1.50 min", "2.00 wk".
std::string format_duration(std::chrono::nanoseconds d);

}  // namespace zetta::util