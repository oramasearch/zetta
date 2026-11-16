// SPDX-License-Identifier: MIT

#include "zetta/governance/token.hpp"

#include <fstream>
#include <utility>

#include "zetta/governance/token_secret.hpp"

namespace zetta::governance {
namespace fs = std::filesystem;

std::string_view to_string(TokenType type) {
    switch (type) {
        case TokenType::Api:    return "api";
        case TokenType::Access: return "access";
    }
    return "unknown";
}

std::optional<TokenType> token_type_from(std::string_view name) {
    if (name == "api") return TokenType::Api;
    if (name == "access") return TokenType::Access;
    return std::nullopt;
}

std::optional<TokenValue> token_value_from_json(const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    // Reject an unknown top-level field (additionalProperties:false), matching
    // the schema — a record carrying an unexpected key is malformed.
    for (const auto& [field, ignored] : document.items()) {
        if (field != "id" && field != "user" && field != "type" &&
            field != "hash" && field != "salt" && field != "created" &&
            field != "expires_at" && field != "revoked") {
            return std::nullopt;
        }
    }
    if (!document.contains("user") || !document["user"].is_string() ||
        !document.contains("type") || !document["type"].is_string() ||
        !document.contains("hash") || !document["hash"].is_string() ||
        !document.contains("salt") || !document["salt"].is_string()) {
        return std::nullopt;
    }
    const auto type = token_type_from(document["type"].get<std::string>());
    if (!type) return std::nullopt;
    TokenValue value;
    value.user_id = document["user"].get<std::string>();
    value.type = *type;
    value.hash = document["hash"].get<std::string>();
    value.salt = document["salt"].get<std::string>();
    value.created = document.value("created", std::string{});
    value.expires_at = document.value("expires_at", std::string{});
    if (document.contains("revoked")) {
        if (!document["revoked"].is_boolean()) return std::nullopt;
        value.revoked = document["revoked"].get<bool>();
    }
    return value;
}

Token::Token(std::string id, TokenValue value, fs::path dir)
    : id_(std::move(id)), value_(std::move(value)), dir_(std::move(dir)) {}

bool Token::expired(std::string_view now) const {
    // No TTL => never expires. Fixed-width ISO-8601 => lexicographic compare.
    if (value_.expires_at.empty()) return false;
    return now >= value_.expires_at;
}

bool Token::validates(std::string_view secret, std::string_view now,
                      bool allow_kdf) const {
    if (value_.revoked || expired(now)) return false;
    if (!uses_kdf()) return verify_token_secret(value_.hash, value_.salt, secret);
    if (!kdf_verifier_.empty()) {
        // Verified once already: compare against the memory-only verifier, so a
        // wrong secret never pays for a derivation once the owner has signed in.
        const std::string candidate = hash_token_secret(value_.salt, secret);
        if (candidate.empty()) return false;
        return constant_time_equal(candidate, kdf_verifier_);
    }
    if (!allow_kdf) return false;
    if (!verify_token_secret(value_.hash, value_.salt, secret)) return false;
    kdf_verifier_ = hash_token_secret(value_.salt, secret);
    return true;
}

bool Token::uses_kdf() const { return is_kdf_token_hash(value_.hash); }

void Token::set_value(TokenValue value) {
    if (value.hash != value_.hash || value.salt != value_.salt) {
        kdf_verifier_.clear();  // a new secret: the old verifier no longer applies
    }
    value_ = std::move(value);
    persist_meta();
}

nlohmann::json Token::metadata() const {
    nlohmann::json meta{{"user", value_.user_id},
                        {"type", std::string(to_string(value_.type))},
                        {"hash", value_.hash},
                        {"salt", value_.salt},
                        {"created", value_.created},
                        {"revoked", value_.revoked}};
    if (!value_.expires_at.empty()) meta["expires_at"] = value_.expires_at;
    return meta;
}

void Token::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "token.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

const nlohmann::json& token_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/token.schema.json",
  "title": "Token",
  "description": "A bearer credential (FEAT-9 / #744) that authenticates as a user, stored HASHED at rest — the plaintext secret is shown once at creation and never persisted. The on-disk token.json projection of the replicated `token:<uuid>` CRDT value. Mirrored by zetta::governance::Token in include/zetta/governance/token.hpp and enforced by token_value_from_json() + Node::load_tokens()/reconcile_tokens() — keep them in sync. `hash` = hex SHA-256(salt || secret), or for the bootstrap owner's operator-chosen secret the PBKDF2 form `pbkdf2-sha256$<iterations>$<hex>` (#769); the secret itself never appears here or in system.tokens.",
  "type": "object",
  "required": ["user", "type", "hash", "salt"],
  "additionalProperties": false,
  "properties": {
    "id": {
      "description": "The token's UUID — its CRDT key and on-disk folder name. Present in token.json and omitted from the CRDT value (the key carries it).",
      "type": "string"
    },
    "user": {
      "description": "The id of the User this token authenticates as.",
      "type": "string"
    },
    "type": {
      "description": "How the token was issued: a pre-assigned long-lived 'access' credential, or an issued revocable 'api' token.",
      "type": "string",
      "enum": ["api", "access"]
    },
    "hash": {
      "description": "The at-rest hash of the secret: hex SHA-256(salt || secret) for a generated secret, or `pbkdf2-sha256$<iterations>$<hex>` (PBKDF2-HMAC-SHA256 over the salt) for the bootstrap owner's operator-chosen secret (#769). The plaintext is never stored.",
      "type": "string"
    },
    "salt": {
      "description": "Hex per-token salt mixed into the hash so identical secrets hash differently.",
      "type": "string"
    },
    "created": {
      "description": "ISO-8601 UTC creation stamp (millisecond precision, trailing 'Z').",
      "type": "string"
    },
    "expires_at": {
      "description": "Optional TTL: the ISO-8601 UTC instant at/after which the token stops validating. Omitted means it never expires.",
      "type": "string"
    },
    "revoked": {
      "description": "When true the token never validates (revocation without deletion, preserving history).",
      "type": "boolean"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::governance
