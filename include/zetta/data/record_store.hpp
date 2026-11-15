#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/signal.hpp"
#include "zetta/data/write_gate.hpp"

namespace zetta::cluster {
class Cluster;
class ReplicationManager;
}  // namespace zetta::cluster

// The keyed record store (#1231, docs/data/record-store.md): durable, replicated
// records addressed by (collection, key), with conditional writes that are
// linearizable per key, lists over a declared secondary index, and per-collection
// retention. Records are opaque bytes (base64 on the wire); lake seals them
// before they arrive.
//
// Storage. The replicated Automerge catalogue holds only each collection's
// definition (`record_collection:<name>`). Records live in a node-local
// append-only log under `<data_dir>/records/` (src/core/data/record_log.hpp):
// the Automerge document keeps every write in its history and saves itself whole,
// which a soak measured growing without bound under steady writes.
//
//   * ORDERING. Every write of a key is ordered by one node, the key's OWNER: a
//     rendezvous hash of (collection, key) over this node and its live peers. The
//     owner checks the condition and writes under the log's lock. A write arriving
//     at another node is forwarded to the owner over the signal plane (`record.put`).
//   * DURABILITY. The owner fsyncs the frame before the write is acknowledged.
//   * SYNCHRONOUS REPLICATION. Before a write is acknowledged, the committed
//     record is installed on every live peer (`record.apply`), which keeps it only
//     when it is newer, so a read on any live node after the acknowledgement
//     returns it.
//   * ANTI-ENTROPY. A node that was down or partitioned catches up with
//     `anti_entropy()`: per collection, 256 bucket digests are compared with each
//     peer (`record.digest`), the versions in differing buckets are listed
//     (`record.keys`), and newer records are pulled (`record.fetch`).
//
// Thread safety: every method may be called from any thread. Writes and
// anti_entropy may block on the signal plane, so they must not run on the
// cluster's network thread or inside a tick; the registry runs them on the
// blocking pool.
namespace zetta::data {

struct RecordCollection {
    std::string name;
    std::vector<std::string> indexes;       // declared secondary index names
    std::optional<std::int64_t> retention_ms;  // records expire this long after their last write
    std::int64_t created_ms = 0;
};

struct StoredRecord {
    std::string collection;
    std::string key;
    std::string value;                          // base64; opaque to the engine
    std::uint64_t revision = 0;
    std::map<std::string, std::string> index;   // index name -> indexed value
    std::string order;                          // ordering value within an index
    std::int64_t updated_ms = 0;
    std::optional<std::int64_t> expires_ms;
    bool deleted = false;                       // a deletion's tombstone
};

enum class RecordCondition { None, IfAbsent, IfRevision };

// A put or a delete.
struct RecordWrite {
    std::string collection;
    std::string key;
    bool remove = false;
    std::string value;                          // put only
    std::map<std::string, std::string> index;   // put only
    std::string order;                          // put only
    RecordCondition condition = RecordCondition::None;
    std::uint64_t revision = 0;                 // IfRevision
};

enum class RecordStatus { Ok, InvalidInput, NotFound, Conflict, Unavailable };

struct RecordOutcome {
    RecordStatus status = RecordStatus::Ok;
    std::string message;                 // the reason when not Ok
    std::string path;                    // the input field at fault, when there is one
    std::optional<StoredRecord> record;  // the record written or read; on Conflict, the live one
    std::optional<RecordCollection> collection;  // create_collection
    bool created = false;                // create_collection: false when it already existed
    std::string owner;                   // the node that ordered the write
    std::size_t replicas = 0;            // live peers that installed the write
    std::size_t replicas_expected = 0;   // live peers when it was written

    bool ok() const { return status == RecordStatus::Ok; }
};

struct RecordListRequest {
    std::string collection;
    std::string index;      // empty: every record, in key order
    std::string value;      // the indexed value to list
    std::string after;      // the cursor a previous page returned
    std::size_t limit = 100;
    bool descending = false;
};

struct RecordPage {
    RecordStatus status = RecordStatus::Ok;
    std::string message;
    std::string path;
    std::vector<StoredRecord> records;
    std::string next_after;  // empty when the listing is complete

    bool ok() const { return status == RecordStatus::Ok; }
};

// What the node-local record log holds.
struct RecordStorageStats {
    std::uint64_t records = 0;       // keys held, deletion tombstones included
    std::uint64_t segments = 0;
    std::uint64_t disk_bytes = 0;    // every segment's size
    std::uint64_t live_bytes = 0;    // frames the key directory points at
    std::uint64_t memory_bytes = 0;  // estimate: key directory, indexes, expiry queue
};

// Wire names: "ok", "invalid_input", "not_found", "conflict", "unavailable".
std::string_view to_string(RecordStatus status);

// The boundary schemas (schema as code; schemas/*.json are generated).
const nlohmann::json& record_collection_schema();  // record_collection:<name>
const nlohmann::json& stored_record_schema();      // one record in the log and on the wire
const nlohmann::json& record_signal_schema();      // record.* signal payloads

nlohmann::json to_json(const RecordCollection& collection);
nlohmann::json to_json(const StoredRecord& record);  // the API shape, with collection + key
std::optional<RecordCollection> record_collection_from_json(const nlohmann::json& value,
                                                            std::string* reason = nullptr);

class RecordLog;

class RecordStore : public zetta::cluster::SignalHandler {
public:
    static constexpr std::string_view kPutMethod = "record.put";
    static constexpr std::string_view kApplyMethod = "record.apply";
    static constexpr std::string_view kDigestMethod = "record.digest";
    static constexpr std::string_view kKeysMethod = "record.keys";
    static constexpr std::string_view kFetchMethod = "record.fetch";

