// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/connect/connector.hpp"

namespace zetta { class Node; }

// **Saved connections** — a named `ConnectorConfig`, credential included, held
// by **every** node ([#827](https://github.com/oramasearch/zetta/issues/827)).
//
// **Why every node.** The distributed connector fetch has each node pull its own
// stripe of the source's result straight from the source
// ([connector-query.md](../../../docs/data/connector-query.md)). A node can only
// do that if it holds the credential locally — a coordinator that fanned the
// token out per query would put the secret on the wire on every run and make the
// coordinator a bottleneck. So the store is an ordinary `data::Table`, whose
// `ReplicationStrategy` defaults to **ReplicateToAll**: the row lands on every
// member through the same CRDT gossip as any other row, once.
//
// **The credential is sealed in the row** ([#1230](https://github.com/oramasearch/zetta/issues/1230)).
// The row holds the connector settings without the token (`connector`, from
// `ConnectorConfig::to_json()`) and the token sealed with AES-256-GCM under a
// key derived from a secret held outside the replicated catalogue
// (`credential`; see credential_seal.hpp). Only `find_connection` /
// `list_connections` open it, in memory, for the connector query that presents
// it. Every read surface — `system.connections`, the `list_connections` and
// `create_connection` capabilities, the web form — renders through
// `SavedConnection::to_redacted_json()` or the metadata fields, and says only
// whether a credential is set. SQL cannot reach the store's database at all.
//
// **Legacy rows.** A row written before #1230 carries the token in plaintext
// inside `connector`. It still opens, and `reseal_connections` (run by
// `Node::start`) rewrites it sealed.
namespace zetta::connect {

// Where the store lives. One reserved database + table so nothing collides with
// a user's schema, and so `system.connections` and the docs name one source.
inline constexpr const char* kConnectionDatabase = "zetta_connect";
inline constexpr const char* kConnectionTable = "connections";

// What a read surface may say about a connection's credential — never the
// credential itself.
enum class CredentialState {
    Set,         // a sealed credential this node opened
    None,        // no credential (a Postgres trust/peer connection)
    Unreadable,  // sealed under a key this node does not hold, or tampered
    Plaintext,   // a legacy row not yet re-sealed (reseal_connections fixes it)
};
std::string_view to_string(CredentialState state);

// One saved connection.
struct SavedConnection {
    std::string name;
    ConnectorConfig config;  // includes the opened token (empty when Unreadable)
    std::string created;     // ISO-8601 UTC, when it was last saved
    CredentialState credential = CredentialState::None;
    std::string credential_error;  // why it is Unreadable

    // The shape every read surface returns: the metadata a human needs to pick
    // the right connection, `credential` ("set" / "none" / "unreadable" /
    // "plaintext"), and **no credential**. `ConnectorConfig::to_json()` already
    // omits the token, so this cannot leak by construction.
    nlohmann::json to_redacted_json() const;
};

// Whether `name` is usable as a connection name: 1–64 chars of
// `[A-Za-z0-9_-]`, starting with a letter or digit. Returns a reason when it is
// not, else an empty string. Applied at the boundary before anything is stored.
std::string connection_name_rejection(const std::string& name);

// Save `config` under `name`, replacing any existing connection with that name
// — which is how a credential is rotated: the new one is sealed afresh. Creates
// the reserved database + table on first use. Returns a path-qualified reason on
// failure (including no sealing key), empty on success. Non-throwing.
std::string save_connection(Node& node, const std::string& name,
                            const ConnectorConfig& config);

// The saved connection called `name`, or nullopt. **Carries the opened token**
// — this is the resolution path a connector query uses, not a read surface. A
// credential that does not open leaves `config.token` empty and says why in
// `credential_error`.
std::optional<SavedConnection> find_connection(const Node& node,
                                               const std::string& name);

// Every saved connection, in insertion order. Carries tokens; a caller rendering
// these to anyone must go through `to_redacted_json()`.
std::vector<SavedConnection> list_connections(const Node& node);

// Drop the connection called `name`. Returns whether one was removed.
bool remove_connection(Node& node, const std::string& name);

// What reseal_connections did.
struct ResealReport {
    std::size_t resealed = 0;             // rewritten under the current key
    std::vector<std::string> unreadable;  // names whose credential did not open
    std::string error;                    // no keyring: nothing could be sealed
};

// Re-seal every stored credential that is not sealed under the node's current
// key: legacy plaintext rows, and rows sealed under a retired root secret
// (rotation). Rows already current are left alone, so it writes nothing on a
// steady store. Run by Node::start.
ResealReport reseal_connections(Node& node);

}  // namespace zetta::connect
