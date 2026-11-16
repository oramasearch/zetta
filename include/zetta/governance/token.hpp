// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::governance {

// What kind of credential a token is. Both authenticate AS a user and are stored
// hashed; the type is governance context for how it was issued:
//   access — a pre-assigned long-lived secret (the bootstrap owner's credential)
//   api    — an issued, revocable programmatic token
// The canonical lowercase name is BOTH the display label AND the stored form.
enum class TokenType { Api, Access };

std::string_view to_string(TokenType type);
std::optional<TokenType> token_type_from(std::string_view name);

// The at-rest value of a token: everything replicated + persisted. The secret
// plaintext is NEVER here — only its salted hash. Parsed at the trust boundary
// by token_value_from_json.
struct TokenValue {
    std::string user_id;      // the user this token authenticates as
    TokenType type = TokenType::Api;
    std::string hash;         // hex SHA-256(salt || secret), or the bootstrap
                              // owner's `pbkdf2-sha256$<iterations>$<hex>` (#769)
    std::string salt;         // hex per-token salt
    std::string created;      // ISO-8601 UTC
    std::string expires_at;   // optional ISO-8601 UTC; empty = never expires
    bool revoked = false;     // a revoked token never validates

    bool operator==(const TokenValue&) const = default;
};

// Parse a TokenValue from JSON at the trust boundary — std::nullopt on anything
// malformed (not an object, missing/wrong-typed field, unknown type) so a
// corrupt token.json / CRDT value is skipped rather than trusted.
std::optional<TokenValue> token_value_from_json(const nlohmann::json& document);

// The Token JSON Schema (draft 2020-12) — the code-owned source of truth for the
// token.json / replicated `token:<uuid>` value shape; schemas/token.schema.json
// is generated from it and drift-checked by tests/test_schemas.cpp.
const nlohmann::json& token_schema();

// A Token (FEAT-9 / #744): a bearer credential that authenticates as a user,
// stored **hashed at rest** (the plaintext secret is shown once at creation and
// never persisted). **Replicated state** (the Principal capture/reconcile path):
// the CRDT document is the source of truth (`token:<uuid>`), and this is its
// in-memory + on-disk projection under a UUID-named folder with token.json. The
// id is the folder name and never changes.
class Token {
public:
    Token(std::string id, TokenValue value, std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& user_id() const { return value_.user_id; }
    TokenType type() const { return value_.type; }
    const std::string& created() const { return value_.created; }
    const std::string& expires_at() const { return value_.expires_at; }
    bool revoked() const { return value_.revoked; }
    const TokenValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Has this token's TTL elapsed as of `now` (an ISO-8601 UTC stamp)? Always
    // false when there is no expiry. Same lexicographic-compare contract as a
    // Capability TTL.
    bool expired(std::string_view now) const;

    // Does `secret` match this token, and is the token live (not revoked, not
    // expired) as of `now`? Recomputes the stored hash's form (SHA-256(salt ||
    // secret), or the PBKDF2 form for the bootstrap owner, #769) and compares it
    // in **constant time**. The whole point of hashing: a stolen store can't
    // recover the secret, and validation can't be timed.
    //
    // A KDF token costs a full PBKDF2 derivation to check, so after the first
    // successful check it keeps a memory-only SHA-256(salt || secret) verifier
    // and later checks compare against that. With `allow_kdf` false, a KDF token
    // that has not been verified yet in this process answers false without
    // deriving: Node::authenticate_token uses it to try every cheap match first
    // and to budget the slow derivations.
    bool validates(std::string_view secret, std::string_view now,
                   bool allow_kdf = true) const;

    // Is the stored hash in the KDF form (the bootstrap owner's token, #769)?
    bool uses_kdf() const;
    // Has this KDF token been verified once in this process (so checking it is
    // cheap)? Always false for a non-KDF token. Memory-only, never persisted.
    bool kdf_verified() const { return !kdf_verifier_.empty(); }

    // Replace the mutable value to match a (replicated) one, then rewrite
    // token.json (used by reconcile — e.g. a peer revoked it).
    void set_value(TokenValue value);

    // The replicated/on-disk value shape (the id is the CRDT key / folder name).
    nlohmann::json metadata() const;

    // Write this token's metadata (id + fields) to dir/token.json.
    void persist_meta() const;

private:
    std::string id_;
    TokenValue value_;
    std::filesystem::path dir_;
    // Memory-only SHA-256(salt || secret) of a KDF token's secret, set by the
    // first successful validates(); cleared when set_value changes the hash.
    mutable std::string kdf_verifier_;
};

}  // namespace zetta::governance
