#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/signal.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/util/sha256.hpp"

namespace zetta::cluster {
class Cluster;
class ReplicationManager;
}

// Backup snapshots (zetta#1518, docs/data/backup-snapshot.md): one consistent
// point-in-time copy of a Zetta's data as one archive, which the Orama
// operator's backup Job streams to the bucket it names (the engine never holds
// the bucket's credentials), and the restore of that archive into an empty
// Zetta, verified against the manifest the archive carries.
//
// What a snapshot holds:
//   * the replicated catalogue as of one moment, every family except the
//     cluster's own membership (`node:`), the object leases (`object_lease:`),
//     retired audit (`audit:`) and the identity records (users, roles, tokens,
//     role assignments, row-security policies, principals, access keys), which
//     are sealed under the source cluster's identity root and stay with it;
//   * every record of every record collection, the engine-internal ones
//     (`_entity_keys`, `_metric.<name>`) included;
//   * every object version of every bucket;
//   * every large-object body and model weights body the catalogue names.
// A partition-only table whose load had not finished (no partition manifest) is
// left out whole, so a table is never restored torn.
//
// Consistency. While it captures, the snapshot freezes the record store's and
// the object store's write gates (WriteGate): writes arriving then are refused
// with Unavailable and retried by their callers. Under the freeze it reads the
// catalogue on the node's owner loop (between two owner tasks, so no statement
// is half applied), writes every record and pins a reader on every object
// version; then it thaws and copies the object and body bytes from what it
// pinned. Bodies are immutable once written.
//
// The archive (`zetta-backup/1`): a sequence of frames, each one JSON header
// line (`kind`, `bytes`) followed by `bytes` of payload: `header`, `catalogue`,
// `records` (JSON lines, at most 1,000 a frame), `object` (raw bytes), `body`
// (raw bytes), `manifest` (what the snapshot counted), and last `end`, whose
// `sha256` covers every byte before it, so a truncated or damaged archive is
// refused.
namespace zetta::data {

class BlobStore;
class RecordStore;

inline constexpr std::string_view kBackupFormat = "zetta-backup/1";

// What a snapshot or a restore counted, by stable name:
//   table:<database>.<table>   rows (rows in the catalogue, plus the rows of
//                              every partition whose body is present)
//   bucket:<name>              objects
//   collection:<name>          records, for every collection holding any
//   catalogue:<family>         live catalogue entries of each family copied
//                              (record collections, metrics and buckets are
//                              counted by their contents instead)
//   bodies                     large-object and weights bodies present
//   bodies:missing             bodies the catalogue names that are gone for
//                              good (absent when none)
using BackupSummary = std::map<std::string, std::int64_t>;

enum class BackupStatus {
    Ok,
    InvalidInput,        // a malformed request or archive
    NotFound,            // no such snapshot (released or expired)
    Conflict,            // the node's state refuses it (not empty, not a restore target)
    Busy,                // another snapshot or restore is running; retry
    VerificationFailed,  // the restored counts are not the manifest's
    Failed,              // the engine could not read or write something
};

// The wire codes the REST routes answer: ok, invalid_input, not_found,
// conflict, busy, verification_failed, engine_error.
std::string_view to_string(BackupStatus status);

struct BackupOutcome {
    BackupStatus status = BackupStatus::Ok;
    std::string message;
    std::string snapshot;       // the snapshot's id
    BackupSummary summary;      // what was counted
    std::uint64_t size_bytes = 0;
    std::filesystem::path archive;
    nlohmann::json details;     // structured detail for a refusal

