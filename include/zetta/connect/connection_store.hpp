#pragma once

#include <optional>
#include <string>
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
// **The token is in the row. That is the point, and it is the one place it
// lives.** Every *derived* surface redacts it: `system.connections`, the
// `list_connections` capability, audit event details, log lines, and
// `ConnectorConfig::to_json()` all omit it. The only serialization that carries
// it is `ConnectorConfig::to_storage_json()`, which exists solely for this store
// — never hand its output to a caller.
namespace zetta::connect {

// Where the store lives. One reserved database + table so nothing collides with
// a user's schema, and so `system.connections` and the docs name one source.
inline constexpr const char* kConnectionDatabase = "zetta_connect";
inline constexpr const char* kConnectionTable = "connections";

// One saved connection.
struct SavedConnection {
    std::string name;
    ConnectorConfig config;  // includes the token
    std::string created;     // ISO-8601 UTC, when it was last saved

    // The shape every read surface returns: the metadata a human needs to pick
    // the right connection, and **no credential**. `ConnectorConfig::to_json()`
    // already omits the token, so this cannot leak by construction.
    nlohmann::json to_redacted_json() const;
};

// Whether `name` is usable as a connection name: 1–64 chars of
// `[A-Za-z0-9_-]`, starting with a letter or digit. Returns a reason when it is
// not, else an empty string. Applied at the boundary before anything is stored.
std::string connection_name_rejection(const std::string& name);

// Save `config` under `name`, replacing any existing connection with that name.
// Creates the reserved database + table on first use. Returns a path-qualified
// reason on failure, empty on success. Non-throwing.
std::string save_connection(Node& node, const std::string& name,
                            const ConnectorConfig& config);

// The saved connection called `name`, or nullopt. **Carries the token** — this
// is the resolution path a connector query uses, not a read surface.
std::optional<SavedConnection> find_connection(const Node& node,
                                               const std::string& name);

// Every saved connection, in insertion order. Carries tokens; a caller rendering
// these to anyone must go through `to_redacted_json()`.
std::vector<SavedConnection> list_connections(const Node& node);

// Drop the connection called `name`. Returns whether one was removed.
bool remove_connection(Node& node, const std::string& name);

}  // namespace zetta::connect
