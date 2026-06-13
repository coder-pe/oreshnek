// oreshnek/include/oreshnek/platform/SecurityUtils.h
#ifndef ORESHNEK_PLATFORM_SECURITY_UTILS_H
#define ORESHNEK_PLATFORM_SECURITY_UTILS_H

#include <string>
#include <vector>
#include <optional>
#include <chrono>  // For JWT expiration
#include <openssl/rand.h> // For RAND_bytes
#include <openssl/evp.h>  // For EVP_sha256, PKCS5_PBKDF2_HMAC
#include <openssl/hmac.h> // For HMAC
#include "oreshnek/json/JsonValue.h" // Use project's JSON implementation
#include "oreshnek/json/JsonParser.h" // For JSON parsing

namespace Oreshnek {
namespace Platform {

class SecurityUtils {
public:
    // Derives a password hash using PBKDF2-HMAC-SHA256 with a freshly generated
    // random per-call salt. The returned string is self-contained (encodes the
    // algorithm, iteration count, salt and hash), so it can be stored directly
    // in the users table and later checked with verifyPassword().
    static std::string hashPassword(const std::string& password);

    // Constant-time verification of a password against a string produced by
    // hashPassword(). Returns false on any malformed input.
    static bool verifyPassword(const std::string& password, const std::string& stored);

    // Random salt as a hex string (kept for callers that manage their own salt).
    static std::string generateSalt();

    // Issues an HS256 JWT (header.payload.signature, all base64url, no padding).
    static std::string generateJWT(int user_id, const std::string& username, const std::string& secret);

    // Fully validates a token: signature (constant-time), alg == HS256
    // (rejects "none"/algorithm confusion) and the exp claim. Returns true only
    // if every check passes.
    static bool validateJWT(const std::string& token, const std::string& secret);

    // Decodes the payload of a token. Only meaningful after validateJWT() has
    // returned true for the same token+secret. Returns a null JsonValue on error.
    static Oreshnek::Json::JsonValue decodeJWT(const std::string& token);

private:
    static std::string base64url_encode(const std::string& input);
    static std::string base64url_decode(const std::string& input);
    static std::string hmac_sha256_raw(const std::string& data, const std::string& key);
    static bool constant_time_equals(const std::string& a, const std::string& b);
    static std::vector<std::string> split(const std::string& str, char delimiter);

    static constexpr int kPbkdf2Iterations = 200000;
    static constexpr int kSaltLength = 16;
    static constexpr int kHashLength = 32; // SHA-256 output
};

} // namespace Platform
} // namespace Oreshnek

#endif // ORESHNEK_PLATFORM_SECURITY_UTILS_H