    bool ok() const { return status == BackupStatus::Ok; }
};

// The stores a snapshot reads and a restore writes, all of one node.
struct BackupStores {
    zetta::cluster::ReplicationManager& replication;
    RecordStore& records;
    ObjectStore& objects;
    BlobStore& bodies;
    std::string node_id;
    // The cluster this node is a member of; null on a standalone node. A body
    // only other members hold is fetched from them through it (zetta#1520).
    zetta::cluster::Cluster* cluster = nullptr;
};

// Whether a catalogue key is copied into a snapshot (and written by a restore).
bool backed_up_key(std::string_view key);

nlohmann::json summary_to_json(const BackupSummary& summary);
std::optional<BackupSummary> summary_from_json(const nlohmann::json& value);

// Count a catalogue (already filtered by backed_up_key) and the stores' contents.
BackupSummary summarize(const nlohmann::json& catalogue,
                        const std::function<bool(const std::string& body_id)>& body_present,
                        const std::map<std::string, std::int64_t>& objects_per_bucket,
                        const std::map<std::string, std::int64_t>& records_per_collection);

// The node as it is now, counted the same way. Lists every bucket and every
// record collection, so it blocks: run it on the blocking pool.
BackupSummary summarize_node(BackupStores& stores);

// Whether a summary counts anything: a node that holds user data.
bool holds_user_data(const BackupSummary& summary);

// Each name whose count differs, as {"<name>": {"expected": n, "observed": m}};
// a name absent on one side counts 0. Empty when they agree.
nlohmann::json summary_differences(const BackupSummary& expected, const BackupSummary& observed);

// Holds both write gates frozen for as long as it lives.
// Holds the record and object write gates frozen on this node and, on a
// cluster, on every other member (zetta#1520), for as long as it lives. Every
// gate is frozen for a bounded lease (kLease) and lapses by itself after it, so
// a lost thaw or a dead coordinator never leaves a member refusing writes.
// Destroyed, it thaws its own gates and sends every member a thaw.
class BackupFreeze {
public:
    static constexpr std::chrono::seconds kLease{120};

    // This node only (a standalone node, and tests). Nullopt when either gate
    // is held by another snapshot.
    static std::optional<BackupFreeze> begin(RecordStore& records, ObjectStore& objects);

    // This node and every member `cluster` knows (null: this node only), all
    // as `owner` (the snapshot's id). Busy when any member's gate is held by
    // another snapshot, or a member does not answer (an older version without
    // `backup.gate` among them): every gate already frozen is released then.
    // Blocks on the signal plane: run it on the blocking pool.
    static BackupOutcome begin(RecordStore& records, ObjectStore& objects,
                               zetta::cluster::Cluster* cluster, std::string owner,
                               std::optional<BackupFreeze>& out);

    BackupFreeze(BackupFreeze&& other) noexcept;
    BackupFreeze& operator=(BackupFreeze&&) = delete;
    BackupFreeze(const BackupFreeze&) = delete;
    BackupFreeze& operator=(const BackupFreeze&) = delete;
    ~BackupFreeze();

    // Every write that passed this node's gates before the freeze has finished.
    bool drained() const;
    // Every member's too: `all` is set when they have. Not ok when a member
    // stops answering or lost its freeze. Blocks: run it on the blocking pool.
    BackupOutcome members_drained(bool& all);
    // Whether every gate, here and on every member, is still held by this
    // freeze: asked after the capture, so a freeze that lapsed during it
    // refuses the snapshot. Blocks: run it on the blocking pool.
    BackupOutcome held();

    const std::vector<std::string>& members() const { return members_; }

private:
    BackupFreeze(RecordStore& records, ObjectStore& objects, std::string owner);
    BackupOutcome ask_members(std::string_view op, bool& all_drained, bool& all_held);
    RecordStore* records_;
    ObjectStore* objects_;
    zetta::cluster::Cluster* cluster_ = nullptr;
    std::string owner_;
    std::vector<std::string> members_;  // the other members frozen
};

// The backup snapshot's part of the signal plane on every node (zetta#1520):
//   backup.gate   {"op": "freeze"|"status"|"thaw", "owner", "lease_ms"} — freeze
//                 or thaw this node's record and object write gates for a
//                 snapshot another member takes; answers {"drained", "held"}.
//                 A thaw also arrives as an event (fire and forget).
//   backup.body   {"id", "offset", "length"} — a slice of a large-object or
//                 weights body this node holds; answers {"size"} and the bytes.
// It chains over the cluster's current handler, like the stores; Node wires it
// in after the object store.
class BackupPeer : public zetta::cluster::SignalHandler {
public:
    static constexpr std::string_view kGateMethod = "backup.gate";
    static constexpr std::string_view kBodyMethod = "backup.body";
    static constexpr std::uint64_t kMaxSliceBytes = ObjectStore::kSliceBytes;

