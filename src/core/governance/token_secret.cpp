#include "zetta/governance/token_secret.hpp"

#include <array>
#include <cstddef>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

// The governance crypto adapter: the second TU (after
// src/core/cluster/security.cpp) allowed to name OpenSSL libcrypto directly.
// It exists so token secrets are hashed with a vetted digest and compared in
// constant time, rather than through the self-contained (non-constant-time,
// "never for secrets") zetta::util::sha256_hex used for content fingerprints.
namespace zetta::governance {
namespace {

std::string to_hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        hex.push_back(kDigits[bytes[i] >> 4]);
        hex.push_back(kDigits[bytes[i] & 0x0f]);
    }
    return hex;
}

// `count` bytes from the OS CSPRNG as lowercase hex; empty on RNG failure.
std::string random_hex(std::size_t count) {
    std::string bytes(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
                   static_cast<int>(count)) != 1) {
        return {};
    }
    return to_hex(reinterpret_cast<const unsigned char*>(bytes.data()), count);
}

}  // namespace

std::string generate_token_secret() { return random_hex(32); }

std::string generate_token_salt() { return random_hex(16); }

std::string hash_token_secret(std::string_view salt, std::string_view secret) {
    // SHA-256(salt || secret): the salt is domain separation so identical
    // secrets hash differently, and the secret is already 256 bits of entropy,
    // so a single vetted digest is sufficient at rest (no work-factor needed for
    // a random secret — unlike a human password).
    std::array<unsigned char, 32> digest{};
    unsigned int written = 0;
    std::string input;
    input.reserve(salt.size() + secret.size());
    input.append(salt);
    input.append(secret);
    if (EVP_Digest(input.data(), input.size(), digest.data(), &written,
                   EVP_sha256(), nullptr) != 1) {
        return {};
    }
    return to_hex(digest.data(), digest.size());
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace zetta::governance
