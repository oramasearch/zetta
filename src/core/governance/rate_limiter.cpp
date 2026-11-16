// SPDX-License-Identifier: MIT

#include "zetta/governance/rate_limiter.hpp"

#include <algorithm>

namespace zetta::governance {

RateLimiter::Decision RateLimiter::admit(const std::string& key, std::uint32_t per_minute,
                                         Clock::time_point now) {
    constexpr auto kWindow = std::chrono::minutes(1);
    std::deque<Clock::time_point>& window = windows_[key];
    while (!window.empty() && now - window.front() >= kWindow) window.pop_front();
    if (window.size() < per_minute) {
        window.push_back(now);
        return {true, 0};
    }
    const auto wait = std::chrono::ceil<std::chrono::seconds>(kWindow - (now - window.front()));
    return {false, static_cast<std::uint32_t>(std::max<std::int64_t>(1, wait.count()))};
}

}  // namespace zetta::governance