    BackupPeer(RecordStore& records, ObjectStore& objects,
               zetta::cluster::ReplicationManager& replication, std::filesystem::path data_dir);
    ~BackupPeer() override;
    BackupPeer(const BackupPeer&) = delete;
    BackupPeer& operator=(const BackupPeer&) = delete;

    void attach(zetta::cluster::Cluster& cluster);
    void detach();

    zetta::cluster::SignalResponse on_request(const zetta::cluster::Signal& request) override;
    void on_event(const zetta::cluster::Signal& event) override;
    void on_task(const zetta::cluster::Signal& task) override;

private:
    zetta::cluster::SignalResponse gate(const nlohmann::json& request);
    zetta::cluster::SignalResponse body(const nlohmann::json& request);

    RecordStore& records_;
    ObjectStore& objects_;
    ReplicatedBlobStore bodies_;
    std::atomic<zetta::cluster::Cluster*> cluster_{nullptr};
    std::atomic<zetta::cluster::SignalHandler*> delegate_{nullptr};
};

// Fetch the whole body `id` from one of `owners` over backup.body, a slice at a
// time, verified against its catalogue entry's size and SHA-256 (`size`,
// `sha256`; an empty hash is not checked). Nullopt when no owner gives it whole.
// Blocks on the signal plane: run it on the blocking pool.
std::optional<std::string> fetch_body(zetta::cluster::Cluster& cluster,
                                      const std::vector<std::string>& owners,
                                      const std::string& id, std::uint64_t size,
                                      const std::string& sha256);

// Writes one snapshot under `<directory>/<id>/`: `archive`, and `snapshot.json`
// once it is complete. Destroyed before finish() succeeds, it removes what it
// wrote.
class SnapshotWriter {
public:
    SnapshotWriter(BackupStores& stores, std::filesystem::path directory, std::string id);
    ~SnapshotWriter();
    SnapshotWriter(const SnapshotWriter&) = delete;
    SnapshotWriter& operator=(const SnapshotWriter&) = delete;

    // Under the freeze: write the header, the catalogue and every record, and
    // pin a reader on every object version. Blocks: run it on the blocking pool.
    BackupOutcome capture(std::shared_ptr<const nlohmann::json> catalogue);

    // After the freeze: the objects' and bodies' bytes, the manifest and the
    // trailer. Blocks: run it on the blocking pool.
    BackupOutcome finish();

private:
    struct PinnedObject {
        std::string bucket;
        std::string key;
        std::uint64_t generation = 0;
        std::int64_t updated_ms = 0;
        std::shared_ptr<ObjectReader> reader;
    };

    bool write_header(nlohmann::json header, std::uint64_t bytes);
    bool write_bytes(std::string_view bytes);
    bool write_frame(nlohmann::json header, std::string_view payload);
    BackupOutcome failed(std::string message);

    BackupStores& stores_;
    std::filesystem::path folder_;
    std::string id_;
    std::ofstream out_;
    zetta::util::Sha256 hash_;
    std::uint64_t written_ = 0;
    bool finished_ = false;
    nlohmann::json catalogue_ = nlohmann::json::object();  // what was copied
    std::vector<std::string> skipped_tables_;
    std::vector<PinnedObject> objects_;
    std::map<std::string, std::int64_t> objects_per_bucket_;
    std::map<std::string, std::int64_t> records_per_collection_;
    std::int64_t taken_ms_ = 0;
};

// The snapshots this node holds, under `<data_dir>/backups/`. Each is kept until
// released or until kRetention after it was taken. Stateless over the directory,
// so any thread may use one.
class BackupSnapshots {
public:
    static constexpr std::chrono::hours kRetention{6};

