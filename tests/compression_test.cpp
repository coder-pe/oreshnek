// tests/compression_test.cpp
//
// Fase 7 tests for response compression: gzip/brotli negotiation via
// Accept-Encoding, the size threshold, and that file responses are never
// compressed (so sendfile / video bytes are untouched).

#include "oreshnek/server/Server.h"
#include "oreshnek/http/Compression.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"

#include <zlib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
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

constexpr int kPort = 18096;

struct Resp {
    std::string headers; // raw header block, lowercased for matching
    std::string body;    // exactly Content-Length bytes (may be binary)
    bool has(const std::string& needle) const { return headers.find(needle) != std::string::npos; }
};

long content_length_of(const std::string& headers_lower) {
    size_t p = headers_lower.find("content-length:");
    if (p == std::string::npos) return -1;
    return std::strtol(headers_lower.c_str() + p + 15, nullptr, 10);
}

// One request -> one response (reads exactly Content-Length body bytes).
Resp round_trip(const std::string& request) {
    Resp r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(fd); return r; }
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::send(fd, request.data(), request.size(), 0);

    std::string raw;
    char buf[8192];
    size_t hdr_end = std::string::npos;
    long clen = -1;
    for (;;) {
        if (hdr_end == std::string::npos) {
            hdr_end = raw.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                std::string lower = raw.substr(0, hdr_end);
                for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                r.headers = lower;
                clen = content_length_of(lower);
                if (clen < 0) clen = 0;
            }
        }
        if (hdr_end != std::string::npos &&
            raw.size() >= hdr_end + 4 + static_cast<size_t>(clen)) {
            r.body = raw.substr(hdr_end + 4, static_cast<size_t>(clen));
            break;
        }
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return r;
}

// gunzip via zlib (gzip wrapper: windowBits 15+16).
std::string gunzip(const std::string& in) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return {};
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    char buf[16384];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK);
    inflateEnd(&zs);
    return ret == Z_STREAM_END ? out : std::string();
}

std::string get(const std::string& path, const std::string& accept_encoding) {
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";
    if (!accept_encoding.empty()) req += "Accept-Encoding: " + accept_encoding + "\r\n";
    req += "\r\n";
    return req;
}
}  // namespace

int main() {
    // A highly compressible text body well above the threshold.
    std::string big;
    for (int i = 0; i < 200; ++i) big += "the quick brown fox jumps over the lazy dog. ";

    const std::string file_path = "/tmp/ore_comp_test.txt";
    { std::ofstream(file_path) << std::string(1000, 'Z'); }

    Server::Server server(2);
    server.enable_compression(/*min_bytes=*/32, /*allow_brotli=*/true);
    server.get("/big", [&big](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text(big);
    });
    server.get("/small", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("tiny");
    });
    server.get("/file", [&file_path](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).file(file_path, "text/plain"); // compressible type, but a file
    });

    if (!server.listen("127.0.0.1", kPort)) { std::cerr << "[FATAL] listen\n"; return 1; }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 1) gzip negotiated -> Content-Encoding: gzip, Vary, and it round-trips.
    {
        Resp r = round_trip(get("/big", "gzip"));
        check(r.has("content-encoding: gzip"), "gzip: Content-Encoding present");
        check(r.has("vary: accept-encoding"), "gzip: Vary present");
        check(r.body.size() < big.size(), "gzip: body is smaller than original");
        check(gunzip(r.body) == big, "gzip: decompresses back to the original");
    }

    // 2) brotli negotiated (if compiled in) -> Content-Encoding: br, smaller body.
    if (Http::brotli_available()) {
        Resp r = round_trip(get("/big", "br"));
        check(r.has("content-encoding: br"), "br: Content-Encoding present");
        check(r.body.size() < big.size(), "br: body is smaller than original");
    } else {
        std::cerr << "[info] brotli not compiled in; skipping br test\n";
    }

    // 3) No Accept-Encoding -> identity (uncompressed).
    {
        Resp r = round_trip(get("/big", ""));
        check(!r.has("content-encoding:"), "identity: no Content-Encoding");
        check(r.body == big, "identity: body is the original");
    }

    // 4) Below the size threshold -> not compressed.
    {
        Resp r = round_trip(get("/small", "gzip"));
        check(!r.has("content-encoding:"), "small: not compressed (below threshold)");
        check(r.body == "tiny", "small: body intact");
    }

    // 5) File responses are never compressed (sendfile / video stay zero-copy).
    {
        Resp r = round_trip(get("/file", "gzip"));
        check(!r.has("content-encoding:"), "file: file responses are not compressed");
        check(r.body.size() == 1000, "file: full file body delivered");
    }

    // 6) Explicit q=0 refusal -> not compressed.
    {
        Resp r = round_trip(get("/big", "gzip;q=0"));
        check(!r.has("content-encoding:"), "q=0: refusal honoured (not compressed)");
    }

    server.request_stop();
    loop.join();
    ::unlink(file_path.c_str());

    if (g_failures == 0) {
        std::cout << "[OK] all compression tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
