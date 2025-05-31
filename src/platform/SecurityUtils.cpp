// oreshnek/src/platform/SecurityUtils.cpp
#include "oreshnek/platform/SecurityUtils.h"
#include <algorithm> // For std::replace
#include <iostream>  // Required for std::cerr [cite: 9]

// Add this declaration for base64_decode.
// In a real project, this would typically be in a dedicated header or part of a base64 utility class/namespace.
// For now, we'll declare it here to resolve the compilation error.
namespace Oreshnek {
namespace Platform {
    std::string base64_decode(const std::string& input);
}
}

namespace Oreshnek {
namespace Platform {

using json = nlohmann::json;

std::string SecurityUtils::hashPassword(const std::string& password, const std::string& salt) {
    std::string salted = password + salt;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, salted.c_str(), salted.size());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

std::string SecurityUtils::generateSalt() {
    unsigned char salt[16];
    RAND_bytes(salt, sizeof(salt));
    std::stringstream ss;
    for(int i = 0; i < 16; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)salt[i];
    }
    return ss.str();
}

std::string SecurityUtils::generateJWT(int user_id, const std::string& username, const std::string& secret) {
    json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    json payload = {
        {"user_id", user_id},
        {"username", username},
        {"exp", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 24*3600}
    };

    std::string header_str = base64_encode(header.dump());
    std::string payload_str = base64_encode(payload.dump());
    
    std::string signature = hmac_sha256(header_str + "." + payload_str, secret);
    return header_str + "." + payload_str + "." + signature;
}

bool SecurityUtils::validateJWT(const std::string& token, const std::string& secret) {
    std::vector<std::string> parts = split(token, '.');
    if(parts.size() != 3) return false;
    
    std::string expected_signature = hmac_sha256(parts[0] + "." + parts[1], secret);
    return parts[2] == expected_signature;
}

json SecurityUtils::decodeJWT(const std::string& token) {
    std::vector<std::string> parts = split(token, '.');
    if (parts.size() < 2) {
        return nullptr; // Invalid JWT format
    }
    try {
        std::string decoded_payload = base64_decode(parts[1]); // Calls the newly declared base64_decode
        return json::parse(decoded_payload);
    } catch (const std::exception& e) {
        std::cerr << "Error decoding JWT payload: " << e.what() << std::endl;
        return nullptr;
    }
}

// Private helper functions for base64 and hmac_sha256
std::string SecurityUtils::base64_encode(const std::string& input) {
    // This is a simplified base64_encode. For production, use a more robust library.
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    int val = 0, valb = -6;
    for(unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while(valb >= 0) {
            encoded.push_back(chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if(valb > -6) encoded.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while(encoded.size() % 4) encoded.push_back('=');
    return encoded;
}

// You will need a base64_decode for decodeJWT. Here's a basic one:
// This function needs to be defined in the Oreshnek::Platform namespace
std::string base64_decode(const std::string& input) {
    // This is a simplified base64_decode. For production, use a more robust library.
    // This implementation is a placeholder and may not handle all edge cases or padding correctly.
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    std::string decoded;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[chars[i]] = i;

    int val = 0, valb = -8;
    for (char c : input) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return decoded;
}


std::string SecurityUtils::hmac_sha256(const std::string& data, const std::string& key) {
    unsigned char* digest;
    unsigned int len = SHA256_DIGEST_LENGTH; // Correct length for SHA256
    
    // HMAC is deprecated in OpenSSL 3.0, but for now we'll cast to suppress the error.
    // A proper solution would involve updating to the new OpenSSL 3.0 API if available,
    // or using a different hashing library.
    digest = HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.length()), 
                 reinterpret_cast<const unsigned char*>(data.c_str()), static_cast<int>(data.length()), NULL, &len);
    
    std::stringstream ss;
    for(unsigned int i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return ss.str();
}

std::vector<std::string> SecurityUtils::split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while(std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace Platform
} // namespace Oreshnek
