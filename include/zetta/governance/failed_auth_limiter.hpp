#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace zetta::governance {

// A failed-authentication budget per request source (#1417): the HTTP gate's
// bound on how much token-store work an unauthenticated caller can make the
// owner loop do. Each source (the connection's remote address) holds a bucket
// of kBurst failures, refilled at one per kRefill. Past it, the source's
// bearers get only the bounded checks (an issued `zt1_` bearer's one token, an
// already verified bearer's digest), never a token-store scan, and a failure
// answers 429. A valid issued token keeps working; a success does not refill
// the bucket, so a valid token cannot buy more scans.
//
// Bounded memory: at most kMaxSources sources are tracked; past that the source
// updated longest ago is forgotten. Not thread-safe: the HTTP adapter calls it
// from its one IO thread only, like its per-connection parser map.
class FailedAuthLimiter {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::uint32_t kBurst = 20;
    static constexpr std::chrono::milliseconds kRefill{1000};
    static constexpr std::size_t kMaxSources = 4096;

    struct Decision {
        bool admitted = true;
        std::uint32_t retry_after_seconds = 0;  // when refused
    };

    // May a request from `source` have its credential checked now?
    Decision admit(const std::string& source, Clock::time_point now = Clock::now());

    // Spend one failure from `source`'s budget.
    void record_failure(const std::string& source, Clock::time_point now = Clock::now());

    std::size_t tracked_sources() const { return buckets_.size(); }

private:
    struct Bucket {
        double tokens = kBurst;
        Clock::time_point updated{};
    };
    void refill(Bucket& bucket, Clock::time_point now) const;

    std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace zetta::governance