    // `directory` holds the record log; empty keeps it in memory (tests and
    // benchmarks that need no durability). `segment_bytes` 0 is the default
    // segment size (tests shrink it to exercise compaction). Call open() first.
    RecordStore(zetta::cluster::ReplicationManager& replication, std::string node_id,
                std::filesystem::path directory, std::uint64_t segment_bytes = 0);
    ~RecordStore() override;

    RecordStore(const RecordStore&) = delete;
    RecordStore& operator=(const RecordStore&) = delete;

    // Scan the log and rebuild the key directory.
    bool open(std::string* reason = nullptr);

    // Join the signal plane, chained over the cluster's current handler; detach
    // restores it. Node wires these into its coordinator lifecycle.
    void attach(zetta::cluster::Cluster& cluster);
    void detach();

    // Declare a collection. Idempotent for an identical definition; a different
    // definition under an existing name is a conflict.
    RecordOutcome create_collection(const RecordCollection& collection);
    std::optional<RecordCollection> find_collection(const std::string& name) const;

    RecordOutcome write(const RecordWrite& write);
    RecordOutcome get(const std::string& collection, const std::string& key) const;
    RecordPage list(const RecordListRequest& request) const;

    // Retention: removes this node's copies of expired records and of deletion
    // tombstones older than the grace period. Call from the owner loop.
    void sweep(std::int64_t now_ms);
    // Flush replica installs and run a bounded step of log compaction.
    void maintain();
    // Pull every record a live peer holds newer than this node does. Blocks on
    // the signal plane; run it off the owner loop.
    std::size_t anti_entropy();

    RecordStorageStats storage_stats() const;

    // Engine-internal collections (names starting `_`, which the API refuses):
    // the time series keeps its buckets here. `fold` rewrites one key on this node
    // only — its caller is the key's one writer — and replicates by anti-entropy.
    bool fold(const std::string& collection, const std::string& key,
              const std::function<std::optional<std::string>(const std::string* current)>& fold,
              std::optional<std::int64_t> expires_ms);
    void scan(const std::string& collection, const std::string& from_key,
              const std::function<bool(const std::string& key, const std::string& value)>& visit) const;
    // Engine-internal rows written once by their one writer, such as a text
    // entity-key table (#1255): `put_internal` stores the value durably on this
    // node and sends it to every live peer, `remove_internal` does the same with
    // a deletion. A peer that misses either catches up by anti-entropy. False
    // when this node's log could not be written or synced.
    bool put_internal(const std::string& collection, const std::string& key, std::string value);
    bool remove_internal(const std::string& collection, const std::string& key);

    // The node that orders writes of (collection, key) in this node's view.
    std::string owner_of(const std::string& collection, const std::string& key) const;

    // Backup snapshots (zetta#1518, docs/data/backup-snapshot.md). While a
    // snapshot holds the gate frozen, a write ordered here answers Unavailable
    // (retry) and a replica install is skipped (anti-entropy brings it later).
    // Engine-internal rows (put_internal, fold) are not gated.
    WriteGate& write_gate() { return gate_; }
    // The cluster this store is attached to; null on a standalone node.
    zetta::cluster::Cluster* cluster() const { return cluster_.load(); }
    // Every live record of `collection` (a user or an engine-internal one) as
    // this node holds it, in key order, with its revision and expiry. `visit`
    // returns false to stop.
    void export_records(const std::string& collection,
                        const std::function<bool(const StoredRecord&)>& visit) const;
    // Restore: install `record` exactly as it was stored (revision, index,
    // order, expiry) and send it to every live peer. False when the name or key
    // is invalid, the record has expired, the gate is frozen, or the log could
    // not be written.
    bool import_record(const StoredRecord& record);

    zetta::cluster::SignalResponse on_request(const zetta::cluster::Signal& request) override;
    void on_event(const zetta::cluster::Signal& event) override;
    void on_task(const zetta::cluster::Signal& task) override;

    // How long a deletion tombstone is kept. A node away for longer than this
    // can bring a deleted record back when it rejoins.
    static constexpr std::int64_t kTombstoneGraceMs = 24ll * 60 * 60 * 1000;

private:
    RecordOutcome commit(const RecordWrite& write, std::int64_t now_ms);
    bool install(const StoredRecord& record, std::int64_t now_ms);
    bool install_collection(const RecordCollection& collection);
    void replicate(const nlohmann::json& payload, const std::string& except,
                   RecordOutcome& outcome);
    std::size_t pull_from(const std::string& peer);
    std::vector<std::string> live_peers() const;

    zetta::cluster::ReplicationManager& replication_;
    std::string node_id_;
    std::unique_ptr<RecordLog> log_;
    std::atomic<zetta::cluster::Cluster*> cluster_{nullptr};
    std::atomic<zetta::cluster::SignalHandler*> delegate_{nullptr};
    WriteGate gate_;
};

}  // namespace zetta::data
