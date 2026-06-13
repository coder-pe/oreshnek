// oreshnek/src/platform/SecurityUtils.cpp
#include "oreshnek/platform/SecurityUtils.h"
#include "oreshnek/utils/Logger.h"
#include <openssl/crypto.h> // For CRYPTO_memcmp
#include <array>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace Oreshnek {
namespace Platform {

using JsonValue = Oreshnek::Json::JsonValue;
using JsonParser = Oreshnek::Json::JsonParser;

namespace {
constexpr char kB64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
}  // namespace

std::string SecurityUtils::base64url_encode(const std::string& input) {
    std::string encoded;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            encoded.push_back(kB64UrlAlphabet[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        encoded.push_back(kB64UrlAlphabet[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    // base64url omits '=' padding.
    return encoded;
}

std::string SecurityUtils::base64url_decode(const std::string& input) {
    // Build the reverse lookup table once. Index with unsigned char to avoid the
    // signed-char negative-index UB that the previous implementation had.
    static const std::array<int8_t, 256> table = [] {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(kB64UrlAlphabet[i])] = static_cast<int8_t>(i);
        }
        return t;
    }();

    std::string decoded;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break; // tolerate accidental padding
        int8_t d = table[c];
        if (d == -1) break;  // stop at first invalid character
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return decoded;
}

std::string SecurityUtils::hmac_sha256_raw(const std::string& data, const std::string& key) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         out, &out_len);
    return std::string(reinterpret_cast<char*>(out), out_len);
}

bool SecurityUtils::constant_time_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string SecurityUtils::generateSalt() {
    unsigned char salt[kSaltLength];
    RAND_bytes(salt, sizeof(salt));
    std::ostringstream ss;
    for (unsigned char b : salt) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}

std::string SecurityUtils::hashPassword(const std::string& password) {
    unsigned char salt[kSaltLength];
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        throw std::runtime_error("RAND_bytes failed while hashing password");
    }

    unsigned char hash[kHashLength];
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt, sizeof(salt), kPbkdf2Iterations, EVP_sha256(),
                          sizeof(hash), hash) != 1) {
        throw std::runtime_error("PBKDF2 derivation failed");
    }

    // Self-contained PHC-like format: pbkdf2_sha256$<iter>$<salt_b64url>$<hash_b64url>
    std::ostringstream out;
    out << "pbkdf2_sha256$" << kPbkdf2Iterations << '$'
        << base64url_encode(std::string(reinterpret_cast<char*>(salt), sizeof(salt))) << '$'
        << base64url_encode(std::string(reinterpret_cast<char*>(hash), sizeof(hash)));
    return out.str();
}

bool SecurityUtils::verifyPassword(const std::string& password, const std::string& stored) {
    std::vector<std::string> parts = split(stored, '$');
    if (parts.size() != 4 || parts[0] != "pbkdf2_sha256") {
        return false;
    }

    int iterations = 0;
    try {
        iterations = std::stoi(parts[1]);
    } catch (const std::exception&) {
        return false;
    }
    if (iterations <= 0) return false;

    std::string salt = base64url_decode(parts[2]);
    std::string expected = base64url_decode(parts[3]);
    if (salt.empty() || expected.empty()) return false;

    std::vector<unsigned char> computed(expected.size());
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()),
                          static_cast<int>(salt.size()), iterations, EVP_sha256(),
                          static_cast<int>(computed.size()), computed.data()) != 1) {
        return false;
    }

    return constant_time_equals(
        expected, std::string(reinterpret_cast<char*>(computed.data()), computed.size()));
}

std::string SecurityUtils::generateJWT(int user_id, const std::string& username,
                                       const std::string& secret) {
    JsonValue header = JsonValue::object();
    header["alg"] = JsonValue("HS256");
    header["typ"] = JsonValue("JWT");

    JsonValue payload = JsonValue::object();
    payload["user_id"] = JsonValue(user_id);
    payload["username"] = JsonValue(username);
    payload["exp"] = JsonValue(static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + 24 * 3600));

    std::string signing_input =
        base64url_encode(header.dump()) + "." + base64url_encode(payload.dump());
    std::string signature = base64url_encode(hmac_sha256_raw(signing_input, secret));
    return signing_input + "." + signature;
}

bool SecurityUtils::validateJWT(const std::string& token, const std::string& secret) {
    std::vector<std::string> parts = split(token, '.');
    if (parts.size() != 3) return false;

    // 1) Signature check (constant time) over "header.payload".
    std::string expected_sig = hmac_sha256_raw(parts[0] + "." + parts[1], secret);
    std::string actual_sig = base64url_decode(parts[2]);
    if (!constant_time_equals(actual_sig, expected_sig)) return false;

    try {
        // 2) Algorithm check: reject "none" / algorithm-confusion attacks.
        JsonValue header = JsonParser::parse(base64url_decode(parts[0]));
        if (!header.is_object() || !header.contains("alg") || !header["alg"].is_string() ||
            header["alg"].get<std::string>() != "HS256") {
            return false;
        }

        // 3) Expiration check (exp is mandatory).
        JsonValue payload = JsonParser::parse(base64url_decode(parts[1]));
        if (!payload.is_object() || !payload.contains("exp") || !payload["exp"].is_number()) {
            return false;
        }
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        if (payload["exp"].get<long long>() <= now) {
            return false; // Expired.
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

JsonValue SecurityUtils::decodeJWT(const std::string& token) {
    std::vector<std::string> parts = split(token, '.');
    if (parts.size() < 2) {
        return JsonValue();
    }
    try {
        return JsonParser::parse(base64url_decode(parts[1]));
    } catch (const std::exception& e) {
        ORE_LOG(WARN) << "Error decoding JWT payload: " << e.what();
        return JsonValue();
    }
}

std::vector<std::string> SecurityUtils::split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream stream(str);
    while (std::getline(stream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace Platform
} // namespace Oreshnek
