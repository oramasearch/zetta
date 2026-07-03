#include "zetta/util/timestamp.hpp"

#include <chrono>
#include <format>

namespace zetta::util {

namespace {

std::string format_iso8601(std::chrono::system_clock::time_point when) {
    using namespace std::chrono;
    const auto midnight = floor<days>(when);
    const year_month_day date{midnight};
    const auto since_midnight = when - midnight;
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

}  // namespace

std::string now_iso8601() {
    return format_iso8601(std::chrono::system_clock::now());
}

std::string iso8601_seconds_ago(long long seconds_ago) {
    return format_iso8601(std::chrono::system_clock::now() -
                          std::chrono::seconds(seconds_ago));
}

}  // namespace zetta::util
