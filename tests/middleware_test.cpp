// tests/middleware_test.cpp
//
// Fase 4 tests for the middleware chain: ordering, short-circuit, the built-in
// CORS preflight, and JWT-protected routes.

#include "oreshnek/server/Server.h"
#include "oreshnek/server/Middleware.h"
#include "oreshnek/platform/SecurityUtils.h"
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

const char* kHost = "127.0.0.1";
constexpr int kPort = 18093;

// Parse Content-Length (case-insensitive) from the header block; -1 if absent.
long content_length_of(const std::string& headers) {
    std::string lower = headers;
    for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    size_t pos = lower.find("content-length:");
    if (pos == std::string::npos) return -1;
    pos += std::string("content-length:").size();
    try {
        return std::stol(headers.substr(pos));
    } catch (...) {
        return -1;
    }
}

// Sends a raw request and reads exactly one framed response (headers +
// Content-Length body), so we never wait on the socket read timeout.
std::string round_trip(const std::string& raw) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    inet_pton(AF_INET, kHost, &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ::send(fd, raw.data(), raw.size(), 0);
    std::string out;
    char buf[4096];
    size_t header_end = std::string::npos;
    long content_length = -1;
    for (;;) {
        if (header_end == std::string::npos) {
            header_end = out.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                content_length = content_length_of(out.substr(0, header_end));
                if (content_length < 0) content_length = 0; // no body expected
            }
        }
        if (header_end != std::string::npos &&
            out.size() >= header_end + 4 + static_cast<size_t>(content_length)) {
            break; // Full response received.
        }
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
        break;
    }
    ::close(fd);
    return out;
}
}  // namespace

int main() {
    const std::string secret = "middleware-test-secret";

    Server::Server server(2);
    namespace MW = Server::Middlewares;

    server.use(MW::request_logger());
    server.use(MW::cors("*"));
    // Custom middleware: hard-block one path regardless of routing.
    server.use([](const Http::HttpRequest& req, Http::HttpResponse& res) -> bool {
        if (req.path() == "/blocked") {
            res.status(Http::HttpStatus::FORBIDDEN).text("blocked");
            return false;
        }
        return true;
    });
    server.use(MW::require_jwt(secret, {"/secure"}));

    server.get("/open", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("open");
    });
    server.get("/secure", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("secret");
    });

    if (!server.listen(kHost, kPort)) {
        std::cerr << "[FATAL] listen failed" << std::endl;
        return 1;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Unprotected route passes through and also carries CORS headers.
    {
        std::string r = round_trip("GET /open HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        check(r.find("200") != std::string::npos, "open route returns 200");
        check(r.find("open") != std::string::npos, "open route body delivered");
        check(r.find("Access-Control-Allow-Origin") != std::string::npos,
              "CORS header present on normal response");
    }

    // CORS preflight is short-circuited with 204.
    {
        std::string r = round_trip(
            "OPTIONS /secure HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        check(r.find("204") != std::string::npos, "OPTIONS preflight returns 204");
    }

    // Custom middleware short-circuits before routing (no /blocked route exists).
    {
        std::string r = round_trip("GET /blocked HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        check(r.find("403") != std::string::npos, "blocked path short-circuits with 403");
    }

    // Protected route without a token -> 401.
    {
        std::string r = round_trip("GET /secure HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        check(r.find("401") != std::string::npos, "protected route without token -> 401");
    }

    // Protected route with a valid token -> handler runs.
    {
        std::string token = Platform::SecurityUtils::generateJWT(1, "u", secret);
        std::string req = "GET /secure HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + token +
                          "\r\nConnection: close\r\n\r\n";
        std::string r = round_trip(req);
        check(r.find("200") != std::string::npos, "protected route with token -> 200");
        check(r.find("secret") != std::string::npos, "protected route body delivered");
    }

    server.request_stop();
    loop.join();

    if (g_failures == 0) {
        std::cout << "[OK] all middleware tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
