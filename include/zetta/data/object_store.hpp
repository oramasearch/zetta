#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/signal.hpp"

namespace zetta::cluster {
class Cluster;
class ReplicationManager;
}  // namespace zetta::cluster

// The object capability (#1005, docs/data/object-store.md): durable byte objects
// addressed by (bucket, key), the storage under lake's liquid `store_backend`
// (a workspace's liquid store bytes live in its workspace Zetta).
// It maps one to one onto liquid's backend: put, put_range, get_range, remove,
// list and barrier, plus usage, a per-bucket quota and an owner lease whose fence
// every mutation carries.
//
// Storage. The replicated Automerge catalogue holds only each bucket's definition
// (`object_bucket:<name>`) and its owner lease (`object_lease:<name>`). Object
// bytes live in a node-local segmented log per bucket under
// `<data_dir>/objects/<bucket>/` (src/core/data/object_log.hpp), never in the
// catalogue (#1231 measured what writes do to it).
//
//   * PLACEMENT. On a multi-node cluster each object is held by `rf` OWNERS, the
//     top of a rendezvous hash of (bucket, key) over this node and its live
//     permanent peers. The first owner, the PRIMARY, orders the key's mutations:
//     it checks the fence, the quota and the write's condition, and appends. A
//     mutation arriving elsewhere is forwarded to it (`object.write`).
//   * REPLICATION. The node that took the request then installs the decided
//     version on the other owners (`object.apply`) and acknowledges once all `rf`
//     owners hold it; otherwise it answers `Unavailable` (retry: an append retry
//     of the same bytes succeeds and changes nothing).
//   * DURABILITY. A write is acknowledged after write(2) on every owner. The
//     barrier fsyncs every bucket segment written since the last barrier on every
//     live node; `durable` makes one write fsync before it answers.
//   * FENCING. The bucket's lease is decided by one node (a rendezvous hash of the
//     bucket), recorded in the catalogue and installed on every live peer before
//     `acquire` answers. Its fence rises with every new holder. Once a bucket has
//     had a holder, every mutation must carry the current fence.
//   * ANTI-ENTROPY. Every few seconds each node compares per-bucket digests of
//     (key, generation, size) for the keys it owns with each peer (`object.digest`,
//     `object.keys`) and pulls what it lacks (`object.read`); a copy it no longer
//     owns is dropped once every owner holds that version.
//
// Thread safety: every method may be called from any thread. Mutations, reads
// that fall to a peer, barrier and anti_entropy may block on the signal plane,
// so they must not run on the cluster's network thread or inside a tick; the
// API runs them on the blocking pool.
namespace zetta::data {

struct ObjectBucket {
    std::string name;
    std::optional<std::uint64_t> quota_bytes;
    std::optional<std::uint32_t> rf;  // copies on a multi-node cluster; absent: min(3, nodes)
    std::int64_t created_ms = 0;
};

struct ObjectLease {
    std::string bucket;
    std::string holder;          // empty once released
    std::uint64_t fence = 0;     // rises with every new holder; 0: never held
    std::int64_t expires_ms = 0;
    std::uint64_t version = 0;   // orders lease states: the higher one wins
};

enum class ObjectStatus {
    Ok,
    InvalidInput,
    NotFound,
    Conflict,       // the write's condition failed (offset, if_absent, bucket definition)
    Fenced,         // the write's fence is not the bucket's current fence
    Held,           // acquire: another holder has the lease
    OutOfRange,     // a read past the object's end
    QuotaExceeded,
    Unavailable,    // an owner did not answer, or not every owner holds the write: retry
    IncarnationChanged,  // the caller's expected incarnation is not the bucket's (lake#543)
};

// Wire names: "ok", "invalid_input", "not_found", "conflict", "fenced", "held",
// "range_not_satisfiable", "quota_exceeded", "unavailable", "incarnation_changed".
std::string_view to_string(ObjectStatus status);

struct ObjectInfo {
    std::string key;
    std::uint64_t size = 0;
    std::uint64_t generation = 0;  // rises with every new version of the key
    std::int64_t updated_ms = 0;
};

struct ObjectOutcome {
    ObjectStatus status = ObjectStatus::Ok;
    std::string message;
    std::string path;                    // the input field at fault, when there is one
    std::optional<ObjectInfo> object;    // written or read; on Conflict the live one (size 0 when absent)
    bool unchanged = false;              // a retried append whose bytes were already there
    std::optional<ObjectBucket> bucket;  // create_bucket
    bool created = false;                // create_bucket: false when it already existed
    std::optional<ObjectLease> lease;    // lease calls; on Held the holder's
    std::uint64_t fence = 0;             // on Fenced: the bucket's current fence
    std::string owner;                   // the node that ordered the write
    std::size_t replicas = 0;            // owners that hold the write
    std::size_t replicas_expected = 0;   // the owners it was placed on
    std::size_t nodes = 0;               // barrier: nodes that synced
    // The deciding node's incarnation for the bucket (lake#543): for a write,
    // the one it was decided under; for a barrier, the one read after the sync;
    // on IncarnationChanged, the current one.
    std::string incarnation;

