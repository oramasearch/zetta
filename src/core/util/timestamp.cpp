#include "zetta/util/timestamp.hpp"

#include <chrono>
#include <format>

namespace zetta::util {

std::string now_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto midnight = floor<days>(now);
    const year_month_day date{midnight};
    const auto since_midnight = now - midnight;
    const auto hour = duration_cast<hours>(since_midnight);
    const auto minute = duration_cast<minutes>(since_midnight - hour);
    const auto second = duration_cast<seconds>(since_midnight - hour - minute);
    const auto millisecond =
        duration_cast<milliseconds>(since_midnight - hour - minute - second);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                       static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()), hour.count(),
                       minute.count(), second.count(), millisecond.count());
}

}  // namespace zetta::util