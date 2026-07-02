#include "zetta/util/duration.hpp"

#include <cstdio>

namespace zetta {

std::string format_duration(std::chrono::nanoseconds d) {
    const double ns = static_cast<double>(d.count());
    const double magnitude = ns < 0 ? -ns : ns;

    struct Unit {
        const char* name;
        double per_ns;  // nanoseconds in one of this unit
        int decimals;
    };
    // Largest to smallest; the first unit the magnitude reaches is used.
    static const Unit units[] = {
        {"wk", 7.0 * 24 * 60 * 60 * 1e9, 2},
        {"d", 24.0 * 60 * 60 * 1e9, 2},
        {"h", 60.0 * 60 * 1e9, 2},
        {"min", 60.0 * 1e9, 2},
        {"s", 1e9, 3},
        {"ms", 1e6, 3},
        {"µs", 1e3, 3},
        {"ns", 1.0, 0},
    };

    char buf[64];
    for (const Unit& u : units) {
        if (magnitude >= u.per_ns) {
            std::snprintf(buf, sizeof(buf), "%.*f %s", u.decimals, ns / u.per_ns,
                          u.name);
            return buf;
        }
    }
    return "0 ns";  // magnitude < 1 ns (i.e. exactly zero)
}

}  // namespace zetta
