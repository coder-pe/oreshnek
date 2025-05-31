#include "oreshnek/platform/SecurityUtils.h"
#include <algorithm> // For std::replace

namespace Oreshnek {
namespace Platform {

using json = nlohmann::json;

std::string SecurityUtils::hashPassword(const std::string& password, const std::string& salt) {
    std::string salted = password + salt; // [cite: 132]
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, salted.c_str(), salted.size());
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss; // [cite: 133]
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i]; // [cite: 134]
    }
    return ss.str(); // [cite: 135]
}

std::string SecurityUtils::generateSalt() {
    unsigned char salt[16];
    RAND_bytes(salt, sizeof(salt)); // [cite: 136]
    std::stringstream ss; // [cite: 137]
    for(int i = 0; i < 16; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)salt[i]; // [cite: 137]
    }
    return ss.str(); // [cite: 138]
}

std::string SecurityUtils::generateJWT(int user_id, const std::string& username, const std::string& secret) {
    json header = {{"alg", "HS256"}, {"typ", "JWT"}}; // [cite: 139]
    json payload = {
        {"user_id", user_id},
        {"username", username},
        {"exp", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 24*3600}
    }; // [cite: 140]

    std::string header_str = base64_encode(header.dump());
    std::string payload_str = base64_encode(payload.dump());
    
    std::string signature = hmac_sha256(header_str + "." + payload_str, secret); // [cite: 141]
    return header_str + "." + payload_str + "." + signature; // [cite: 142]
}

bool SecurityUtils::validateJWT(const std::string& token, const std::string& secret) {
    std::vector<std::string> parts = split(token, '.'); // [cite: 143]
    if(parts.size() != 3) return false;
    
    std::string expected_signature = hmac_sha256(parts[0] + "." + parts[1], secret);
    return parts[2] == expected_signature; // [cite: 144]
}

json SecurityUtils::decodeJWT(const std::string& token) {
    std::vector<std::string> parts = split(token, '.');
    if (parts.size() < 2) {
        return nullptr; // Invalid JWT format
    }
    try {
        std::string decoded_payload = base64_decode(parts[1]); // Assuming base64_decode is available
        return json::parse(decoded_payload);
    } catch (const std::exception& e) {
        std::cerr << "Error decoding JWT payload: " << e.what() << std::endl;
        return nullptr;
    }
}

// Private helper functions for base64 and hmac_sha256
std::string SecurityUtils::base64_encode(const std::string& input) {
    // This is a simplified base64_encode. For production, use a more robust library.
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; // [cite: 145]
    std::string encoded;
    int val = 0, valb = -6;
    for(unsigned char c : input) {
        val = (val << 8) + c; // [cite: 146]
        valb += 8;
        while(valb >= 0) {
            encoded.push_back(chars[(val >> valb) & 0x3F]); // [cite: 147]
            valb -= 6;
        }
    }
    if(valb > -6) encoded.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]); // [cite: 148]
    while(encoded.size() % 4) encoded.push_back('='); // [cite: 148]
    return encoded;
}

// You will need a base64_decode for decodeJWT. Here's a basic one:
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
    unsigned char* digest; // [cite: 149]
    unsigned int len = SHA256_DIGEST_LENGTH; // Correct length for SHA256
    
    digest = HMAC(EVP_sha256(), key.c_str(), key.length(), 
                 reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), NULL, &len); // [cite: 150]
    
    std::stringstream ss; // [cite: 151]
    for(unsigned int i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]); // [cite: 151]
    }
    return ss.str(); // [cite: 152]
}

std::vector<std::string> SecurityUtils::split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens; // [cite: 153]
    std::string token;
    std::istringstream tokenStream(str);
    while(std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token); // [cite: 154]
    }
    return tokens;
}

} // namespace Platform
} // namespace Oreshnek
