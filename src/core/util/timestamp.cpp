// SPDX-License-Identifier: MIT

#include "zetta/util/timestamp.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <string>

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

std::optional<double> parse_iso8601_seconds(std::string_view stamp) {
    // Shape: "YYYY-MM-DDTHH:MM:SS[.fraction]Z". Field positions are fixed;
    // only the fractional part varies in width (now_iso8601 writes 3 digits).
    const auto read_number = [&stamp](std::size_t offset, std::size_t digits,
                                      int& out) {
        if (offset + digits > stamp.size()) return false;
        const char* first = stamp.data() + offset;
        const auto result = std::from_chars(first, first + digits, out);
        return result.ec == std::errc() && result.ptr == first + digits;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (stamp.size() < 20 || !read_number(0, 4, year) || stamp[4] != '-' ||
        !read_number(5, 2, month) || stamp[7] != '-' ||
        !read_number(8, 2, day) || stamp[10] != 'T' ||
        !read_number(11, 2, hour) || stamp[13] != ':' ||
        !read_number(14, 2, minute) || stamp[16] != ':' ||
        !read_number(17, 2, second) || stamp.back() != 'Z') {
        return std::nullopt;
    }
    if (hour > 23 || minute > 59 || second > 60) return std::nullopt;

    double fraction = 0.0;
    if (stamp.size() > 20) {  // a fractional part sits between SS and 'Z'
        if (stamp[19] != '.') return std::nullopt;
        double scale = 0.1;
        for (std::size_t index = 20; index + 1 < stamp.size(); ++index) {
            const char digit = stamp[index];
            if (digit < '0' || digit > '9') return std::nullopt;
            fraction += (digit - '0') * scale;
            scale *= 0.1;
        }
        if (stamp.size() == 21) return std::nullopt;  // a bare "." before 'Z'
    }

    using namespace std::chrono;
    const year_month_day date{std::chrono::year(year), std::chrono::month(month),
                              std::chrono::day(day)};
    if (!date.ok()) return std::nullopt;
    const sys_days midnight{date};
    const auto whole_seconds = midnight.time_since_epoch() + hours(hour) +
                               minutes(minute) + seconds(second);
    return static_cast<double>(
               duration_cast<std::chrono::seconds>(whole_seconds).count()) +
           fraction;
}

std::optional<double> parse_time_seconds(const nlohmann::json& value) {
    if (value.is_number()) {
        const double seconds = value.get<double>();
        if (std::isfinite(seconds)) {
            return seconds;
        }
        return std::nullopt;
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    std::string stamp = value.get<std::string>();
    if (stamp.size() == 10) {
        stamp += "T00:00:00Z";
    } else if (!stamp.ends_with('Z')) {
        stamp += 'Z';
    }
    return parse_iso8601_seconds(stamp);
}

}  // namespace zetta::util
