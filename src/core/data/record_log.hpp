// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/record_store.hpp"

// Internal: the node-local storage under the keyed record store (#1231,
// docs/data/record-store.md → "Storage").
//
// Records do NOT live in the replicated Automerge document: it keeps every write
// in its history and saves the whole document, so a steady write load made the
// catalogue grow without bound (the soak measured it). Each node instead keeps
// its copy of every record in an append-only log of segments under
// `<data_dir>/records/`, with an in-memory key directory (Bitcask's design):
//
//   * a write appends one checksummed frame to the active segment and points the
//     key directory at it; values stay on disk and are read with one pread;
//   * a purge appends a small marker, so a restart never revives an older frame;
//   * the active segment rolls at kSegmentBytes; `maintain` deletes dead sealed
//     segments oldest-first and moves the live frames of a mostly-dead oldest
//     segment forward, a bounded batch per call, so disk use follows the live set;
//   * startup scans the segments in order and rebuilds the directory, the
//     secondary indexes, the expiry queue and the anti-entropy digests.
//
// Thread safety: one shared mutex — the lowest-level handoff inside this file
// adapter. Reads (get, scans, digests) take it shared; a write takes it
// exclusively for the append and the directory change, and fsyncs AFTER
// releasing it, so a read never waits for a disk flush.
namespace zetta::data {

// The record value a frame and a signal carry, and its non-throwing parse.
nlohmann::json stored_value(const StoredRecord& record);
std::optional<StoredRecord> parse_stored(std::string collection, std::string key,
                                         const nlohmann::json& value);

// One key's anti-entropy summary: what a peer compares against its own copy.
struct RecordVersion {
    std::string key;
    std::uint64_t revision = 0;
    bool deleted = false;
    std::int64_t updated_ms = 0;
    std::optional<std::int64_t> expires_ms;
};

class RecordLog {
public:
    static constexpr std::size_t kBuckets = 256;
    static constexpr std::uint64_t kSegmentBytes = 32ull * 1024 * 1024;

    // `directory` empty keeps everything in memory (tests and benchmarks that
    // need no durability). `tombstone_grace_ms` is how long a deletion is kept.
    RecordLog(std::filesystem::path directory, std::int64_t tombstone_grace_ms,
              std::uint64_t segment_bytes = kSegmentBytes);
    ~RecordLog();

    RecordLog(const RecordLog&) = delete;
    RecordLog& operator=(const RecordLog&) = delete;

    // Scan the segments and rebuild. False (with a reason) when the directory
    // cannot be created or a segment cannot be opened.
    bool open(std::string* reason);

    // The conditional write. Under the exclusive lock, `decide` sees the key's
    // current record (null when absent; its `value` filled only when
    // `with_value`) and returns the record to write, or nullopt to write nothing.
    // `sync` fsyncs the segment before returning. `durable` reports whether that
    // flush succeeded.
    struct Written {
        std::optional<StoredRecord> record;
        bool durable = true;
    };
    using Decide = std::function<std::optional<StoredRecord>(const StoredRecord* current)>;
    Written update(const std::string& collection, const std::string& key,
                   const Decide& decide, bool with_value, bool sync);

    std::optional<StoredRecord> get(const std::string& collection, const std::string& key,
                                    bool with_value) const;

    // Ordered walks, values loaded; the visitor returns false to stop.
    // Index walks order by (order, key) within one index value; `after` is
    // exclusive. Key walks order by key.
    using Visitor = std::function<bool(StoredRecord&&)>;
    void scan_index(const std::string& collection, const std::string& index,
                    const std::string& value,
                    const std::optional<std::pair<std::string, std::string>>& after,
                    bool descending, const Visitor& visit) const;
    void scan_keys(const std::string& collection, const std::optional<std::string>& from,
                   bool inclusive, bool descending, const Visitor& visit) const;

    // Remove the key when its live revision is still `revision`.
    bool purge(const std::string& collection, const std::string& key, std::uint64_t revision);

    // Take the keys whose expiry, or whose deletion grace, has passed (at most
    // `limit`; the rest stay queued). Entries a later write superseded are dropped.
    struct Due {
        std::string collection;
        std::string key;
        std::uint64_t revision = 0;
    };
    std::vector<Due> take_due(std::int64_t now_ms, std::size_t limit);

    // Anti-entropy: per collection, kBuckets XOR digests of (key, revision,
    // deleted); and the versions of the keys in some buckets.
    std::map<std::string, std::array<std::uint64_t, kBuckets>> digests() const;
    std::vector<RecordVersion> versions(const std::string& collection,
                                        const std::vector<std::size_t>& buckets) const;
    static std::size_t bucket_of(const std::string& key);

    // Flush unsynced appends (replica installs), then a bounded step of segment
    // cleanup and compaction.
    void maintain();

    RecordStorageStats stats() const;

private:
    struct Segment;
    // One key's directory entry: the record's metadata and where its frame is.
    struct Entry {
        std::uint64_t revision = 0;
        std::int64_t updated_ms = 0;
        std::optional<std::int64_t> expires_ms;
        bool deleted = false;
        std::map<std::string, std::string> index;
        std::string order;
        std::uint32_t segment = 0;
        std::uint64_t offset = 0;
        std::uint32_t length = 0;
    };
    struct Collection {
        std::map<std::string, Entry> records;
        // index name + '\0' + indexed value -> (order, key), in walk order
        std::map<std::string, std::set<std::pair<std::string, std::string>>> indexes;
        std::array<std::uint64_t, kBuckets> digests{};
    };

    std::optional<StoredRecord> load_locked(const std::string& collection, const std::string& key,
                                            const Entry& entry, bool with_value) const;
    bool append_locked(const std::string& frame, std::uint32_t& segment, std::uint64_t& offset,
                       std::shared_ptr<Segment>& written_to);
    void place_locked(Collection& collection, const std::string& name, const std::string& key,
                      Entry entry);
    void erase_locked(Collection& collection, const std::string& key);
    bool roll_locked();
    void compact_step();

    std::filesystem::path directory_;
    std::int64_t tombstone_grace_ms_;
    std::uint64_t segment_bytes_;
    mutable std::shared_mutex mutex_;
    std::map<std::string, Collection> collections_;
    std::map<std::uint32_t, std::shared_ptr<Segment>> segments_;
    std::uint32_t active_ = 0;
    std::multimap<std::int64_t, Due> due_;
    std::uint64_t compaction_offset_ = 0;
    std::mutex maintenance_;  // one maintain() at a time
    std::uint64_t index_bytes_ = 0;
};

}  // namespace zetta::data
