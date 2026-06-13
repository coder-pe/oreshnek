// tests/integration_test.cpp
//
// Socket-level integration tests for the Oreshnek server. These are designed to
// exercise the connection/concurrency model directly and are intended to be run
// under sanitizers (cmake -DORESHNEK_ASAN=ON or -DORESHNEK_TSAN=ON).
//
// Scenarios covered:
//   * keep-alive: many sequential requests on one connection
//   * pipelining: several requests written at once, N responses expected
//   * large body echo: stresses request-body parsing and partial socket writes
//   * concurrency: many simultaneous connections hammered from worker threads
//
// A test "passes" when every response is correct AND the sanitizer reports no
// data race / use-after-free. On the pre-Phase-1 code base the pipelining and
// concurrency tests are expected to fail (that is the point of the safety net).

#include "oreshnek/server/Server.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kPort = 18080;
constexpr const char* kHost = "127.0.0.1";

std::atomic<int> g_failures{0};

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        g_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Minimal blocking HTTP client that understands Content-Length framing and
// keeps leftover bytes between reads (so pipelined responses parse correctly).
// ---------------------------------------------------------------------------
class Client {
public:
    Client() = default;
    ~Client() { close_fd(); }

    bool connect() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        // Bound the time we are willing to wait so a broken server can't hang
        // the whole test run.
        timeval tv{};
        tv.tv_sec = 5;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        inet_pton(AF_INET, kHost, &addr.sin_addr);
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool send_all(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads one complete HTTP response (status line + headers + Content-Length
    // body). Returns the response body on success, std::nullopt on error.
    struct Response {
        int status = 0;
        std::string headers; // raw header block (without the trailing CRLFCRLF)
        std::string body;

        bool has_header(const std::string& needle) const {
            std::string lower = headers, n = needle;
            for (char& c : lower) c = static_cast<char>(::tolower(c));
            for (char& c : n) c = static_cast<char>(::tolower(c));
            return lower.find(n) != std::string::npos;
        }
    };

    // read_body=false is used for HEAD responses: parse status+headers but do
    // not consume a body (there is none, even though Content-Length is set).
    bool read_response(Response& out, bool read_body = true) {
        size_t header_end;
        while ((header_end = buf_.find("\r\n\r\n")) == std::string::npos) {
            if (!fill()) return false;
        }

        size_t sp = buf_.find(' ');
        out.status = (sp != std::string::npos) ? std::atoi(buf_.c_str() + sp + 1) : 0;
        out.headers = buf_.substr(0, header_end);

        size_t body_start = header_end + 4;
        if (!read_body) {
            buf_.erase(0, body_start);
            return true;
        }

        size_t content_length = parse_content_length(out.headers);
        while (buf_.size() < body_start + content_length) {
            if (!fill()) return false;
        }
        out.body = buf_.substr(body_start, content_length);
        buf_.erase(0, body_start + content_length);
        return true;
    }

private:
    bool fill() {
        char tmp[65536];
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf_.append(tmp, static_cast<size_t>(n));
        return true;
    }

    static size_t parse_content_length(const std::string& headers) {
        // Case-insensitive search for the Content-Length header value.
        std::string lower = headers;
        for (char& c : lower) c = static_cast<char>(::tolower(c));
        size_t pos = lower.find("content-length:");
        if (pos == std::string::npos) return 0;
        pos += std::strlen("content-length:");
        return static_cast<size_t>(std::strtoul(headers.c_str() + pos, nullptr, 10));
    }

    void close_fd() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
    std::string buf_;
};

std::string make_request(const std::string& method, const std::string& path,
                         const std::string& body = "",
                         const std::string& extra_headers = "") {
    std::string req = method + " " + path + " HTTP/1.1\r\n";
    req += "Host: localhost\r\n";
    if (!body.empty() || method == "POST") {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Content-Type: text/plain\r\n";
    }
    req += extra_headers;
    req += "\r\n";
    req += body;
    return req;
}

// Known file content served by the /file route, written to disk in main().
std::string g_file_content;
std::string g_file_path;

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------
void test_keep_alive() {
    Client c;
    check(c.connect(), "keep_alive: connect");
    for (int i = 0; i < 50; ++i) {
        check(c.send_all(make_request("GET", "/ping")), "keep_alive: send");
        Client::Response resp;
        check(c.read_response(resp), "keep_alive: read");
        check(resp.status == 200, "keep_alive: status 200");
        check(resp.body == "pong", "keep_alive: body == pong, got '" + resp.body + "'");
    }
}

void test_pipelining() {
    Client c;
    check(c.connect(), "pipelining: connect");
    constexpr int N = 16;
    std::string batch;
    for (int i = 0; i < N; ++i) batch += make_request("GET", "/ping");
    check(c.send_all(batch), "pipelining: send batch");
    for (int i = 0; i < N; ++i) {
        Client::Response resp;
        check(c.read_response(resp), "pipelining: read #" + std::to_string(i));
        check(resp.body == "pong",
              "pipelining: body #" + std::to_string(i) + " == pong, got '" + resp.body + "'");
    }
}

void test_large_body_echo() {
    Client c;
    check(c.connect(), "large_body: connect");
    std::string payload(512 * 1024, 'x');  // 512 KiB, larger than write chunk
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>('a' + (i % 26));
    check(c.send_all(make_request("POST", "/echo", payload)), "large_body: send");
    Client::Response resp;
    check(c.read_response(resp), "large_body: read");
    check(resp.body.size() == payload.size(),
          "large_body: size " + std::to_string(resp.body.size()) + " == " +
              std::to_string(payload.size()));
    check(resp.body == payload, "large_body: content matches");
}

void test_concurrency() {
    constexpr int kThreads = 8;
    constexpr int kReqs = 40;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([] {
            Client c;
            if (!c.connect()) {
                check(false, "concurrency: connect");
                return;
            }
            for (int i = 0; i < kReqs; ++i) {
                if (!c.send_all(make_request("GET", "/ping"))) {
                    check(false, "concurrency: send");
                    return;
                }
                Client::Response resp;
                if (!c.read_response(resp)) {
                    check(false, "concurrency: read");
                    return;
                }
                check(resp.body == "pong", "concurrency: body == pong, got '" + resp.body + "'");
            }
        });
    }
    for (auto& th : threads) th.join();
}

void test_file_full() {
    Client c;
    check(c.connect(), "file_full: connect");
    check(c.send_all(make_request("GET", "/file")), "file_full: send");
    Client::Response resp;
    check(c.read_response(resp), "file_full: read");
    check(resp.status == 200, "file_full: status 200");
    check(resp.has_header("accept-ranges: bytes"), "file_full: Accept-Ranges present");
    check(resp.body == g_file_content,
          "file_full: body matches file (" + std::to_string(resp.body.size()) + " bytes)");
}

void test_file_range() {
    Client c;
    check(c.connect(), "file_range: connect");
    // Request bytes 10-19 (10 bytes).
    check(c.send_all(make_request("GET", "/file", "", "Range: bytes=10-19\r\n")), "file_range: send");
    Client::Response resp;
    check(c.read_response(resp), "file_range: read");
    check(resp.status == 206, "file_range: status 206, got " + std::to_string(resp.status));
    check(resp.has_header("content-range: bytes 10-19/"), "file_range: Content-Range header");
    check(resp.body == g_file_content.substr(10, 10),
          "file_range: body == bytes[10..19], got '" + resp.body + "'");

    // Suffix range: last 5 bytes.
    Client c2;
    check(c2.connect(), "file_range2: connect");
    check(c2.send_all(make_request("GET", "/file", "", "Range: bytes=-5\r\n")), "file_range2: send");
    Client::Response r2;
    check(c2.read_response(r2), "file_range2: read");
    check(r2.status == 206, "file_range2: status 206");
    check(r2.body == g_file_content.substr(g_file_content.size() - 5),
          "file_range2: body == last 5 bytes");
}

void test_head() {
    Client c;
    check(c.connect(), "head: connect");
    check(c.send_all(make_request("HEAD", "/file")), "head: send");
    Client::Response resp;
    check(c.read_response(resp, /*read_body=*/false), "head: read");
    check(resp.status == 200, "head: status 200");
    check(resp.has_header("content-length: " + std::to_string(g_file_content.size())),
          "head: Content-Length reflects full size");
    check(resp.body.empty(), "head: no body");

    // A subsequent request on the same connection must still work (no body
    // bytes were left unconsumed by the HEAD response).
    check(c.send_all(make_request("GET", "/ping")), "head: pipelined GET send");
    Client::Response ping;
    check(c.read_response(ping), "head: pipelined GET read");
    check(ping.body == "pong", "head: keep-alive after HEAD works");
}

void test_chunked() {
    Client c;
    check(c.connect(), "chunked: connect");
    // "Wiki" + "pedia" + " in\r\n\r\nchunks." => decoded body.
    std::string req =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "E\r\n in\r\n\r\nchunks.\r\n"
        "0\r\n\r\n";
    check(c.send_all(req), "chunked: send");
    Client::Response resp;
    check(c.read_response(resp), "chunked: read");
    check(resp.status == 200, "chunked: status 200, got " + std::to_string(resp.status));
    check(resp.body == "Wikipedia in\r\n\r\nchunks.",
          "chunked: decoded body matches, got '" + resp.body + "'");
}

void test_expect_continue() {
    Client c;
    check(c.connect(), "expect: connect");
    // Send headers only, advertising Expect: 100-continue.
    std::string head =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "Expect: 100-continue\r\n"
        "\r\n";
    check(c.send_all(head), "expect: send headers");

    Client::Response interim;
    check(c.read_response(interim), "expect: read 100");
    check(interim.status == 100, "expect: got 100 Continue, got " + std::to_string(interim.status));

    // Now send the body and read the final response.
    check(c.send_all("hello"), "expect: send body");
    Client::Response resp;
    check(c.read_response(resp), "expect: read final");
    check(resp.status == 200, "expect: final status 200");
    check(resp.body == "hello", "expect: echoed body == hello, got '" + resp.body + "'");
}

}  // namespace

