// tests/lifecycle_test.cpp
//
// Fase 4 tests: graceful shutdown (in-flight requests are drained, not dropped)
// and the request read-timeout (a slow client that never finishes its request
// receives 408 and is closed).

#include "oreshnek/server/Server.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
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
    } else {
        std::cerr << "[ok] " << msg << std::endl;
    }
}

const char* kHost = "127.0.0.1";

int connect_to(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, kHost, &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void send_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

// Read until EOF or timeout_ms elapses; returns whatever was received.
std::string read_until_eof(int fd, int timeout_ms) {
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            continue;
        }
        break; // EOF (0) or timeout/error (<0)
    }
    return out;
}

// --- Test 1: graceful shutdown drains in-flight requests --------------------
void test_graceful_drain(int port) {
    Server::Server server(2);
    server.get("/slow", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        res.status(Http::HttpStatus::OK).text("drained");
    });

    if (!server.listen(kHost, port)) {
        check(false, "graceful: server failed to listen");
        return;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = connect_to(port);
    check(fd >= 0, "graceful: connected");
    send_all(fd, "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    // Let the worker pick up the request, then ask the server to stop while it
    // is still running the handler.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    server.request_stop();

    std::string resp = read_until_eof(fd, 3000);
    ::close(fd);
    loop.join();

    check(resp.find("200") != std::string::npos, "graceful: in-flight request got 200");
    check(resp.find("drained") != std::string::npos, "graceful: in-flight body delivered");
}

// --- Test 2: read timeout returns 408 ---------------------------------------
void test_read_timeout(int port) {
    Server::Server server(2);
    server.configure(Server::Server::Settings{/*read*/1, /*write*/5, /*idle*/30, /*grace*/5});
    server.post("/echo", [](const Http::HttpRequest& req, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).body(std::string(req.body()));
    });

    if (!server.listen(kHost, port)) {
        check(false, "read-timeout: server failed to listen");
        return;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = connect_to(port);
    check(fd >= 0, "read-timeout: connected");
    // Promise a body but never send it: the server should time out the read.
    send_all(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n");

    std::string resp = read_until_eof(fd, 5000);
    ::close(fd);
    server.request_stop();
    loop.join();

    check(resp.find("408") != std::string::npos, "read-timeout: slow request got 408");
}

// --- Test 3: handler timeout returns 504 ------------------------------------
void test_handler_timeout(int port) {
    Server::Server server(2);
    // read, write, idle, grace, handler=1s
    server.configure(Server::Server::Settings{30, 5, 30, 5, 1});
    server.get("/slow", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500)); // exceeds 1s
        res.status(Http::HttpStatus::OK).text("late");
    });

    if (!server.listen(kHost, port)) {
        check(false, "handler-timeout: server failed to listen");
        return;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int fd = connect_to(port);
    check(fd >= 0, "handler-timeout: connected");
    send_all(fd, "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    std::string resp = read_until_eof(fd, 4000);
    ::close(fd);
    server.request_stop();
    loop.join();

    check(resp.find("504") != std::string::npos, "handler-timeout: slow handler got 504");
}

// --- Test 4: load shedding returns 503 when the in-flight cap is reached -----
void test_load_shedding(int port) {
    Server::Server server(2);
    // read, write, idle, grace, handler=0 (disabled), max_concurrent_handlers=1.
    server.configure(Server::Server::Settings{30, 5, 30, 5, 0, 1});
    server.get("/slow", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        res.status(Http::HttpStatus::OK).text("done");
    });

    if (!server.listen(kHost, port)) {
        check(false, "load-shed: server failed to listen");
        return;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Connection A occupies the single in-flight slot with a slow handler.
    int fd_a = connect_to(port);
    check(fd_a >= 0, "load-shed: first connection");
    send_all(fd_a, "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    // Give the event loop time to dispatch A to a worker (gauge -> 1).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Connection B arrives while A is still running: it must be shed with 503.
    int fd_b = connect_to(port);
    check(fd_b >= 0, "load-shed: second connection");
    send_all(fd_b, "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    std::string resp_b = read_until_eof(fd_b, 2000);
    std::string resp_a = read_until_eof(fd_a, 3000);
    ::close(fd_b);
    ::close(fd_a);
    server.request_stop();
    loop.join();

    check(resp_b.find("503") != std::string::npos, "load-shed: capped request got 503");
    check(resp_b.find("Retry-After") != std::string::npos, "load-shed: 503 carries Retry-After");
    check(resp_a.find("200") != std::string::npos, "load-shed: in-flight request still got 200");
    check(server.metrics().load_shed_total.load() >= 1, "load-shed: counter incremented");
    check(server.metrics().workers_in_flight.load() == 0, "load-shed: in-flight gauge drains to 0");
}
}  // namespace

int main() {
    test_graceful_drain(18091);
    test_read_timeout(18092);
    test_handler_timeout(18096);
    test_load_shedding(18097);

    if (g_failures == 0) {
        std::cout << "[OK] all lifecycle tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
