// oreshnek/src/server/Metrics.cpp
#include "oreshnek/server/Metrics.h"

#include <sstream>

namespace Oreshnek {
namespace Server {

void Metrics::record_status(int code) {
    if (code >= 200 && code < 300) responses_2xx.fetch_add(1, std::memory_order_relaxed);
    else if (code >= 300 && code < 400) responses_3xx.fetch_add(1, std::memory_order_relaxed);
    else if (code >= 400 && code < 500) responses_4xx.fetch_add(1, std::memory_order_relaxed);
    else if (code >= 500) responses_5xx.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::observe_duration(double seconds) {
    for (size_t i = 0; i < kBuckets.size(); ++i) {
        if (seconds <= kBuckets[i]) {
            duration_buckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    duration_count_.fetch_add(1, std::memory_order_relaxed);
    // Atomic double accumulation (C++20).
    double cur = duration_sum_.load(std::memory_order_relaxed);
    while (!duration_sum_.compare_exchange_weak(cur, cur + seconds,
                                                std::memory_order_relaxed)) {
    }
}

std::string Metrics::render() const {
    std::ostringstream o;
    auto counter = [&](const char* name, const char* help, uint64_t v) {
        o << "# HELP " << name << ' ' << help << "\n# TYPE " << name << " counter\n"
          << name << ' ' << v << '\n';
    };

    counter("oreshnek_requests_total", "Total HTTP requests received.",
            requests_total.load(std::memory_order_relaxed));

    o << "# HELP oreshnek_responses_total HTTP responses by status class.\n"
      << "# TYPE oreshnek_responses_total counter\n"
      << "oreshnek_responses_total{class=\"2xx\"} " << responses_2xx.load(std::memory_order_relaxed) << '\n'
      << "oreshnek_responses_total{class=\"3xx\"} " << responses_3xx.load(std::memory_order_relaxed) << '\n'
      << "oreshnek_responses_total{class=\"4xx\"} " << responses_4xx.load(std::memory_order_relaxed) << '\n'
      << "oreshnek_responses_total{class=\"5xx\"} " << responses_5xx.load(std::memory_order_relaxed) << '\n';

    counter("oreshnek_connections_accepted_total", "Total accepted connections.",
            connections_accepted.load(std::memory_order_relaxed));
    counter("oreshnek_rate_limited_total", "Requests rejected by the rate limiter.",
            rate_limited_total.load(std::memory_order_relaxed));
    counter("oreshnek_handler_timeouts_total", "Requests aborted by the handler timeout.",
            handler_timeouts_total.load(std::memory_order_relaxed));
    counter("oreshnek_load_shed_total", "Requests rejected with 503 due to the in-flight handler cap.",
            load_shed_total.load(std::memory_order_relaxed));

    o << "# HELP oreshnek_connections_active Currently open connections.\n"
      << "# TYPE oreshnek_connections_active gauge\n"
      << "oreshnek_connections_active " << connections_active.load(std::memory_order_relaxed) << '\n';

    o << "# HELP oreshnek_workers_in_flight Handlers dispatched to a worker and not yet completed.\n"
      << "# TYPE oreshnek_workers_in_flight gauge\n"
      << "oreshnek_workers_in_flight " << workers_in_flight.load(std::memory_order_relaxed) << '\n';

    // Histogram: cumulative buckets, then sum and count.
    o << "# HELP oreshnek_request_duration_seconds Request processing duration.\n"
      << "# TYPE oreshnek_request_duration_seconds histogram\n";
    for (size_t i = 0; i < kBuckets.size(); ++i) {
        o << "oreshnek_request_duration_seconds_bucket{le=\"" << kBuckets[i] << "\"} "
          << duration_buckets_[i].load(std::memory_order_relaxed) << '\n';
    }
    const uint64_t count = duration_count_.load(std::memory_order_relaxed);
    o << "oreshnek_request_duration_seconds_bucket{le=\"+Inf\"} " << count << '\n'
      << "oreshnek_request_duration_seconds_sum " << duration_sum_.load(std::memory_order_relaxed) << '\n'
      << "oreshnek_request_duration_seconds_count " << count << '\n';

    return o.str();
}

}  // namespace Server
}  // namespace Oreshnek
