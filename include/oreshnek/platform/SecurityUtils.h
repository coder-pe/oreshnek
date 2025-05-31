// oreshnek/include/oreshnek/platform/SecurityUtils.h
#ifndef ORESHNEK_PLATFORM_SECURITY_UTILS_H
#define ORESHNEK_PLATFORM_SECURITY_UTILS_H

#include <string>
#include <vector>
#include <sstream>
#include <iomanip> // For std::setw, std::setfill
#include <chrono>  // For JWT expiration
#include <openssl/sha.h> // For SHA256_DIGEST_LENGTH, SHA256_CTX, SHA256_Init, SHA256_Update, SHA256_Final
#include <openssl/rand.h> // For RAND_bytes
#include <openssl/evp.h> // For EVP_sha256 (HMAC is in hmac.h)
#include <openssl/hmac.h> // <<< ADD THIS LINE for HMAC
#include <nlohmann/json.hpp> // For JSON Web Token

namespace Oreshnek {
namespace Platform {

class SecurityUtils {
public:
    static std::string hashPassword(const std::string& password, const std::string& salt); //
    static std::string generateSalt(); //
    static std::string generateJWT(int user_id, const std::string& username, const std::string& secret); //
    static bool validateJWT(const std::string& token, const std::string& secret); //
    static nlohmann::json decodeJWT(const std::string& token);

private:
    static std::string base64_encode(const std::string& input); //
    static std::string hmac_sha256(const std::string& data, const std::string& key); //
    static std::vector<std::string> split(const std::string& str, char delimiter); //
};

} // namespace Platform
} // namespace Oreshnek

#endif // ORESHNEK_PLATFORM_SECURITY_UTILS_H
