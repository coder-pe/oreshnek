// tests/security_test.cpp
//
// Unit tests for the Fase 2 security primitives: PBKDF2 password hashing and
// HS256 JWT issuance/validation.

#include "oreshnek/platform/SecurityUtils.h"
#include "oreshnek/json/JsonValue.h"

#include <iostream>
#include <string>

using Oreshnek::Platform::SecurityUtils;

namespace {
int g_failures = 0;
void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        ++g_failures;
    }
}
}  // namespace

int main() {
    // --- Password hashing (PBKDF2 + per-user random salt) --------------------
    const std::string pw = "s3cret-passw0rd!";
    std::string hash = SecurityUtils::hashPassword(pw);
    check(hash.rfind("pbkdf2_sha256$", 0) == 0, "hash carries algorithm prefix");
    check(SecurityUtils::verifyPassword(pw, hash), "correct password verifies");
    check(!SecurityUtils::verifyPassword("wrong", hash), "wrong password rejected");
    check(!SecurityUtils::verifyPassword(pw, "not-a-valid-hash"), "malformed stored hash rejected");
    check(!SecurityUtils::verifyPassword(pw, ""), "empty stored hash rejected");

    std::string hash2 = SecurityUtils::hashPassword(pw);
    check(hash != hash2, "same password yields different hash (random salt)");
    check(SecurityUtils::verifyPassword(pw, hash2), "second independent hash verifies");

    // --- JWT (HS256) ---------------------------------------------------------
    const std::string secret = "unit-test-secret";
    std::string token = SecurityUtils::generateJWT(42, "alice", secret);

    check(SecurityUtils::validateJWT(token, secret), "freshly issued token is valid");
    check(!SecurityUtils::validateJWT(token, "other-secret"), "wrong secret rejected");

    // Tamper with the signature segment (after the last '.').
    std::string tampered = token;
    size_t sig_pos = tampered.rfind('.') + 1;
    tampered[sig_pos] = (tampered[sig_pos] == 'A') ? 'B' : 'A';
    check(!SecurityUtils::validateJWT(tampered, secret), "tampered signature rejected");

    // Tamper with the payload segment (claims) -> signature no longer matches.
    std::string payload_tampered = token;
    size_t first_dot = payload_tampered.find('.') + 1;
    payload_tampered[first_dot] = (payload_tampered[first_dot] == 'A') ? 'B' : 'A';
    check(!SecurityUtils::validateJWT(payload_tampered, secret), "tampered payload rejected");

    check(!SecurityUtils::validateJWT("not.a.jwt", secret), "garbage token rejected");
    check(!SecurityUtils::validateJWT("onlyonepart", secret), "single-segment token rejected");

    // Decode claims (only meaningful after validation succeeds).
    auto payload = SecurityUtils::decodeJWT(token);
    check(payload.is_object(), "payload decodes to a JSON object");
    if (payload.is_object()) {
        const auto& obj = payload.get_object();
        check(obj.count("user_id") && static_cast<int>(obj.at("user_id").get_number()) == 42,
              "user_id claim == 42");
        check(obj.count("username") && obj.at("username").get_string() == "alice",
              "username claim == alice");
        check(obj.count("exp"), "exp claim present");
    }

    if (g_failures == 0) {
        std::cout << "[OK] all security tests passed" << std::endl;
        return 0;
    }
    std::cerr << "[FAILED] " << g_failures << " check(s) failed" << std::endl;
    return 1;
}
