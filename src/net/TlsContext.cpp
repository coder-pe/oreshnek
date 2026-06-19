// oreshnek/src/net/TlsContext.cpp
#include "oreshnek/net/TlsContext.h"
#include "oreshnek/utils/Logger.h"

#include <openssl/err.h>

#include <stdexcept>
#include <string>

namespace Oreshnek {
namespace Net {

namespace {
std::string openssl_error() {
    unsigned long e = ERR_get_error();
    if (e == 0) return "unknown error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    return buf;
}
}  // namespace

TlsContext::TlsContext(const std::string& cert_file, const std::string& key_file,
                       const std::string& min_version) {
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == nullptr) {
        throw std::runtime_error("SSL_CTX_new failed: " + openssl_error());
    }

    int min = TLS1_2_VERSION;
    if (min_version == "1.3") min = TLS1_3_VERSION;
    SSL_CTX_set_min_proto_version(ctx_, min);

    // No legacy compression; prefer the server's cipher ordering. Partial writes
    // + moving write buffer let our offset-based write loop advance after a
    // short SSL_write instead of having to re-present the exact same buffer.
    SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_file.c_str()) != 1) {
        std::string err = openssl_error();
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("Failed to load TLS certificate '" + cert_file + "': " + err);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        std::string err = openssl_error();
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("Failed to load TLS private key '" + key_file + "': " + err);
    }
    if (SSL_CTX_check_private_key(ctx_) != 1) {
        std::string err = openssl_error();
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        throw std::runtime_error("TLS private key does not match certificate: " + err);
    }

    ORE_LOG(INFO) << "TLS enabled (cert=" << cert_file << ", min TLS "
                  << (min == TLS1_3_VERSION ? "1.3" : "1.2") << ")";
}

TlsContext::~TlsContext() {
    if (ctx_ != nullptr) SSL_CTX_free(ctx_);
}

SSL* TlsContext::new_session(int fd) const {
    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr) {
        ORE_LOG(ERROR) << "SSL_new failed: " << openssl_error();
        return nullptr;
    }
    if (SSL_set_fd(ssl, fd) != 1) {
        ORE_LOG(ERROR) << "SSL_set_fd failed: " << openssl_error();
        SSL_free(ssl);
        return nullptr;
    }
    SSL_set_accept_state(ssl); // server-side handshake
    return ssl;
}

}  // namespace Net
}  // namespace Oreshnek
