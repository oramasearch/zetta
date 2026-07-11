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
// EVP SHA-256 for the digest, CRYPTO_memcmp for the constant-time compare) — the
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

// Constant-time equality of two byte strings (typically two hex hashes of equal
// length). Avoids a timing oracle that a plain `==` early-out would leak while
// comparing a candidate hash against the stored one. Unequal lengths are an
// immediate (safe) false.
bool constant_time_equal(std::string_view a, std::string_view b);

}  // namespace zetta::governance