    bool ok() const { return status == ObjectStatus::Ok; }
};

// What every mutation carries.
struct ObjectWrite {
    std::optional<std::uint64_t> fence;  // the Zetta-Fence header
    bool durable = false;                // fsync before answering
    // The Zetta-Incarnation precondition (lake#543): the bucket's incarnation
    // the caller last saw; empty for none. Checked by the deciding node in the
    // same step as the write.
    std::string incarnation;
};

struct ObjectPage {
    ObjectStatus status = ObjectStatus::Ok;
    std::string message;
    std::string path;
    std::vector<ObjectInfo> objects;
    std::string next_after;  // empty when the listing is complete

    bool ok() const { return status == ObjectStatus::Ok; }
};

struct ObjectUsage {
    ObjectStatus status = ObjectStatus::Ok;
    std::string message;
    std::uint64_t objects = 0;
    std::uint64_t bytes = 0;
    std::optional<std::uint64_t> quota_bytes;

    bool ok() const { return status == ObjectStatus::Ok; }
};

// A resolved read: a consistent view of one version of one object, read a slice
// at a time. A local reader holds the segments it reads from, so a version
// replaced or removed meanwhile still reads whole; a reader of a peer's copy is
// pinned to the version's generation and fails if the peer no longer holds it.
class ObjectReader {
public:
    virtual ~ObjectReader() = default;
    virtual const ObjectInfo& info() const = 0;
    // The range resolved, within the object.
    virtual std::uint64_t offset() const = 0;
    virtual std::uint64_t length() const = 0;
    // `count` bytes starting `from` bytes into the range. False on an I/O or
    // peer failure.
    virtual bool read(std::uint64_t from, std::uint64_t count, std::string& out) = 0;
};

// A whole-object put streamed in parts (begin_upload → upload_part* →
// commit_upload or abort_upload). Its parts are staged on every owner and become
// visible at once when the commit is decided.
struct ObjectUpload {
    std::uint64_t id = 0;
    std::string bucket;
    std::string key;
    ObjectWrite options;
    std::vector<std::string> owners;
    std::uint64_t written = 0;
};

// What the node-local object logs hold.
struct ObjectStorageStats {
    std::uint64_t buckets = 0;
    std::uint64_t failed_logs = 0;   // buckets whose log failed an fsync and is not reopened
    std::uint64_t stopped_logs = 0;  // of those, the ones whose reopening gave up
    std::uint64_t objects = 0;      // live objects held on this node
    std::uint64_t object_bytes = 0; // their bytes
    std::uint64_t tombstones = 0;
    std::uint64_t segments = 0;
    std::uint64_t disk_bytes = 0;   // every segment's size
    std::uint64_t live_bytes = 0;   // frames the directories point at
    std::uint64_t memory_bytes = 0; // estimate: directories and uploads
};

// A bucket whose node-local log failed an fsync (#1235): it serves nothing on
// this node until maintenance reopens it from disk, or, once `stopped`, until
// the node restarts.
struct ObjectLogFailure {
    std::string bucket;
    bool stopped = false;
    std::uint32_t reopen_attempts = 0;
};

// The boundary schemas (schema as code; schemas/*.json are generated).
const nlohmann::json& object_bucket_schema();  // object_bucket:<name>
const nlohmann::json& object_lease_schema();   // object_lease:<name>
const nlohmann::json& object_signal_schema();  // the header of every object.* signal

nlohmann::json to_json(const ObjectBucket& bucket);
nlohmann::json to_json(const ObjectLease& lease);
std::optional<ObjectBucket> object_bucket_from_json(const nlohmann::json& value,
                                                    std::string* reason = nullptr);
std::optional<ObjectLease> object_lease_from_json(const std::string& bucket,
                                                  const nlohmann::json& value);

// Names: buckets `^[a-z0-9][a-z0-9_.-]{0,95}$`; keys 1 to 1024 bytes, no control
// characters; lease holders 1 to 256 bytes, no control characters.
bool valid_bucket_name(std::string_view name);
bool valid_object_key(std::string_view key);

class ObjectLog;

class ObjectStore : public zetta::cluster::SignalHandler {
public:
    // The most bytes one frame, one signal and one step of a larger write carry.
    static constexpr std::uint32_t kSliceBytes = 4u * 1024 * 1024;
    static constexpr std::size_t kMaxListLimit = 1000;
    static constexpr std::int64_t kMaxLeaseTtlMs = 24ll * 60 * 60 * 1000;
    // How long a deletion tombstone is kept. A node away for longer than this can
    // bring a removed object back when it rejoins.
    static constexpr std::int64_t kTombstoneGraceMs = 24ll * 60 * 60 * 1000;
    // An upload with no part for this long is dropped.
    static constexpr std::int64_t kUploadIdleMs = 10ll * 60 * 1000;
    // The durability format this build's bucket logs keep, reported as
    // `object_log_prefix_durability` (lake#543). 1: the prefix guarantee of
    // #1235 — after any crash, power loss included, a bucket's log on one node
    // keeps a prefix of its writes reaching the last durable write and barrier.
    // A node that reports nothing predates it. Raise it only for a stronger
    // guarantee; a caller accepts this value or higher.
    static constexpr std::uint32_t kPrefixDurability = 1;

