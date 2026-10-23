#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace zetta::governance {

// A sliding one-minute admission window per key (zetta#1289): the per-caller
// limit on a published pipeline's served route. `admit` lets a call through
// when fewer than `per_minute` calls under the same key were admitted in the
// minute before `now`, and records it; otherwise it refuses and says how many
// seconds until the oldest admitted call leaves the window.
//
// Node-local by design: each node counts the calls it serves, so a caller
// spreading calls over N nodes is admitted up to N times the limit. The limit
// bounds what one node spends on one caller; a cluster-wide count would be a
// replicated counter per call, which this slice does not add. Owner-thread
// only, like the served routes that use it.
class RateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    struct Decision {
        bool admitted = false;
        std::uint32_t retry_after_seconds = 0;  // when refused
    };

    Decision admit(const std::string& key, std::uint32_t per_minute,
                   Clock::time_point now = Clock::now());

    // Forget every key (a test, or a node restart, which also forgets).
    void clear() { windows_.clear(); }

private:
    std::map<std::string, std::deque<Clock::time_point>> windows_;
};

}  // namespace zetta::governance
