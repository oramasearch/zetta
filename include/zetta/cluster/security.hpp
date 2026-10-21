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
// the handshake — the join is token-gated at the transport, with no enrollment
// RPC and no leader (matching the equal-peer cluster).
//
// Known limitation (documented, follow-up): Corosio exposes no peer-certificate
// accessor, so a node that IS enrolled could still claim another member's node
// id in its HELLO. CTL-2's boundary (TB-3, outsiders) is closed; member-vs-
// member impersonation needs the CSR-enrollment upgrade.

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
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

    bool enabled() const {
        return !join_token.empty() && complete();
    }
};

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

// The draft-07 schema for the persisted secret file — the code-owned source of
// truth; `schemas/cluster_secret.schema.json` is generated from it and
// drift-checked by tests/test_schemas.cpp.
const nlohmann::json& cluster_secret_schema();

}  // namespace zetta::cluster
