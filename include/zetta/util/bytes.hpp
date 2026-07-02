#pragma once

#include <cstdint>
#include <string>

namespace zetta::util {

// Format a byte count with a binary unit (B, KB, MB, GB, TB, PB). Exact bytes
// below 1 KB ("512 B"); one decimal above ("1.5 KB", "2.0 MB").
std::string format_bytes(std::uintmax_t bytes);

}  // namespace zetta::util