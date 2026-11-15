#include "zetta/governance/failed_auth_limiter.hpp"

#include <algorithm>
#include <cmath>

namespace zetta::governance {

void FailedAuthLimiter::refill(Bucket& bucket, Clock::time_point now) const {
    if (now <= bucket.updated) return;
    const double elapsed =
        std::chrono::duration<double, std::milli>(now - bucket.updated).count();
    bucket.tokens = std::min<double>(
        kBurst, bucket.tokens + elapsed / static_cast<double>(kRefill.count()));
    bucket.updated = now;
}

FailedAuthLimiter::Decision FailedAuthLimiter::admit(const std::string& source,
                                                     Clock::time_point now) {
    const auto found = buckets_.find(source);
    if (found == buckets_.end()) return {};
    Bucket& bucket = found->second;
    refill(bucket, now);
    if (bucket.tokens >= 1.0) {
        // A full bucket carries no information: forget it to keep the map small.
        if (bucket.tokens >= kBurst) buckets_.erase(found);
        return {};
    }
    const double missing = 1.0 - bucket.tokens;
    const auto wait = std::ceil(missing * static_cast<double>(kRefill.count()) / 1000.0);
    return {false, static_cast<std::uint32_t>(std::max(1.0, wait))};
}

void FailedAuthLimiter::record_failure(const std::string& source, Clock::time_point now) {
    auto found = buckets_.find(source);
    if (found == buckets_.end()) {
        if (buckets_.size() >= kMaxSources) {
            const auto oldest = std::min_element(
                buckets_.begin(), buckets_.end(), [](const auto& left, const auto& right) {
                    return left.second.updated < right.second.updated;
                });
            if (oldest != buckets_.end()) buckets_.erase(oldest);
        }
        found = buckets_.emplace(source, Bucket{static_cast<double>(kBurst), now}).first;
    }
    Bucket& bucket = found->second;
    refill(bucket, now);
    bucket.tokens = std::max(0.0, bucket.tokens - 1.0);
}

}  // namespace zetta::governance