int main() {
    using namespace Oreshnek;

    // Write a known file to serve.
    g_file_path = "/tmp/oreshnek_it_file.bin";
    g_file_content.resize(100000);
    for (size_t i = 0; i < g_file_content.size(); ++i)
        g_file_content[i] = static_cast<char>('A' + (i % 26));
    {
        std::ofstream ofs(g_file_path, std::ios::binary);
        ofs.write(g_file_content.data(), g_file_content.size());
    }

    Server::Server server(4);
    server.get("/ping", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("pong");
    });
    server.post("/echo", [](const Http::HttpRequest& req, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).body(std::string(req.body()));
    });
    server.get("/file", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).file(g_file_path, "application/octet-stream");
    });

    if (!server.listen(kHost, kPort)) {
        std::cerr << "[FATAL] server failed to listen on " << kHost << ":" << kPort << std::endl;
        return 1;
    }
    std::thread server_thread([&server] { server.run(); });

    // Give the event loop a moment to enter epoll_wait/kevent.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    test_keep_alive();
    test_pipelining();
    test_large_body_echo();
    test_concurrency();
    test_file_full();
    test_file_range();
    test_head();
    test_chunked();
    test_expect_continue();

    // Correct shutdown contract: signal the loop, then join its thread. run()
    // tears down its own connections/fds; the Server destructor stops the pool.
    server.request_stop();
    server_thread.join();

    int failures = g_failures.load();
    if (failures == 0) {
        std::cout << "[OK] all integration tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << failures << " check(s) failed" << std::endl;
    return 1;
}
