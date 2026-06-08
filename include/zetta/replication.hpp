#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {

// What/whom to replicate. The first (and currently only) policy is
// ReplicateToAllNodes: every node holds a full copy of all replicated state.
// More policies (sharded, quorum, ...) will be added behind this enum/seam.
enum class ReplicationPolicy { ReplicateToAllNodes };

// The node's replicated state, backed by an Automerge CRDT document.
//
// Local changes are made with set(); peers exchange save()/merge() blobs (the
// cluster gossips them) and the CRDT converges conflict-free — concurrent
// changes to different keys both survive, with no coordination. Automerge is
// hidden behind this facade (pImpl); the rest of zetta sees only JSON + bytes.
// Thread-safe (the cluster's network thread and local callers both touch it).
//
// Holds the node's DDL / metadata / rows for replication. (Hooking the live
// Node's mutations into here is the next step; today it's a standalone
// replicated document.)
class ReplicationManager {
public:
    // `node_id` is this node's UUIDv4 — its unique Automerge actor id, so each
    // node's changes are attributed distinctly and concurrent edits merge
    // conflict-free.
    explicit ReplicationManager(
        std::string node_id,
        ReplicationPolicy policy = ReplicationPolicy::ReplicateToAllNodes);
    ~ReplicationManager();

    ReplicationManager(const ReplicationManager&) = delete;
    ReplicationManager& operator=(const ReplicationManager&) = delete;

    ReplicationPolicy policy() const { return policy_; }

    // Set a top-level key in the replicated document (a local change). The value
    // may be any JSON (stored as a JSON string inside the CRDT).
    void set(const std::string& key, const nlohmann::json& value);

    // Remove a top-level key (a local change). No-op if absent. Removal merges
    // conflict-free like any other change.
    void remove(const std::string& key);

    // The merged document as JSON.
    nlohmann::json snapshot() const;

    // Number of top-level keys in the replicated document — a coarse "how much
    // is replicated" count (zetta seeds one key per node, so this is the number
    // of nodes the document knows about).
    std::size_t entry_count() const;

    // A short, stable fingerprint of the document's current Automerge heads
    // (the change-DAG leaves). Two nodes hold identical state exactly when their
    // versions match — that's the convergence/"in sync" check. "empty" for an
    // empty document.
    std::string version() const;

    // Serialize the whole document, to gossip to a peer.
    std::vector<std::byte> save() const;
    // Merge a peer's serialized document into ours (conflict-free). Ignored if
    // the blob doesn't decode.
    void merge(std::span<const std::byte> blob);

private:
    struct State;  // hides the Automerge backend
    std::unique_ptr<State> state_;
    ReplicationPolicy policy_;
};

}  // namespace zetta
