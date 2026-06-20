// tests/rate_limit_test.cpp
//
// Fase 6 tests for per-IP rate limiting: the token-bucket unit logic plus an
// end-to-end check that a flood of requests is throttled with 429.

#include "oreshnek/server/RateLimiter.h"
#include "oreshnek/server/Server.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace Oreshnek;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}

// Count non-overlapping occurrences of `needle` in `hay`.
int count_of(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) {
        ++n;
    }
    return n;
}

void test_token_bucket() {
    Server::TokenBucketLimiter limiter(/*rate=*/1000.0, /*burst=*/3.0);

    // Burst of 3 is allowed immediately, the 4th (same instant) is throttled.
    check(limiter.allow("1.2.3.4"), "1st request allowed");
    check(limiter.allow("1.2.3.4"), "2nd request allowed");
    check(limiter.allow("1.2.3.4"), "3rd request allowed (burst)");
    check(!limiter.allow("1.2.3.4"), "4th request throttled");

    // A different IP has its own independent bucket.
    check(limiter.allow("5.6.7.8"), "other IP not affected");

    // After a short wait the bucket refills (1000/s -> well over 1 token in 5ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(limiter.allow("1.2.3.4"), "request allowed again after refill");

    // Idle (refilled-to-full) buckets are evicted to bound memory.
    Server::TokenBucketLimiter ev(1000.0, 2.0);
    ev.allow("x");
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // refills to full
    ev.evict_idle();
    check(ev.tracked() == 0, "idle bucket evicted");
}

void test_end_to_end(int port) {
    Server::Server server(2);
    server.enable_rate_limit(/*rate=*/5.0, /*burst=*/5.0);
    server.get("/ping", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("pong");
    });
    if (!server.listen("127.0.0.1", port)) {
        check(false, "server failed to listen");
        return;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    check(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "connected");

    // Pipeline 30 keep-alive requests; only ~burst (5) should pass, rest -> 429.
    constexpr int kN = 30;
    std::string req;
    for (int i = 0; i < kN; ++i) {
        req += "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    }
    ::send(fd, req.data(), req.size(), 0);

    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string out;
    char buf[8192];
    while (count_of(out, "HTTP/1.1 200") + count_of(out, "HTTP/1.1 429") < kN) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    server.request_stop();
    loop.join();

    int ok = count_of(out, "HTTP/1.1 200");
    int throttled = count_of(out, "HTTP/1.1 429");
    check(ok >= 1, "at least one request passed");
    check(throttled >= 1, "at least one request was throttled (429)");
    check(ok + throttled == kN, "every request received a response");
    check(ok <= 6, "throttling kept the passed count near the burst");
}
}  // namespace

int main() {
    test_token_bucket();
    test_end_to_end(18094);

    if (g_failures == 0) {
        std::cout << "[OK] all rate-limit tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
