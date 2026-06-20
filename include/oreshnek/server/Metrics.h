// oreshnek/include/oreshnek/server/Metrics.h
#ifndef ORESHNEK_SERVER_METRICS_H
#define ORESHNEK_SERVER_METRICS_H

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace Oreshnek {
namespace Server {

// Process-wide server metrics, exported in Prometheus text exposition format.
// All counters are atomic: they are updated from both the event-loop thread
// (accept / rate-limit / handler-timeout) and worker threads (per-response), so
// no locking is needed.
class Metrics {
public:
    // Upper bounds (seconds) of the request-duration histogram buckets; the
    // implicit +Inf bucket equals the total count.
    static constexpr std::array<double, 9> kBuckets{
        0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0};

    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> responses_2xx{0};
    std::atomic<uint64_t> responses_3xx{0};
    std::atomic<uint64_t> responses_4xx{0};
    std::atomic<uint64_t> responses_5xx{0};
    std::atomic<uint64_t> connections_accepted{0};
    std::atomic<int64_t>  connections_active{0};
    std::atomic<uint64_t> rate_limited_total{0};
    std::atomic<uint64_t> handler_timeouts_total{0};

    // Record a response by its numeric status code (buckets it into 2xx..5xx).
    void record_status(int code);

    // Record one request's processing duration (seconds) into the histogram.
    void observe_duration(double seconds);

    // Render all metrics in Prometheus text exposition format.
    std::string render() const;

private:
    std::array<std::atomic<uint64_t>, kBuckets.size()> duration_buckets_{};
    std::atomic<uint64_t> duration_count_{0};
    std::atomic<double>   duration_sum_{0.0};
};

}  // namespace Server
}  // namespace Oreshnek

#endif  // ORESHNEK_SERVER_METRICS_H
