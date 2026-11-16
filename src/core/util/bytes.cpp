// SPDX-License-Identifier: MIT

#include "zetta/util/bytes.hpp"

#include <cstdio>

namespace zetta::util {

std::string format_bytes(std::uintmax_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    char buf[48];
    if (bytes < 1024) {
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
        return buf;
    }
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
    return buf;
}

}  // namespace zetta::util