    explicit BackupSnapshots(std::filesystem::path directory);
    const std::filesystem::path& directory() const { return directory_; }

    // A complete snapshot: `archive`, `size_bytes` and `summary`. NotFound once
    // released or expired.
    BackupOutcome find(const std::string& id) const;
    // Remove it. NotFound once released or expired.
    BackupOutcome release(const std::string& id);
    // Remove every snapshot older than kRetention, and every half-written one
    // left that long; how many were removed.
    std::size_t expire(std::chrono::system_clock::time_point now);

    // A snapshot id: 36 characters of lower-case hex and dashes, so it can
    // never name a path outside the directory.
    static bool valid_id(std::string_view id);

private:
    std::filesystem::path directory_;
};

// Where a node records the restore it took, `<data_dir>/backups/restore.json`:
// the snapshot, its source node, and the summary counted when it finished.
// Discard reads it.
std::filesystem::path restore_record_path(const std::filesystem::path& data_dir);

// Restores one archive, fed a slice at a time as it arrives. begin() first; it
// refuses a node that holds user data. feed() applies each frame as it
// completes; finish() checks the trailer, counts the node and compares the
// counts with the archive's manifest. One restore at a time per node (the
// caller guarantees it). All three block: run them on the blocking pool.
class SnapshotRestore {
public:
    SnapshotRestore(BackupStores& stores, std::filesystem::path record_path);

    BackupOutcome begin();
    BackupOutcome feed(std::string_view bytes);
    BackupOutcome finish();

    // Whether anything was written to the node yet.
    bool wrote() const { return wrote_; }

private:
    enum class Stage { Header, Payload, Done };

    BackupOutcome header_line(std::string_view line);
    BackupOutcome payload(std::string_view bytes);
    BackupOutcome complete_frame();
    BackupOutcome apply_catalogue();
    BackupOutcome apply_records();
    BackupOutcome object_bytes(std::string_view bytes);
    BackupOutcome failed(BackupStatus status, std::string message);
    bool write_record(const nlohmann::json& state);

    BackupStores& stores_;
    std::filesystem::path record_path_;
    Stage stage_ = Stage::Header;
    std::string pending_;
    nlohmann::json header_;
    std::string kind_;
    std::uint64_t remaining_ = 0;
    zetta::util::Sha256 hash_;
    bool saw_header_ = false;
    bool saw_catalogue_ = false;
    bool saw_manifest_ = false;
    bool wrote_ = false;
    bool failed_ = false;
    std::string snapshot_;
    std::string source_node_;
    BackupSummary expected_;
    std::int64_t started_ms_ = 0;
    // The object being streamed: a whole put when it fits one slice, else an upload.
    std::string object_bucket_;
    std::string object_key_;
    std::uint64_t object_size_ = 0;
    std::string object_slice_;
    std::optional<ObjectUpload> upload_;
    std::set<std::string> restored_buckets_;
    // Records past their retention when restored: not written, by collection.
    std::map<std::string, std::int64_t> expired_;
};

// What a discard names: this node, and the snapshot it was restored from.
struct DiscardRequest {
    std::string node_id;
    std::string snapshot;
};

// Empty a scratch Zetta after a verified restore, so the next rehearsal into it
// finds it empty. Refused unless every one of these holds: the node was started
// as a restore target (`restore_target`: --restore-target or
// ZETTA_RESTORE_TARGET), it holds a restore record, the request names this node
// and that record's snapshot, and the node still holds exactly what the restore
// counted (nothing written since). It removes every object, every record and
// every catalogue entry a restore writes (record collection and metric
// definitions stay), then the restore record. Blocks: run it on the blocking pool.
BackupOutcome discard_restored(BackupStores& stores, const std::filesystem::path& record_path,
                               bool restore_target, const DiscardRequest& request);

}  // namespace zetta::data
