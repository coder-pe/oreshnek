// tests/tls_test.cpp
//
// Fase 6 test for TLS/HTTPS: a self-signed certificate is generated at runtime,
// the server is started with TLS enabled, and an OpenSSL client performs a full
// handshake + request/response over the encrypted connection.

#include "oreshnek/server/Server.h"
#include "oreshnek/http/HttpRequest.h"
#include "oreshnek/http/HttpResponse.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
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

const char* kHost = "127.0.0.1";
constexpr int kPort = 18443;
const std::string kDir = "/tmp/ore_tls_test";
const std::string kCert = kDir + "/cert.pem";
const std::string kKey = kDir + "/key.pem";

// Generate a throwaway self-signed cert/key. Returns false if openssl is absent.
bool generate_cert() {
    std::string cmd =
        "mkdir -p " + kDir + " && openssl req -x509 -newkey rsa:2048 -keyout " + kKey +
        " -out " + kCert + " -days 1 -nodes -subj /CN=localhost -batch >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// Blocking TLS client: connect, handshake, send `request`, return the response
// (read until the Content-Length body is complete).
std::string tls_round_trip(const std::string& request) {
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
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    std::string out;
    if (SSL_connect(ssl) == 1) {  // self-signed: we do not verify here
        SSL_write(ssl, request.data(), static_cast<int>(request.size()));
        char buf[4096];
        long content_length = -1;
        size_t header_end = std::string::npos;
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
            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
            break;
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    return out;
}
}  // namespace

int main() {
    if (!generate_cert()) {
        std::cout << "[SKIP] tls_test: could not generate a certificate (openssl missing?)"
                  << std::endl;
        return 0;
    }

    // A larger-than-one-chunk file to exercise the TLS file path (pread +
    // SSL_write, since sendfile cannot encrypt) with partial writes.
    const std::string file_path = kDir + "/data.bin";
    const std::string file_content(100000, 'Z');
    { std::ofstream(file_path, std::ios::binary).write(file_content.data(),
                                                       static_cast<std::streamsize>(file_content.size())); }

    Server::Server server(2);
    server.enable_tls(kCert, kKey, "1.2");
    server.get("/ping", [](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).text("pong");
    });
    server.get("/file", [&file_path](const Http::HttpRequest&, Http::HttpResponse& res) {
        res.status(Http::HttpStatus::OK).file(file_path, "application/octet-stream");
    });

    if (!server.listen(kHost, kPort)) {
        std::cerr << "[FATAL] listen failed" << std::endl;
        return 1;
    }
    std::thread loop([&server] { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Two requests over fresh TLS sessions to exercise the handshake path twice.
    for (int i = 0; i < 2; ++i) {
        std::string r = tls_round_trip(
            "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        check(r.find("200") != std::string::npos, "TLS request returns 200");
        check(r.find("pong") != std::string::npos, "TLS response body delivered");
    }

    // File body over TLS (pread + SSL_write fallback).
    {
        std::string r = tls_round_trip(
            "GET /file HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        size_t body_pos = r.find("\r\n\r\n");
        check(body_pos != std::string::npos, "TLS file response has headers");
        if (body_pos != std::string::npos) {
            std::string body = r.substr(body_pos + 4);
            check(body.size() == file_content.size(), "TLS file body fully delivered");
            check(body == file_content, "TLS file body bytes match");
        }
    }

    server.request_stop();
    loop.join();

    if (g_failures == 0) {
        std::cout << "[OK] all TLS tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
