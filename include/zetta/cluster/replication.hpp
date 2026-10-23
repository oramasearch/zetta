#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
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

// What one gossip round ships to one peer (#794): a document blob that merge()
// accepts, holding either the whole document or only the changes the peer lacks.
struct ReplicationDelta {
    // Empty when the peer already holds every change we hold: send nothing.
    std::vector<std::byte> blob;
    // The whole document: the peer's clock was unknown or unreadable, or it
    // lacks at least half of our changes (too far behind for a delta to pay).
    bool full = false;
    // How many changes a delta carries (0 for the whole document).
    std::size_t changes = 0;
};

// One write of an atomic ReplicationManager::update(): set `key` to `value`, or
// remove it (a tombstone, exactly like remove()) when `value` is empty.
struct ReplicationWrite {
    std::string key;
    std::optional<nlohmann::json> value;
};

// Decides an update from the key's current live value (null when absent or
// removed). It runs under the replication mutex, so it must be cheap and must not
// call back into the manager. Return nullopt to write nothing.
using ReplicationUpdateDecision =
    std::function<std::optional<std::vector<ReplicationWrite>>(
        const nlohmann::json* current)>;

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

    // Atomic read-check-write (#1231): read `key`'s live value, let `decide`
    // choose the writes, and apply them as ONE local change, all under the
    // replication mutex, so no other local write can interleave between the read
    // and the writes. Record collections and metrics are declared with it (an
    // existing definition is compared, never overwritten). Returns whether
    // anything was written.
    bool update(const std::string& key, const ReplicationUpdateDecision& decide);

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

    // Serialize the whole document (persistence, and the gossip fallback).
    std::vector<std::byte> save() const;

    // The document's clock (#794): for every actor, the highest change sequence
    // this document holds. Opaque, compact bytes (about 20 bytes per actor, so per
    // node). A peer advertises its clock on gossip; save_since() reads it back.
    std::string clock() const;

    // Only what a peer lacks (#794): the changes whose sequence is above the
    // peer's clock for their actor, as a document blob merge() accepts. Falls
    // back to the whole document (`full`) when `peer_clock` is empty or does not
    // decode, or when the peer lacks at least half of our changes. Changes travel
    // as stored, so signed or sealed values arrive unchanged. The blob is empty
    // when the peer lacks nothing.
    //
    // Gap-safe by construction: merge() skips a change whose sequence is at or
    // below the receiver's clock for that actor, so a delta must never start
    // above what the receiver holds. Basing it on the clock the receiver itself
    // reported (never on what we think we sent) guarantees that.
    ReplicationDelta save_since(std::string_view peer_clock) const;

    // Whether `clock` holds every change `other` holds (both from clock()). False
    // when either does not decode. Gossip uses it to see a peer take a SYNC.
    static bool clock_covers(std::string_view clock, std::string_view other);
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

    // The effective encoded value of `key` given the document's winner: the
    // removal marker when any concurrent value is a tombstone, else the winner.
    // Caller holds the state mutex.
    std::string effective_locked(const std::string& key,
                                 const std::string& winner) const;

    std::unique_ptr<State> state_;
    std::string node_id_;  // this node's UUID (its Automerge actor id)
    ReplicationPolicy policy_;
};

}  // namespace zetta::cluster
