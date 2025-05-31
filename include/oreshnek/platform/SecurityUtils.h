#ifndef ORESHNEK_PLATFORM_SECURITY_UTILS_H
#define ORESHNEK_PLATFORM_SECURITY_UTILS_H

#include <string>
#include <vector>
#include <sstream>
#include <iomanip> // For std::setw, std::setfill
#include <chrono>  // For JWT expiration
#include <openssl/sha.h> // For SHA256_DIGEST_LENGTH, SHA256_CTX, SHA256_Init, SHA256_Update, SHA256_Final [cite: 132]
#include <openssl/rand.h> // For RAND_bytes [cite: 136]
#include <openssl/evp.h> // For HMAC, EVP_sha256 [cite: 149]
#include <nlohmann/json.hpp> // For JSON Web Token [cite: 139]

namespace Oreshnek {
namespace Platform {

class SecurityUtils {
public:
    static std::string hashPassword(const std::string& password, const std::string& salt); // [cite: 132]
    static std::string generateSalt(); // [cite: 136]
    static std::string generateJWT(int user_id, const std::string& username, const std::string& secret); // [cite: 139]
    static bool validateJWT(const std::string& token, const std::string& secret); // [cite: 143]
    static nlohmann::json decodeJWT(const std::string& token); // To extract payload without full validation

private:
    static std::string base64_encode(const std::string& input); // [cite: 145]
    static std::string hmac_sha256(const std::string& data, const std::string& key); // [cite: 149]
    static std::vector<std::string> split(const std::string& str, char delimiter); // [cite: 153]
};

} // namespace Platform
} // namespace Oreshnek

#endif // ORESHNEK_PLATFORM_SECURITY_UTILS_H
