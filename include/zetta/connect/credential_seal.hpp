#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// **Sealed connector credentials** ([#1230](https://github.com/oramasearch/zetta/issues/1230)).
//
// A saved connection's credential (a Snowflake token, a Postgres password) has
// to be on every node, because each node fetches its own stripe of a connector
// query straight from the source. The connection row therefore replicates to
// every member — and so the credential must not ride in it as plaintext. It is
// **sealed**: AES-256-GCM under a key that is never in the replicated catalogue.
//
// **Where the key comes from.** The sealing key is derived (HKDF-SHA256) from a
// root secret held outside the catalogue:
//   - a node started by `main` reads it from a file OUTSIDE `data_dir` named at
//     start (`--connection-root-secret-file` / ZETTA_CONNECTION_ROOT_SECRET_FILE,
//     e.g. a mounted Kubernetes Secret; #1314), so a copy of `data_dir` alone
//     does not open a stored credential. Every member is given the same file, so
//     each opens what another sealed. Started without one, `main` gives the node
//     no keyring at all: connector credentials are neither saved nor opened, and
//     every other capability runs. (Before #1314 `main` used the cluster join
//     token, which lives in `<data_dir>/security/`; it is now only a retired
//     secret, so rows sealed under it re-seal under the external one.)
//   - a node embedded without one (the library, a test) creates a random root
//     secret in `<data_dir>/security/connection-root-secret` (0600) on first
//     use. That key is local to the one node and sits beside the data, so it
//     defeats a reader of the table files but not a copy of `data_dir`.
//
// **Rotation.** A key carries an id (a short hash of the key). A sealed value
// names the id it was sealed under. The keyring holds the current key plus the
// retired ones it can still open; `connect::reseal_connections` re-seals every
// entry not under the current key (and any legacy plaintext entry), and runs on
// every node start — so rotating the join token re-seals the store the first
// time each node starts under the new token.
//
// The sealed value is bound to the connection's name (it is the AEAD's
// associated data), so a sealed credential copied onto another connection's row
// does not open.
namespace zetta::connect {

// One 256-bit sealing key and its public id (16 lowercase hex characters).
struct CredentialKey {
    std::string id;
    std::array<unsigned char, 32> bytes{};
};

// The keys this node seals and unseals with: the current one, and retired ones
// it can still open (but never seals under). An empty keyring seals nothing.
class CredentialKeyring {
public:
    CredentialKeyring() = default;

    // Derive the keyring from root secrets (the current join token, then the
    // retired ones). Empty optional with a reason when `current` is empty.
    static std::optional<CredentialKeyring> from_root_secrets(
        const std::string& current, const std::vector<std::string>& retired,
        std::string* reason = nullptr);

    bool empty() const { return !current_.has_value(); }
    const CredentialKey* current() const;
    // The key with this id — current or retired — or null.
    const CredentialKey* find(std::string_view id) const;

private:
    std::optional<CredentialKey> current_;
    std::vector<CredentialKey> retired_;
};

// The keyring of a node with no cluster root secret: the random root secret in
// `<data_dir>/security/connection-root-secret`, created 0600 on first use.
std::optional<CredentialKeyring> load_or_create_local_keyring(
    const std::string& data_dir, std::string* reason = nullptr);

// The root secret supplied from outside data_dir (#1314): the file's first
// non-empty line is the current secret, any later ones are retired secrets it
// can still open (rotation). Lines starting with `#` are comments.
struct ExternalRootSecret {
    std::string current;
    std::vector<std::string> retired;
};

// The shortest root secret accepted: 32 characters (128 bits as hex, more as
// random text). `openssl rand -hex 32` makes a suitable one.
inline constexpr std::size_t kMinimumRootSecretLength = 32;

// Read the external root secret at `path`. Refused (empty optional with a
// reason) when the file, after following symbolic links (a Kubernetes Secret
// mount is links), lies inside `data_dir`; when it is not a regular file, is
// larger than 64 KiB, is writable by group or others, or holds no secret; or
// when a secret is shorter than kMinimumRootSecretLength.
std::optional<ExternalRootSecret> load_external_root_secret(
    const std::string& path, const std::string& data_dir, std::string* reason = nullptr);

// Seal `plaintext`, bound to `context` (the connection name). The sealed value:
//   {"v":1, "alg":"AES-256-GCM", "kid", "nonce", "ciphertext", "tag"}
// with the binary fields lowercase hex. Empty optional with a reason when the
// keyring is empty or OpenSSL fails.
std::optional<nlohmann::json> seal_credential(const CredentialKeyring& keyring,
                                              std::string_view plaintext,
                                              std::string_view context,
                                              std::string* reason = nullptr);

// Open a sealed value bound to `context`. Empty optional with a reason when the
// value is malformed, names a key this keyring does not hold, or fails
// authentication (tampered, or bound to another connection).
std::optional<std::string> unseal_credential(const CredentialKeyring& keyring,
                                             const nlohmann::json& sealed,
                                             std::string_view context,
                                             std::string* reason = nullptr);

// The key id a sealed value names, or empty when it is not a well-formed sealed
// value.
std::string sealed_key_id(const nlohmann::json& sealed);

// Whether `value` has the sealed shape (mirrors sealed_credential_schema()).
bool is_sealed_credential(const nlohmann::json& value);

// The draft-07 schema of a sealed value — the code-owned source of truth;
// `schemas/sealed_credential.schema.json` is generated from it.
const nlohmann::json& sealed_credential_schema();

}  // namespace zetta::connect
