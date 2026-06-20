// oreshnek/include/oreshnek/server/RateLimiter.h
#ifndef ORESHNEK_SERVER_RATE_LIMITER_H
#define ORESHNEK_SERVER_RATE_LIMITER_H

#include <chrono>
#include <string>
#include <unordered_map>

namespace Oreshnek {
namespace Server {

// Per-key token-bucket rate limiter. Each key (typically a client IP) gets a
// bucket that refills at `rate` tokens/second up to `burst` capacity; a request
// is allowed iff a token is available.
//
// NOT thread-safe: it is designed to be called only from the single event-loop
// thread (the rate-limit decision happens before a request is handed to a
// worker), so it needs no locking.
class TokenBucketLimiter {
public:
    TokenBucketLimiter(double rate_per_sec, double burst);

    // Consume a token for `key`. Returns true if allowed, false if rate-limited.
    bool allow(const std::string& key);

    // Drop buckets that have refilled to full capacity (idle clients) to bound
    // memory. Cheap; call periodically from the event loop's maintenance sweep.
    void evict_idle();

    std::size_t tracked() const { return buckets_.size(); }

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;
    };

    double rate_;
    double burst_;
    std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace Server
}  // namespace Oreshnek

#endif  // ORESHNEK_SERVER_RATE_LIMITER_H
