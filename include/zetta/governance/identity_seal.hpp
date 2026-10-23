#pragma once

// The identity root (#763, #764): who may write the accounts, roles, tokens,
// role assignments, row-security policies, principals and access keys that
// every node trusts. See docs/governance/identity-seal.md.
//
// Trust model — "membership is not authority". The join token (cluster/
// security.hpp) admits a node to the cluster's replicated document; it says
// nothing about who may change identity. Identity records are therefore
// SEALED: each one carries an Ed25519 signature, over its catalogue key and its
// value, made by the cluster's identity root. Every node verifies the seal
// before it materializes the record, and a record whose seal is missing or
// wrong is treated as absent. A member that writes identity straight into the
// replicated document (no seal, a seal from another root, or a sealed value
// moved to another key) changes nothing anyone honors.
//
// Where the root comes from. The root's private key is derived from the
// owner's admin token (--admin-token / ZETTA_ADMIN_TOKEN), domain-separated, so
// every node handed the same owner token derives the same root and can issue
// identity, and a node started with any other token derives a different root
// whose records no other node accepts. A node with no owner token that founds a
// cluster generates a random root and keeps its seed; a node that only needs to
// verify pins the root's public key (--identity-root). The pinned public key is
// stored in <data_dir>/security/identity-root.json and wins over any later flag.
//
// Versions and removals (#1341). A seal also covers a per-key version (`_v`)
// and the root's public key, and a removal is itself a sealed record (a
// tombstone, `_tombstone: true`). A node keeps the newest sealed record it has
// accepted for every identity key and refuses an older one, so a member that
// kept an earlier sealed value (a token before its revocation) cannot write it
// back, and a member cannot delete a record without the root.
//
// What it does not cover, by design: a node that holds the owner token IS the
// owner, so a compromised node that was handed that token can issue identity.
// Recorded in the doc page.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::governance {

// The field a sealed identity record carries its seal in. The seal covers the
// key and every other field of the value.
inline constexpr std::string_view kIdentitySealField = "_seal";

// The seal's format prefix ("ed25519-v2:<128 hex>"). v2 signs the root's public
// key and the record's version along with the key and value (#1341).
inline constexpr std::string_view kIdentitySealPrefix = "ed25519-v2:";

// The field a sealed record carries its version in: a positive integer, larger
// for every later record of the same key. The seal covers it.
inline constexpr std::string_view kIdentityVersionField = "_v";

// The field that marks a sealed removal. A sealed tombstone is
// {"_tombstone": true, "_v": N, "_seal": ...} and carries nothing else.
inline constexpr std::string_view kIdentityTombstoneField = "_tombstone";

// A sealed identity record that opened: its fields (without `_seal`, `_v` and
// `_tombstone`), its version, whether it is a removal, and its seal text (which
// orders two records of the same version, so every node picks the same one).
struct OpenedIdentityRecord {
    nlohmann::json value;
    std::uint64_t version = 0;
    bool removed = false;
    std::string seal;
};

// Whether `a` replaces `b` (two records of one key): a higher version, or the
// same version and a larger seal. A total order, so every node agrees.
bool identity_record_supersedes(const OpenedIdentityRecord& a,
                                const OpenedIdentityRecord& b);

// True when `key` is an identity record's catalogue key (user:, role:,
// token:, user_role:, policy:, principal:, key:) — the records a seal guards.
bool is_identity_key(std::string_view key);

class IdentityAuthority {
public:
    // The root every holder of the owner admin token derives: the Ed25519 key
    // seeded by SHA-256("zetta-identity-root-v1" || secret). Empty optional for
    // an empty secret or an OpenSSL failure.
    static std::optional<IdentityAuthority> from_owner_secret(
        std::string_view owner_secret);
    // A fresh random root (a founding node with no owner token).
    static std::optional<IdentityAuthority> generate();
    // A root from its stored 32-byte seed (64 lowercase hex).
    static std::optional<IdentityAuthority> from_seed_hex(std::string_view seed_hex);
    // A verify-only root from its public key (64 lowercase hex). It opens
    // sealed records and seals nothing.
    static std::optional<IdentityAuthority> verify_only(
        std::string_view public_key_hex);

