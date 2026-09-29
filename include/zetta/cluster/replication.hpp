#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>


namespace zetta::cluster {

using ReplicationChangeToken = std::uint64_t;

struct ReplicationChanges {
    ReplicationChangeToken next_token = 0;
    std::shared_ptr<const nlohmann::json> snapshot;
    std::vector<std::string> keys;
    bool full_resync = false;
};

struct ReplicationMemoryUsage {
    std::uint64_t bytes = 0;
    std::uint64_t items = 0;
    std::uint64_t pending_change_bytes = 0;
    std::uint64_t pending_changes = 0;
    std::uint64_t audit_bytes = 0;
    std::uint64_t audit_items = 0;
};

// What/whom to replicate. The first (and currently only) policy is
// ReplicateToAllNodes: every node holds a full copy of all replicated state.
// More policies (sharded, quorum, ...) will be added through this policy enum.
enum class ReplicationPolicy { ReplicateToAllNodes };

// The node's replicated state, backed by an Automerge CRDT document.
//
// Local changes are made with set(); peers exchange save()/merge() blobs (the
// cluster gossips them) and the CRDT converges conflict-free — concurrent
// changes to different keys both survive, with no coordination. Automerge is
// hidden behind this adapter (pImpl); the rest of zetta sees only JSON + bytes.
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

    // This node's id — its UUIDv4 Automerge actor id (the same UUID as its
    // cluster node id). Stable for the manager's lifetime.
    const std::string& node_id() const { return node_id_; }

    // Set a top-level key in the replicated document (a local change). The value
    // may be any JSON (stored as a JSON string inside the CRDT).
    void set(const std::string& key, const nlohmann::json& value);

    // Remove a top-level key (a local change).
    //
    // This writes a TOMBSTONE value, it does not delete the key from the CRDT map,
    // and the difference is load-bearing (#838). A map delete carries the set of
    // predecessor writes the deleting node could see *at that moment*; any write
    // it had not yet merged survives the delete verbatim, so the key comes back on
    // every node the moment that write arrives — including on the node that
    // removed it. A tombstone is an ordinary value change, so it merges like any
    // other, and `snapshot()` treats a tombstoned key as absent.
    //
    // Removal WINS over a concurrent write: `snapshot()` inspects every concurrent
    // value at a key, so a key is gone if *any* of them is a tombstone, whatever
    // order the merges happened in. A write that is causally *after* the removal
    // (the writer had already seen the tombstone) re-creates the key normally —
    // that is a deliberate re-create, not a resurrection.
    //
    // The cost is that a removed key stays in the document as a tombstone. That
    // is the standard CRDT trade and it is already what `row:` deletes do.
    void remove(const std::string& key);

    // The merged document as JSON — live entries only, tombstoned keys omitted.
    // This compatibility accessor copies the writer-maintained parsed cache.
    nlohmann::json snapshot() const;

    // A shared immutable view of the writer-maintained parsed cache. Writers use
    // copy-on-write when a reader still holds the current view (#1079).
    std::shared_ptr<const nlohmann::json> shared_snapshot() const;

    // Return the keys whose effective values changed after `token`, together
    // with the immutable snapshot that contains those values. Token zero means
    // that the caller has no baseline and requires a full reconcile. A bounded
    // journal also requests a full reconcile when the token is too old (#1079).
    ReplicationChanges changes_since(ReplicationChangeToken token) const;

    // Whether the document knows this key at all, INCLUDING as a tombstone. This
    // is the question "was this ever replicated?", as opposed to `snapshot()`'s
    // "is it live now?". Node::migrate_to_replication asks it: a key that is
    // absent from the snapshot because it was removed must not be re-captured
    // from a stale local projection on the next start (#838).
    bool knows_key(const std::string& key) const;

    // Number of LIVE top-level keys in the replicated document — a coarse "how
    // much is replicated" count (zetta seeds one key per node, so this is the
    // number of nodes the document knows about). Tombstones are not counted.
    std::size_t entry_count() const;

    // Cheap counters for the CRDT document, parsed snapshot, encoded-value
    // cache, and bounded pending-change journal.
    ReplicationMemoryUsage memory_usage() const;

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

    // Restore this node's persisted document, including its local actor clock.
    // Unlike a peer merge, this preserves the sequence used for the next write.
    bool restore(std::span<const std::byte> blob);

    // #1164: durably migrate live legacy audit values, then discard every audit
    // operation (including removals) from history and all caches. The visitor
    // must make the record durable before returning true. After retirement,
    // local writes and peer merges cannot introduce audit: keys again.
    bool retire_audit(const std::function<bool(const std::string&,
                                             const nlohmann::json&)>& migrate,
                      const std::function<bool()>& flush);

private:
    struct State;

    // Whether any concurrent value at `key` is a tombstone. Caller holds the
    // state mutex.
    bool removed_locked(const std::string& key) const;

    std::unique_ptr<State> state_;
    std::string node_id_;  // this node's UUID (its Automerge actor id)
    ReplicationPolicy policy_;
};

}  // namespace zetta::cluster