    // `directory` holds one log per bucket; empty keeps everything in memory
    // (tests and benchmarks that need no durability). `segment_bytes` 0 is the
    // default segment size (tests shrink it to exercise compaction).
    ObjectStore(zetta::cluster::ReplicationManager& replication, std::string node_id,
                std::filesystem::path directory, std::uint64_t segment_bytes = 0);
    ~ObjectStore() override;

    ObjectStore(const ObjectStore&) = delete;
    ObjectStore& operator=(const ObjectStore&) = delete;

    // Open every bucket log already on disk.
    bool open(std::string* reason = nullptr);

    // A random id (32 hex digits) chosen when this store was constructed, so
    // it differs after every start of the node (lake#543).
    const std::string& incarnation() const { return incarnation_; }
    // The incarnation a bucket's answers carry: incarnation(), `.`, and how
    // many times this node reopened the bucket's log after a failed fsync. A
    // restart or a reopen can drop writes acknowledged without durability, and
    // either changes it, so a caller that sees it change between a write's
    // answer and its barrier's knows that write may be gone. Read it before
    // the call it answers for: a reopen that lands during the call then shows
    // in the next answer, never hidden in this one.
    std::string incarnation(const std::string& bucket) const;

    // Join the signal plane, chained over the cluster's current handler; detach
    // restores it.
    void attach(zetta::cluster::Cluster& cluster);
    void detach();

    // ---- buckets ----------------------------------------------------------------

    // Idempotent for an identical definition; a different one is a Conflict.
    ObjectOutcome create_bucket(const ObjectBucket& bucket);
    std::optional<ObjectBucket> find_bucket(const std::string& name) const;

    // ---- mutations ----------------------------------------------------------------

    // Atomically replace the whole object. Bodies larger than kSliceBytes go
    // through an upload. `if_absent`: Conflict when the key exists.
    ObjectOutcome put(const std::string& bucket, const std::string& key, std::string_view bytes,
                      const ObjectWrite& options, bool if_absent = false);
    // Create or extend the object at exactly `offset` (see the contract in
    // docs/data/object-store.md). Bodies larger than kSliceBytes are applied a
    // slice at a time, each conditional on the previous.
    ObjectOutcome put_range(const std::string& bucket, const std::string& key,
                            std::uint64_t offset, std::string_view bytes,
                            const ObjectWrite& options);
    ObjectOutcome remove(const std::string& bucket, const std::string& key,
                         const ObjectWrite& options);

    // A streamed whole-object put. `expected_size`, when known, is checked
    // against the quota before any byte is staged.
    ObjectOutcome begin_upload(const std::string& bucket, const std::string& key,
                               const ObjectWrite& options,
                               std::optional<std::uint64_t> expected_size, ObjectUpload& upload);
    ObjectOutcome upload_part(ObjectUpload& upload, std::string_view bytes);  // ≤ kSliceBytes
    ObjectOutcome commit_upload(ObjectUpload& upload, bool if_absent);
    void abort_upload(ObjectUpload& upload);

    // ---- reads ----------------------------------------------------------------------

    // Resolve [offset, offset + length) of the object (`length` absent: to its end).
    // NotFound when absent, OutOfRange when the range is not inside it.
    // `needs_peer`, when given, makes this a local-only call that never waits:
    // when the read would go to a peer it sets *needs_peer and returns at once.
    ObjectOutcome open_read(const std::string& bucket, const std::string& key,
                            std::uint64_t offset, std::optional<std::uint64_t> length,
                            std::shared_ptr<ObjectReader>& reader, bool* needs_peer = nullptr);
    // Live objects by key, `limit` 1 to 1000; `next_after` continues the listing.
    ObjectPage list(const std::string& bucket, const std::string& prefix,
                    const std::string& after, std::size_t limit);
    // Every write acknowledged before the call is fsynced on every live node.
    // `expected_incarnation` (lake#543), when not empty, must be this node's
    // incarnation for the bucket before the sync, and the barrier fails
    // IncarnationChanged when the bucket's log was reopened while it synced:
    // the answer's incarnation is the one read after the sync.
    ObjectOutcome barrier(const std::string& bucket, const std::string& expected_incarnation = {});
    ObjectUsage usage(const std::string& bucket);

