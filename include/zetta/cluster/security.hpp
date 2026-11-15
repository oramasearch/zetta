#pragma once

// Inter-node transport security (CTL-2, #466): the join token, the derived
// cluster CA, and per-node TLS identities. See docs/cluster/security.md.
//
// Trust model — "the join token is the trust root". A cluster has one join
// token (32 random bytes, lowercase hex). From it every holder derives the
// SAME cluster CA (an Ed25519 key seeded by a hash of the token, wrapped in a
// certificate with fixed fields), then mints its own per-node identity: a
// fresh Ed25519 keypair in a CA-signed certificate whose CN is the node id.
// The cluster transport (cluster.cpp) runs mutual TLS 1.3 with require_peer
// against that CA, so a node that was never given the token cannot complete
// the handshake — the join is token-gated at the transport, with no leader
// (matching the equal-peer cluster).
//
// The cluster refuses a peer whose HELLO node id is not its certificate's CN
// (hello_matches_peer_certificate, #669).
//
// Enrollment (#1319, CTL-2 V2). The token-derived CA lets any token holder mint
// a certificate for any node id, so the cluster also keeps a second CA, the
// ENROLLMENT CA: a random Ed25519 key, never derived from the token, generated
// once per cluster and handed to a member only inside an authenticated link.
// Every node enrolls the key it presents (its CSR is the TLS key the handshake
// just proved it holds) and gets a 30-day enrollment certificate for its node
// id, recorded as the replicated `node_cert:<node_id>`. Every node pins each
// enrolled node id to that key, and refuses a peer presenting another key for
// an enrolled id, whatever CA signed its certificate. A token holder can still
// join under a NEW id in this release (the token-derived CA is accepted for one
// more release, so a rolling upgrade works); it can no longer take an id that is
// already enrolled. See docs/cluster/security.md.

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/transport/credentials.hpp"

namespace zetta::cluster {

// Everything the cluster transport needs to run mutual TLS: the token (so the
// join command can hand it to the next node), the trust anchor, and this
// node's own identity. All-empty (a default ClusterSecurity) means plaintext.
struct ClusterSecurity : zetta::transport::TlsCredentials {
    std::string join_token;  // 64 lowercase hex chars — the cluster secret
    // Earlier join tokens this node ran under, newest first (at most
    // kMaxRetiredJoinTokens). Kept so secrets derived from a token — the
    // connector-credential sealing key (#1230) — can still open what was sealed
    // under the previous one and re-seal it. Not used for transport trust.
    std::vector<std::string> retired_join_tokens;

    // --- Enrollment (#1319) ------------------------------------------------
    // Whether the cluster runs enrollment: set by load_or_create_cluster_security
    // (every zetta process). An embedded Cluster with derived material opts in.
    bool enroll = false;
    // <data_dir>/security, where the enrollment CA, this node's enrollment
    // certificate and the pins are kept. Empty: kept in memory only.
    std::string directory;
    // The enrollment CA's private seed (64 hex), when this node holds it.
    std::string enrollment_ca_seed;
    // This node's enrollment certificate (PEM), when it has one.
    std::string enrolled_certificate_pem;

