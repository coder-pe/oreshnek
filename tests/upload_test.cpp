// tests/upload_test.cpp
//
// Tests for streaming large request bodies straight to disk
// (Server::enable_upload_streaming). A body above the configured threshold is
// spooled to a temp file as it arrives — bypassing the ~1 MiB read buffer — and
// the handler receives its path via HttpRequest::body_file(). Exercised at the
// socket level: a multi-MiB upload (which the old in-memory path could not
// accept), the size cap (413), the small-body buffered path, and temp-file
// cleanup.

#include "oreshnek/server/Server.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace Oreshnek;
namespace fs = std::filesystem;

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

void send_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}
void send_all(int fd, const std::string& s) { send_all(fd, s.data(), s.size()); }

std::string read_until_eof(int fd, int timeout_ms) {
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string out;
    char buf[8192];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
        break;
    }
    return out;
}

// Deterministic payload + additive checksum so we can verify integrity without
// keeping the whole thing around twice.
std::string make_payload(size_t n) {
    std::string s;
    s.resize(n);
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    return s;
}
uint64_t checksum(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
uint64_t checksum_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return checksum(data);
}

size_t count_spool_files(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file()) ++n;
    }
    return n;
}
}  // namespace

int main() {
    // The server closes the connection on 413 while the client is still sending
    // the (rejected) oversized body; ignore SIGPIPE so that write fails softly.
    ::signal(SIGPIPE, SIG_IGN);

    const fs::path base = fs::temp_directory_path() / "oreshnek_upload_test";
    const fs::path spool = base / "spool";
    const fs::path storage = base / "storage";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(storage, ec);

    const int port = 18101;
    Server::Server server(2);
    // Generous timeouts so the multi-MiB upload never trips a sweep during CI.
    server.configure(Server::Server::Settings{/*read*/0, /*write*/10, /*idle*/0, /*grace*/5,
                                              /*handler*/0, /*max_handlers*/0});
    // Threshold 64 KiB; cap 4 MiB (so a 3 MiB upload passes, a 5 MiB one is 413).
    server.enable_upload_streaming(spool.string(), /*max*/4 * 1024 * 1024, /*threshold*/64 * 1024);

    // Streaming upload: move the spooled body into storage/<name>.
    server.put("/up/:name", [&](const Http::HttpRequest& req, Http::HttpResponse& res) {
        const auto& bf = req.body_file();
        if (!bf) {
            res.status(Http::HttpStatus::BAD_REQUEST).json({{"error", "no body file"}});
            return;
        }
        const std::string name(req.param("name").value_or("f"));
        const fs::path dest = storage / name;
        std::error_code e;
        fs::rename(*bf, dest, e);
        if (e) {  // cross-device fallback
            fs::copy_file(*bf, dest, fs::copy_options::overwrite_existing, e);
            fs::remove(*bf, e);
        }
        const auto sz = fs::file_size(dest, e);
        res.status(Http::HttpStatus::CREATED).json(
            {{"name", name}, {"size", static_cast<long long>(sz)}});
    });

    // Small buffered body: echo its length (exercises the unchanged in-memory path).
    server.post("/small", [](const Http::HttpRequest& req, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).json({{"len", static_cast<long long>(req.body().size())}});
    });

    if (!server.listen(kHost, port)) {
        check(false, "server failed to listen");
        return 1;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // --- Test 1: 3 MiB upload streams to disk intact (old path capped at ~1 MiB) -
    {
        const std::string payload = make_payload(3 * 1024 * 1024);
        int fd = connect_to(port);
        check(fd >= 0, "stream: connected");
        std::string head = "PUT /up/big.bin HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                           std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
        send_all(fd, head);
        send_all(fd, payload);
        std::string resp = read_until_eof(fd, 5000);
        ::close(fd);

        check(resp.find("201") != std::string::npos, "stream: 3 MiB upload returns 201");
        const fs::path stored = storage / "big.bin";
        check(fs::exists(stored), "stream: file stored");
        check(fs::file_size(stored) == payload.size(), "stream: stored size matches");
        check(checksum_file(stored) == checksum(payload), "stream: stored bytes match (checksum)");
    }

    // --- Test 2: over the cap -> 413, nothing stored -----------------------------
    {
        const std::string payload = make_payload(5 * 1024 * 1024);  // > 4 MiB cap
        int fd = connect_to(port);
        check(fd >= 0, "cap: connected");
        std::string head = "PUT /up/toobig.bin HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                           std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n";
        send_all(fd, head);
        send_all(fd, payload);  // best-effort; server may close mid-send
        std::string resp = read_until_eof(fd, 5000);
        ::close(fd);

        check(resp.find("413") != std::string::npos, "cap: oversized upload returns 413");
        check(!fs::exists(storage / "toobig.bin"), "cap: nothing stored for rejected upload");
    }

    // --- Test 3: small body still goes through the buffered path ------------------
    {
        const std::string payload(100, 'x');  // < 64 KiB threshold
        int fd = connect_to(port);
        check(fd >= 0, "small: connected");
        std::string req = "POST /small HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                          std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
        send_all(fd, req);
        std::string resp = read_until_eof(fd, 3000);
        ::close(fd);
        check(resp.find("\"len\":100") != std::string::npos, "small: buffered body length echoed");
    }

    // --- Test 4: no temp files left behind ---------------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(count_spool_files(spool) == 0, "cleanup: spool dir has no leftover temp files");

    server.request_stop();
    loop.join();
    fs::remove_all(base, ec);

    if (g_failures == 0) {
        std::cout << "[OK] all upload tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
