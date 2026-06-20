// tests/metrics_test.cpp
//
// Fase 6 test for the Prometheus /metrics endpoint: drive some traffic, then
// scrape /metrics and verify the exported counters/histogram.

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

constexpr int kPort = 18095;

// Open a connection, send `request`, return the full framed response.
std::string round_trip(const std::string& request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::send(fd, request.data(), request.size(), 0);

    std::string out;
    char buf[8192];
    size_t header_end = std::string::npos;
    long content_length = -1;
    for (;;) {
        if (header_end == std::string::npos) {
            header_end = out.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                std::string h = out.substr(0, header_end);
                for (char& c : h) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                size_t p = h.find("content-length:");
                content_length = (p == std::string::npos) ? 0
                                 : std::strtol(h.c_str() + p + 15, nullptr, 10);
            }
        }
        if (header_end != std::string::npos &&
            out.size() >= header_end + 4 + static_cast<size_t>(content_length)) {
            break;
        }
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return out;
}

// Extract the integer value on the sample line that starts with `label`
// (newline-anchored so it does not match the "# HELP/# TYPE" lines); -1 if absent.
long metric_value(const std::string& body, const std::string& label) {
    size_t p = body.find("\n" + label);
    if (p == std::string::npos) return -1;
    p += 1 + label.size();
    return std::strtol(body.c_str() + p, nullptr, 10);
}
}  // namespace

int main() {
    Server::Server server(2);
    server.enable_metrics("/metrics");
    server.get("/ping", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("pong");
    });

    if (!server.listen("127.0.0.1", kPort)) {
        std::cerr << "[FATAL] listen failed" << std::endl;
        return 1;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Three 2xx and one 4xx.
    for (int i = 0; i < 3; ++i) {
        round_trip("GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    }
    round_trip("GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    std::string m = round_trip("GET /metrics HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    server.request_stop();
    loop.join();

    check(m.find("200") != std::string::npos, "metrics endpoint returns 200");
    check(m.find("oreshnek_requests_total") != std::string::npos, "requests_total exported");
    check(m.find("oreshnek_request_duration_seconds_bucket") != std::string::npos,
          "duration histogram exported");
    check(m.find("oreshnek_connections_accepted_total") != std::string::npos,
          "connections_accepted exported");

    long c2xx = metric_value(m, "oreshnek_responses_total{class=\"2xx\"} ");
    long c4xx = metric_value(m, "oreshnek_responses_total{class=\"4xx\"} ");
    check(c2xx >= 3, "at least three 2xx responses counted");
    check(c4xx >= 1, "at least one 4xx response counted");

    long reqs = metric_value(m, "oreshnek_requests_total ");
    check(reqs >= 5, "requests_total counts all requests (incl. /metrics)");

    if (g_failures == 0) {
        std::cout << "[OK] all metrics tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
