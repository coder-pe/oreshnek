// oreshnek/src/server/RateLimiter.cpp
#include "oreshnek/server/RateLimiter.h"

#include <algorithm>

namespace Oreshnek {
namespace Server {

TokenBucketLimiter::TokenBucketLimiter(double rate_per_sec, double burst)
    : rate_(rate_per_sec > 0 ? rate_per_sec : 1.0),
      burst_(burst > 0 ? burst : 1.0) {}

bool TokenBucketLimiter::allow(const std::string& key) {
    const auto now = std::chrono::steady_clock::now();
    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        // New client starts with a full bucket, then spends one token.
        buckets_.emplace(key, Bucket{burst_ - 1.0, now});
        return true;
    }

    Bucket& b = it->second;
    const double elapsed =
        std::chrono::duration<double>(now - b.last_refill).count();
    b.tokens = std::min(burst_, b.tokens + elapsed * rate_);
    b.last_refill = now;

    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    return false;
}

void TokenBucketLimiter::evict_idle() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        Bucket& b = it->second;
        const double elapsed =
            std::chrono::duration<double>(now - b.last_refill).count();
        // A bucket that has refilled to capacity carries no state worth keeping.
        if (b.tokens + elapsed * rate_ >= burst_) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace Server
}  // namespace Oreshnek