    bool enabled() const {
        return !join_token.empty() && complete();
    }
};

// The enrollment CA (#1319): a random Ed25519 key and the certificate it
// self-signs (subject CN=zetta-cluster-enrollment-ca, fields fixed so one seed
// always gives one certificate).
struct EnrollmentCa {
    std::string seed;             // 64 hex — the private key; never replicated
    std::string certificate_pem;  // public
    std::string public_key;       // 64 hex — the CA's raw Ed25519 public key
};

// A fresh enrollment CA from the OS CSPRNG, or the one `seed_hex` gives. Empty
// optional on a malformed seed or an OpenSSL failure.
std::optional<EnrollmentCa> generate_enrollment_ca();
std::optional<EnrollmentCa> enrollment_ca_from_seed(std::string_view seed_hex);

// How long an enrollment certificate is valid, and how long before it ends a
// node renews its own.
inline constexpr int kEnrollmentValidityDays = 30;
inline constexpr int kEnrollmentRenewDays = 10;

// SHA-256 of the SubjectPublicKeyInfo of a certificate, 64 hex: the key a node
// id is pinned to. Empty when the bytes are not one certificate.
std::string certificate_public_key_sha256(std::span<const unsigned char> der);
std::string certificate_pem_public_key_sha256(std::string_view pem);

// `ca`'s certificate for `node_id` over the public key in `subject_der` (the
// certificate the peer presented in the handshake: the key it proved it holds),
// valid from now for kEnrollmentValidityDays, with a random serial. Empty on a
// malformed subject or an OpenSSL failure.
std::string sign_enrollment_certificate(const EnrollmentCa& ca, const std::string& node_id,
                                        std::span<const unsigned char> subject_der);

// What a verified enrollment certificate says.
struct EnrolledCertificate {
    std::string node_id;
    std::string public_key_sha256;
    std::string serial;            // lowercase hex
    std::int64_t not_after = 0;    // seconds since the Unix epoch
    std::string ca_public_key;     // 64 hex
};

// `certificate_pem` when `ca_certificate_pem` (a self-signed enrollment CA)
// signed it, it is inside its validity window now, and it names exactly one
// common name. Empty optional otherwise.
std::optional<EnrolledCertificate> verify_enrolled_certificate(
    std::string_view certificate_pem, std::string_view ca_certificate_pem);

// The proof a joining node sends with its enrollment request:
// HMAC-SHA256(join token, "zetta-enroll-v1\n" + node id + "\n" + key hash), 64 hex.
std::string enrollment_proof(std::string_view join_token, std::string_view node_id,
                             std::string_view public_key_sha256);
// Whether `proof` is that proof, compared in constant time.
bool enrollment_proof_matches(std::string_view join_token, std::string_view node_id,
                              std::string_view public_key_sha256, std::string_view proof);

// The DER bytes of the first certificate in `pem`; empty when there is none.
std::vector<unsigned char> certificate_pem_to_der(std::string_view pem);

// The pins: node id -> the SHA-256 of the key it enrolled (#1319). Kept in
// <directory>/node-pins.json ({"version": 1, "pins": {...}}). A missing file is
// no pins; a malformed one fails.
bool load_node_pins(const std::string& directory, std::map<std::string, std::string>& pins,
                    std::string* reason = nullptr);
bool store_node_pins(const std::string& directory,
                     const std::map<std::string, std::string>& pins,
                     std::string* reason = nullptr);
// Store the enrollment CA's seed (cluster-ca.json) and this node's enrollment
// certificate (node-cert-enrolled.pem), both 0600, in `directory`.
bool store_enrollment(const std::string& directory, const EnrollmentCa& ca,
                      const std::string& enrolled_certificate_pem,
                      std::string* reason = nullptr);

// How many earlier join tokens the secret file keeps (ClusterSecurity::
// retired_join_tokens).
inline constexpr std::size_t kMaxRetiredJoinTokens = 4;

// What printed output shows in place of the join token (#1393). The token is the
// cluster root secret, so a command or message meant for a log carries this
// instead; the operator reads the value from <data_dir>/security/cluster-secret.json
// or passes it as ZETTA_CLUSTER_TOKEN.
inline constexpr std::string_view kJoinTokenPlaceholder = "<join-token>";

// A fresh join token: 32 bytes from the OS CSPRNG, lowercase hex.
std::string generate_join_token();

// True for a well-formed token (exactly 64 lowercase hex characters).
bool valid_join_token(std::string_view token);

// Derive the full security material for one node from a join token: the
// deterministic cluster CA (same token -> same CA on every node) plus a fresh
// per-node keypair + CA-signed certificate (CN = node_id). Empty optional with
// a reason on a malformed token or an OpenSSL failure. Pure — no file access.
std::optional<ClusterSecurity> derive_cluster_security(const std::string& join_token,
                                                       const std::string& node_id,
                                                       std::string* reason = nullptr);

// Load this node's security material from `<data_dir>/security/`, creating
// whatever is missing: the secret file (schema-validated JSON holding the join
// token; generated on a first start, or taken from `join_token_override` when
// the operator passed `--join-token` — a different token moves the stored one
// to `retired_join_tokens`), and the node's key + certificate (reused
// while they still verify against the derived CA and carry this node id,
// regenerated otherwise). Empty optional with a reason on a malformed secret
// file, a bad override token, or an I/O / OpenSSL failure.
std::optional<ClusterSecurity> load_or_create_cluster_security(
    const std::string& data_dir, const std::string& node_id,
    const std::string& join_token_override = {}, std::string* reason = nullptr);

// The draft-07 schemas of cluster-ca.json and node-pins.json (#1319), the
// code-owned sources of truth for schemas/cluster_ca.schema.json and
// schemas/node_pins.schema.json.
const nlohmann::json& cluster_ca_schema();
const nlohmann::json& node_pins_schema();
// The replicated `node_cert:<node_id>` record (#1319):
// {certificate, ca_certificate, public_key_sha256, serial, issued_by, not_after}.
const nlohmann::json& node_cert_record_schema();
inline constexpr std::string_view kNodeCertPrefix = "node_cert:";

// The draft-07 schema for the persisted secret file — the code-owned source of
// truth; `schemas/cluster_secret.schema.json` is generated from it and
// drift-checked by tests/test_schemas.cpp.
const nlohmann::json& cluster_secret_schema();

}  // namespace zetta::cluster