    // Whether this node holds the root's private key, so it may issue identity.
    bool can_seal() const { return has_seed_; }
    // The root's public key, 64 lowercase hex. The value --identity-root takes.
    const std::string& public_key_hex() const { return public_key_hex_; }
    // The private seed, 64 lowercase hex; empty for a verify-only root. Only a
    // generated root is ever written to disk.
    std::string seed_hex() const;

    // `value` with its version and seal set (an existing seal is replaced).
    // Empty optional when this root cannot seal, `value` is not a JSON object,
    // `value` carries a tombstone marker, or `version` is zero.
    std::optional<nlohmann::json> seal(std::string_view key, nlohmann::json value,
                                       std::uint64_t version) const;
    // A sealed removal of `key` at `version`. Empty optional when this root
    // cannot seal or `version` is zero.
    std::optional<nlohmann::json> seal_removal(std::string_view key,
                                               std::uint64_t version) const;
    // The record, when its seal is this root's signature over the root, `key`,
    // and the rest of `value`, and it carries a positive version. Empty optional
    // otherwise: no seal, a malformed seal, another root's seal, a changed
    // field or version, another key, no version, or a malformed tombstone.
    std::optional<OpenedIdentityRecord> open(std::string_view key,
                                             const nlohmann::json& value) const;

    // A UUID-shaped id fixed by this root and `purpose` ("owner-user",
    // "builtin-role:admin", ...). Every node that bootstraps the same root
    // writes the same key, so the owner and the built-in roles converge to one
    // record however many nodes seeded them, and in whatever order.
    std::string derived_id(std::string_view purpose) const;

    bool operator==(const IdentityAuthority& other) const {
        return public_key_hex_ == other.public_key_hex_;
    }

private:
    IdentityAuthority() = default;
    static std::optional<IdentityAuthority> from_seed(
        const std::array<unsigned char, 32>& seed);
    // `value` (no seal yet) with this root's seal added.
    std::optional<nlohmann::json> sign(std::string_view key,
                                       nlohmann::json value) const;

    bool has_seed_ = false;
    std::array<unsigned char, 32> seed_{};
    std::array<unsigned char, 32> public_key_{};
    std::string public_key_hex_;
};

// The version the next sealed record of a key takes: larger than `current`'s
// (the record the document holds now, when it opens under `root`) and than
// `floor` (the newest the caller holds), and at least the wall clock in
// milliseconds, so writers on different nodes that never saw each other's
// records still order by time.
std::uint64_t next_identity_version(const IdentityAuthority& root,
                                    std::string_view key,
                                    const nlohmann::json* current,
                                    std::uint64_t floor = 0);

// What load_or_create_identity_root decided, for the startup log.
struct IdentityRootSetup {
    std::optional<IdentityAuthority> authority;
    // Set when the node runs, but cannot issue identity for a reason its
    // operator should hear (its admin token is not the cluster's root).
    std::string warning;
    // True when this call pinned a root for the first time.
    bool pinned_now = false;
};

// Load this node's identity root from <data_dir>/security/identity-root.json,
// pinning one on the first start:
//   - a pinned file wins. `owner_secret` gives the node the private key only
//     when it derives the pinned public key (else a warning, verify-only); a
//     stored seed (a generated root) does the same. `root_override`
//     (--identity-root) must equal the pinned key, or the start is refused.
//   - no file: `root_override` is pinned when given; else the root derived from
//     `owner_secret`; else a generated root, whose seed is stored (0600).
// Empty `authority` with a reason on a malformed file, a bad override, a
// mismatched override, or an I/O / OpenSSL failure.
IdentityRootSetup load_or_create_identity_root(const std::string& data_dir,
                                               const std::string& owner_secret,
                                               const std::string& root_override,
                                               std::string* reason = nullptr);

// The draft-07 schema of identity-root.json — the code-owned source of truth;
// schemas/identity_root.schema.json is generated from it.
const nlohmann::json& identity_root_schema();

}  // namespace zetta::governance
