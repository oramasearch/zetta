#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/signal.hpp"

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
// It is built on the replicated catalogue, in the catalogue's idiom: every
// record, index entry and collection definition is a key of the Automerge
// document, so it replicates, persists and reconciles like everything else.
// Three things are added on top:
//
//   * ORDERING. Every write of a key is ordered by one node, the key's OWNER: a
//     rendezvous hash of (collection, key) over this node and its live peers. The
//     owner checks the condition and writes with ReplicationManager::update, an
//     atomic read-check-write under the replication mutex. A write arriving at
//     another node is forwarded to the owner over the signal plane (`record.put`).
//   * SYNCHRONOUS REPLICATION. Before a write is acknowledged, the committed
//     record is sent to every live peer (`record.apply`) and installed there, so a
//     read on any live node after the acknowledgement returns it. A record key is a
//     revision register (replication.hpp): concurrent copies resolve to the highest
//     revision on every node.
//   * DURABILITY. The owner appends the committed record to a write-ahead journal
//     (`records.journal`) and fsyncs it before acknowledging. Startup replays the
//     journal into the document; a successful persist of the document truncates
//     what it now contains.
//
// Thread safety: every method may be called from any thread. Reads use the shared
// snapshot (#1227: one find, or one key range — never a copy of the catalogue).
// Writes may block on the signal plane, so they must not run on the cluster's
// network thread or inside a tick; the registry runs them on the blocking pool.
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

// Wire names: "ok", "invalid_input", "not_found", "conflict", "unavailable".
std::string_view to_string(RecordStatus status);

// The boundary schemas (schema as code; schemas/*.json are generated).
const nlohmann::json& record_collection_schema();  // record_collection:<name>
const nlohmann::json& stored_record_schema();      // record:<collection>:<key>
const nlohmann::json& record_signal_schema();      // record.put / record.apply payloads

nlohmann::json to_json(const RecordCollection& collection);
nlohmann::json to_json(const StoredRecord& record);  // the API shape, with collection + key
std::optional<RecordCollection> record_collection_from_json(const nlohmann::json& value,
                                                            std::string* reason = nullptr);

class RecordStore : public zetta::cluster::SignalHandler {
public:
    static constexpr std::string_view kPutMethod = "record.put";
    static constexpr std::string_view kApplyMethod = "record.apply";

    // `journal_path` is the write-ahead journal; empty disables it (tests that
    // need no durability).
    RecordStore(zetta::cluster::ReplicationManager& replication, std::string node_id,
                std::filesystem::path journal_path);
    ~RecordStore() override;

    RecordStore(const RecordStore&) = delete;
    RecordStore& operator=(const RecordStore&) = delete;

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

    // Retention and cleanup for the keys this node owns: removes expired records,
    // deletion tombstones and stale index entries older than the grace period.
    // Local writes only (they replicate by gossip). Call from the owner loop.
    void sweep(std::int64_t now_ms);

    // The journal (durability before acknowledgement). replay_journal runs once
    // at startup, after the persisted document is restored. journal_mark is taken
    // just before the document is saved and journal_truncate(mark) after the save
    // is on disk, dropping the entries the saved document already holds.
    std::size_t replay_journal();
    std::uint64_t journal_mark() const;
    void journal_truncate(std::uint64_t mark);

    // The node that orders writes of (collection, key) in this node's view.
    std::string owner_of(const std::string& collection, const std::string& key) const;

    zetta::cluster::SignalResponse on_request(const zetta::cluster::Signal& request) override;
    void on_event(const zetta::cluster::Signal& event) override;
    void on_task(const zetta::cluster::Signal& task) override;

    // How long a deletion tombstone or a stale index entry is kept before the
    // owner removes it: longer than any write can be in flight.
    static constexpr std::int64_t kTombstoneGraceMs = 10 * 60 * 1000;

private:
    class Journal;

    RecordOutcome commit(const RecordWrite& write, std::int64_t now_ms);
    bool install(const StoredRecord& record);
    bool install_collection(const RecordCollection& collection);
    void replicate(const nlohmann::json& payload, const std::string& except,
                   RecordOutcome& outcome);
    std::vector<std::string> live_peers() const;

    zetta::cluster::ReplicationManager& replication_;
    std::string node_id_;
    std::unique_ptr<Journal> journal_;
    std::atomic<zetta::cluster::Cluster*> cluster_{nullptr};
    std::atomic<zetta::cluster::SignalHandler*> delegate_{nullptr};
};

}  // namespace zetta::data
