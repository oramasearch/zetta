#pragma once

#include <string>
#include <string_view>

namespace zetta::governance {

// Token-secret cryptography (FEAT-9 / #744) — the one place a token's plaintext
// secret is generated, hashed for at-rest storage, and verified. A token secret
// is a high-entropy random value shown to the operator exactly once at creation;
// only its salted hash is ever persisted (`token:<uuid>` / token.json), so the
// store leaks nothing a stolen document could replay.
//
// These helpers name OpenSSL libcrypto directly (RAND_bytes for the CSPRNG,
// EVP SHA-256 for the digest, PKCS5_PBKDF2_HMAC for the bootstrap owner's
// operator-chosen secret, CRYPTO_memcmp for the constant-time compare) — the
// second such TU after src/core/cluster/security.cpp; see src/core/CLAUDE.md.
// Everything is lowercase hex so the values round-trip as plain JSON strings and
// sort/compare as text.

// A fresh token secret: 32 bytes from the OS CSPRNG, lowercase hex (64 chars).
// This is the PLAINTEXT returned once at token creation — never stored.
std::string generate_token_secret();

// A fresh per-token salt: 16 bytes from the OS CSPRNG, lowercase hex (32 chars).
// Stored alongside the hash so two tokens with the same secret hash differently.
std::string generate_token_salt();

// The at-rest token hash: lowercase-hex SHA-256 of (salt || secret). Deterministic
// — the same (salt, secret) always yields the same 64-char digest, which is what
// validation recomputes and compares. Returns empty only on an OpenSSL failure.
std::string hash_token_secret(std::string_view salt, std::string_view secret);

// The key-derivation function for a token whose secret may be chosen by a person
// (#769): the bootstrap owner's --admin-token / ZETTA_ADMIN_TOKEN. A generated
// secret carries 256 bits of entropy, so one SHA-256 is enough at rest; an
// operator-chosen one may not, and a stolen store must not let it be guessed
// offline at SHA-256 speed. PBKDF2-HMAC-SHA256 (OpenSSL PKCS5_PBKDF2_HMAC) with
// kTokenKdfIterations rounds, the OWASP figure for this function.
//
// The stored form is self-describing so the existing `hash` field carries it with
// no schema change: `pbkdf2-sha256$<iterations>$<64 hex>`. A plain 64-hex hash is
// the single-SHA-256 form; is_kdf_token_hash tells them apart.
inline constexpr unsigned kTokenKdfIterations = 600000;
inline constexpr std::string_view kTokenKdfPrefix = "pbkdf2-sha256$";

// Derive the stored KDF form for (salt, secret). Empty only on an OpenSSL failure.
std::string kdf_hash_token_secret(std::string_view salt, std::string_view secret,
                                  unsigned iterations = kTokenKdfIterations);

// Is `stored_hash` in the KDF form (`pbkdf2-sha256$...`)?
bool is_kdf_token_hash(std::string_view stored_hash);

// Does `secret` match `stored_hash` (either form) under `salt`? Recomputes the
// hash the stored form names and compares in constant time. A malformed KDF form
// (bad iteration count, wrong digest length) never matches.
bool verify_token_secret(std::string_view stored_hash, std::string_view salt,
                         std::string_view secret);

// Constant-time equality of two byte strings (typically two hex hashes of equal
// length). Avoids a timing oracle that a plain `==` early-out would leak while
// comparing a candidate hash against the stored one. Unequal lengths are an
// immediate (safe) false.
bool constant_time_equal(std::string_view a, std::string_view b);

}  // namespace zetta::governance
