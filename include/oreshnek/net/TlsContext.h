// oreshnek/include/oreshnek/net/TlsContext.h
#ifndef ORESHNEK_NET_TLS_CONTEXT_H
#define ORESHNEK_NET_TLS_CONTEXT_H

#include <openssl/ssl.h>
#include <string>

namespace Oreshnek {
namespace Net {

// Owns an OpenSSL SSL_CTX configured as a TLS server: loads the certificate
// chain and private key, sets the minimum protocol version and sane options.
// Shared (read-only after construction) across all connections; OpenSSL makes
// SSL_CTX safe to use concurrently to create per-connection SSL objects.
class TlsContext {
public:
    // Throws std::runtime_error if the context cannot be created or the
    // certificate/key cannot be loaded or do not match.
    TlsContext(const std::string& cert_file, const std::string& key_file,
               const std::string& min_version);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* get() const { return ctx_; }

    // Create a new server-side SSL object bound to `fd`, ready to drive a
    // non-blocking handshake (accept state). Returns nullptr on failure.
    SSL* new_session(int fd) const;

private:
    SSL_CTX* ctx_ = nullptr;
};

}  // namespace Net
}  // namespace Oreshnek

#endif  // ORESHNEK_NET_TLS_CONTEXT_H