    // ---- the owner lease ----------------------------------------------------------

    // One attempt: Held while another holder's lease is unexpired. The API's
    // `wait_ms` retries on lease changes (watch_lease) and on expiry.
    ObjectOutcome acquire(const std::string& bucket, const std::string& holder,
                          std::int64_t ttl_ms);
    ObjectOutcome renew(const std::string& bucket, const std::string& holder,
                        std::uint64_t fence, std::int64_t ttl_ms);
    ObjectOutcome release(const std::string& bucket, const std::string& holder,
                          std::uint64_t fence);
    std::optional<ObjectLease> lease(const std::string& bucket) const;
    // `wake` runs once, on the next change of the bucket's lease on this node.
    void watch_lease(const std::string& bucket, std::function<void()> wake);

    // ---- placement and upkeep ------------------------------------------------------

    // The nodes that hold (bucket, key) in this node's view, primary first.
    std::vector<std::string> owners_of(const std::string& bucket, const std::string& key) const;
    // Tombstone grace, idle uploads, segment flushing and a bounded compaction step.
    void maintain(std::int64_t now_ms);
    // Pull what this node owns and lacks from every live peer, and drop copies it
    // no longer owns once their owners hold them. Blocks on the signal plane.
    std::size_t anti_entropy();
    ObjectStorageStats storage_stats() const;
    // The buckets whose log has failed on this node (health).
    std::vector<ObjectLogFailure> failed_logs() const;

    // An injection point for fault tests: performs every fsync of every bucket
    // log (segment files and directories) in place of ::fsync. Set it before
    // the store opens or writes.
    using Fsync = std::function<int(const std::filesystem::path& file, int descriptor)>;
    void set_fsync(Fsync fsync);
    // Injection points for fault tests (lake#543): run on the deciding node
    // between a write's precondition and fence checks and its log write, and
    // between a barrier's first incarnation read and its sync. Set them before
    // the calls they should interrupt.
    void set_before_log_write(std::function<void()> hook);
    void set_before_barrier_sync(std::function<void()> hook);

    zetta::cluster::SignalResponse on_request(const zetta::cluster::Signal& request) override;
    void on_event(const zetta::cluster::Signal& event) override;
    void on_task(const zetta::cluster::Signal& task) override;

private:
    struct Bucket;
    struct Mutation;
    // This node's view of placement for one bucket, computed once for a loop.
    struct Placement {
        std::vector<std::string> candidates;
        std::uint32_t rf = 1;
        std::vector<std::string> owners(const std::string& bucket, const std::string& key) const;
        bool owns(const std::string& node, const std::string& bucket, const std::string& key) const;
    };
    Placement placement(const std::string& bucket) const;

    Bucket* bucket_state(const std::string& name, bool create);
    std::vector<std::string> live_peers() const;
    std::uint32_t replication_factor(const std::string& bucket) const;
    ObjectOutcome mutate(const Mutation& mutation, std::string_view bytes);
    ObjectOutcome decide(const Mutation& mutation, std::string_view bytes);
    ObjectOutcome install(const Mutation& mutation, std::string_view bytes);
    ObjectOutcome lease_decide(const nlohmann::json& request);
    ObjectOutcome lease_call(const nlohmann::json& request);
    void install_lease(const ObjectLease& lease);
    std::uint64_t bucket_bytes(Bucket& state, const std::string& bucket) const;
    std::size_t pull_from(const std::string& peer);
    std::size_t evict_unowned();

    zetta::cluster::ReplicationManager& replication_;
    std::string node_id_;
    std::filesystem::path directory_;
    std::uint64_t segment_bytes_;
    std::string incarnation_;
    Fsync fsync_;  // tests only (set_fsync)
    std::function<void()> before_log_write_;     // tests only
    std::function<void()> before_barrier_sync_;  // tests only
    // `expected` against this node: nullopt when it names another start of the
    // node (or is malformed), else the log reopen count it names.
    std::optional<std::uint64_t> expected_reopens(const std::string& expected) const;
    mutable std::shared_mutex buckets_mutex_;
    std::map<std::string, std::unique_ptr<Bucket>> buckets_;
    std::mutex lease_mutex_;  // lease decisions on this node, one at a time
    std::atomic<zetta::cluster::Cluster*> cluster_{nullptr};
    std::atomic<zetta::cluster::SignalHandler*> delegate_{nullptr};
};

}  // namespace zetta::data
