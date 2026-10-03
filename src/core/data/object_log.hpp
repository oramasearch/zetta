#pragma once

#include <array>
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

#include "zetta/data/object_store.hpp"

// Internal: the node-local storage of ONE object bucket (#1005,
// docs/data/object-store.md → "Storage").
//
// Object bytes never enter the replicated Automerge document (#1231 measured
// what a steady write load does to it). Each node keeps its copy of a bucket's
// objects in an append-only log of segments under `<data_dir>/objects/<bucket>/`,
// with an in-memory directory, in the record log's design:
//
//   * FRAMES. `u32 header length, u32 header crc32c, header, payload`. The header
//     names the frame's kind, the key, its generation or upload id, offsets, the
//     write time and the payload's crc32c. Payloads are at most kSliceBytes.
//   * VERSIONS. Every version of a key has a GENERATION; a later version always
//     has a higher one (max(previous + 1, now in microseconds)), so the directory
//     is the highest generation of each key whatever order frames are replayed in.
//     An APPEND data frame both carries bytes and establishes its generation; a
//     STAGE data frame belongs to an upload and becomes visible only when a COMMIT
//     frame names that upload; a REMOVE frame is a tombstone; a PURGE frame drops
//     every version at or below its generation (expired tombstones, and copies a
//     node no longer owns).
//   * ATOMIC PUT. A put writes its stage frames, then one commit frame. A reader
//     sees the old version until the commit is in the directory, then the new
//     one; a crash before the commit frame is on disk leaves the old version.
//   * DURABILITY. A write is acknowledged after write(2); `sync` (the barrier)
//     fsyncs every segment written since the last one, and `maintain` fsyncs a
//     segment the log has rolled past, so only the last two can hold bytes never
//     fsynced. Startup verifies their payload checksums and truncates the last at
//     the first torn frame.
//   * COMPACTION. `compact_step` retires dead segments oldest-first and moves the
//     live frames of mostly-dead oldest segments forward, within a byte budget a
//     call, taking the lock once per batch.
//
// Thread safety: the lowest-level handoffs inside this file adapter. Writers
// serialize on a write mutex; the directory's shared mutex is held exclusively
// only to decide and to publish, never across write(2). Reads resolve a snapshot
// of the pieces under the shared lock and pread outside it (a segment's
// descriptor lives as long as a snapshot holds it).
namespace zetta::data {

// crc32c (Castagnoli), software slicing-by-8.
std::uint32_t crc32c(std::string_view bytes, std::uint32_t crc = 0);

// One key's anti-entropy summary.
struct ObjectVersion {
    std::string key;
    std::uint64_t generation = 0;
    std::uint64_t size = 0;
    bool deleted = false;
    std::int64_t updated_ms = 0;
};

class ObjectLog {
public:
    static constexpr std::size_t kBuckets = 256;
    static constexpr std::uint64_t kSegmentBytes = 64ull * 1024 * 1024;
    static constexpr std::uint32_t kSliceBytes = 4u * 1024 * 1024;

    struct Segment;

    // One stretch of an object's bytes inside a segment.
    struct Piece {
        std::shared_ptr<Segment> segment;
        std::uint64_t file_offset = 0;  // the payload's first byte in the segment
        std::uint64_t length = 0;
    };

    // What a read resolved: the version and the pieces covering the range.
    struct ReadSnapshot {
        ObjectVersion version;
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        std::vector<Piece> pieces;  // in object order, exactly covering the range
        // Copy `length` bytes from `pieces` into `out` (resized). False on an I/O error.
        bool read_into(std::uint64_t from, std::uint64_t count, std::string& out) const;
    };

    enum class Result {
        Ok,         // written
        Unchanged,  // a retried append whose bytes were already there
        Conflict,   // the condition failed; `version` is the live one
        Gap,        // a replica append past its end: it needs repair first
        Stale,      // a replica install older than what is held: nothing to do
        NotFound,   // the key has no live version
        Io,         // the segment could not be written
    };
    struct Written {
        Result result = Result::Ok;
        std::optional<ObjectVersion> version;  // after the write; on Conflict the live one
        bool durable = true;                   // with `sync`: the fsync succeeded
    };

    // `directory` empty keeps the segments in memory (tests and benchmarks).
    ObjectLog(std::filesystem::path directory, std::int64_t tombstone_grace_ms,
              std::uint64_t segment_bytes = kSegmentBytes);
    ~ObjectLog();

    ObjectLog(const ObjectLog&) = delete;
    ObjectLog& operator=(const ObjectLog&) = delete;

    // Scan the segments and rebuild the directory. False (with a reason) when the
    // directory cannot be created or a segment cannot be opened.
    bool open(std::string* reason);

    // The live version, or the tombstone, of `key`.
    std::optional<ObjectVersion> stat(const std::string& key) const;

    // ---- the ordering node (it decides) ----------------------------------------

    // Append at exactly `offset`. Same generation and offset == size: appended.
    // offset + bytes inside the object and equal to the bytes there: Unchanged.
    // Absent and offset 0: a new generation, at least `fresh`. Anything else:
    // Conflict carrying the live version (size 0 when absent).
    Written append(const std::string& key, std::uint64_t offset, std::string_view bytes,
                   std::uint64_t fresh, bool sync);
    // A whole object in one frame (at most kSliceBytes). `if_absent`: Conflict when live.
    Written put(const std::string& key, std::string_view bytes, std::uint64_t fresh,
                bool if_absent, bool sync);
    // Commit a staged upload as the new version: Conflict when `if_absent` and live,
    // NotFound when the upload's bytes are not all here.
    Written commit(const std::string& key, std::uint64_t upload, std::uint64_t size,
                   std::uint64_t fresh, bool if_absent, bool sync);
    // Tombstone the live version; NotFound when there is none.
    Written remove(const std::string& key, std::uint64_t fresh, bool sync);

    // ---- uploads ---------------------------------------------------------------

    // Stage `bytes` of upload `upload` at `offset` (uploads are written in order).
    Written stage(const std::string& key, std::uint64_t upload, std::uint64_t offset,
                  std::string_view bytes, bool sync);
    // Forget an upload; its frames are dead.
    void abort(std::uint64_t upload);
    // Drop uploads idle since before `idle_before_ms`.
    void expire_uploads(std::int64_t idle_before_ms);
    std::uint64_t staged_bytes() const;

    // ---- replicas (they install what the ordering node decided) ----------------

    Written apply_append(const std::string& key, std::uint64_t generation,
                         std::uint64_t offset, std::string_view bytes, bool sync);
    Written apply_put(const std::string& key, std::uint64_t generation, std::string_view bytes,
                      bool sync);
    Written apply_commit(const std::string& key, std::uint64_t upload, std::uint64_t generation,
                         std::uint64_t size, bool sync);
    Written apply_remove(const std::string& key, std::uint64_t generation,
                         std::int64_t updated_ms, bool sync);
    // Drop this node's copy of `key` at or below `generation` (it is no longer an
    // owner, or a tombstone's grace has passed). Leaves no tombstone.
    bool purge(const std::string& key, std::uint64_t generation);

    // ---- reads -----------------------------------------------------------------

    enum class ReadStatus { Ok, NotFound, OutOfRange };
    // Resolve [offset, offset + length) of the live version; `length` absent: to
    // the end. An empty object reads as Ok with no bytes.
    ReadStatus snapshot(const std::string& key, std::uint64_t offset,
                        std::optional<std::uint64_t> length, ReadSnapshot& out) const;
    // A range of a specific generation (anti-entropy repair, remote reads).
    ReadStatus snapshot_generation(const std::string& key, std::uint64_t generation,
                                   std::uint64_t offset, std::uint64_t length,
                                   ReadSnapshot& out) const;

    // Objects in key order after `after` (exclusive) whose key starts with
    // `prefix`, tombstones too when `tombstones`; `visit` returns false to stop.
    void list(const std::string& prefix, const std::string& after,
              const std::function<bool(const ObjectVersion&)>& visit,
              bool tombstones = false) const;
    // Every version held, tombstones included.
    void each(const std::function<void(const ObjectVersion&)>& visit) const;

    // Anti-entropy: kBuckets XOR digests of (key, generation, size, deleted) over
    // the keys `keep` admits, and the versions in some buckets.
    std::array<std::uint64_t, kBuckets> digests(
        const std::function<bool(const std::string& key)>& keep) const;
    std::vector<ObjectVersion> versions(const std::vector<std::size_t>& buckets,
                                        const std::function<bool(const std::string&)>& keep) const;
    static std::size_t bucket_of(const std::string& key);

    // Live objects and their bytes (tombstones excluded).
    std::uint64_t object_count() const;
    std::uint64_t object_bytes() const;

    // The barrier: fsync every segment written since the last sync.
    bool sync();
    // Tombstones past their grace: purge them. Then a bounded step of segment
    // cleanup and compaction.
    void maintain(std::int64_t now_ms);

    struct Stats {
        std::uint64_t objects = 0;     // live
        std::uint64_t tombstones = 0;
        std::uint64_t segments = 0;
        std::uint64_t disk_bytes = 0;  // every segment's size
        std::uint64_t live_bytes = 0;  // frames the directory points at
        std::uint64_t memory_bytes = 0;  // estimate: directory and uploads
    };
    Stats stats() const;

private:
    struct Location {
        std::uint32_t segment = 0;
        std::uint64_t frame_offset = 0;
        std::uint32_t frame_length = 0;
    };
    struct Extent {
        std::uint64_t object_offset = 0;
        std::uint64_t length = 0;
        std::uint64_t file_offset = 0;  // payload start in the segment
        Location frame;
    };
    struct Entry {
        std::uint64_t generation = 0;
        std::uint64_t upload = 0;  // the committed upload, 0 when none
        std::uint64_t size = 0;
        bool deleted = false;
        std::int64_t updated_ms = 0;
        std::vector<Extent> extents;   // in object order, contiguous from 0
        std::optional<Location> marker;  // the commit or remove frame
    };
    struct Upload {
        std::string key;
        std::vector<Extent> extents;
        std::uint64_t size = 0;
        std::int64_t touched_ms = 0;
    };
    struct Frame;

    bool roll_locked();
    // Append a frame at the active segment's end (rolling it when full). Called
    // holding write_mutex_ and `lock`; releases `lock` for the write itself.
    bool append_locked(std::unique_lock<std::shared_mutex>& lock, const Frame& frame,
                       std::string_view payload, Location& at, std::uint64_t& payload_offset,
                       std::shared_ptr<Segment>& written_to);
    void account_locked(const Location& at, bool live);
    void place_locked(const std::string& key, Entry entry);
    void erase_locked(const std::string& key);
    bool equal_locked(const Entry& entry, std::uint64_t offset, std::string_view bytes) const;
    bool covering(const std::vector<Extent>& extents, std::uint64_t size) const;
    Written finish(Written written, std::shared_ptr<Segment> segment, bool sync);
    ReadStatus snapshot_locked(const Entry& entry, std::uint64_t offset, std::uint64_t length,
                               const std::string& key, ReadSnapshot& out) const;
    // What a frame backs right now: an object's extent, an upload's extent, or
    // an object's commit / remove / empty-object marker. Nothing: the frame is dead.
    struct Backing {
        Extent* extent = nullptr;
        Location* marker = nullptr;
        bool staged = false;  // an upload's extent, not yet counted live
    };
    Backing backing_locked(const Frame& frame, const Location& at);
    void compact_step();

    std::filesystem::path directory_;
    std::int64_t tombstone_grace_ms_;
    std::uint64_t segment_bytes_;
    mutable std::shared_mutex mutex_;
    // Writers, one at a time, for the whole of a write (decision, append,
    // directory change); `mutex_` is held only for the decision and the change.
    std::mutex write_mutex_;
    std::map<std::string, Entry> entries_;
    std::array<std::uint64_t, kBuckets> digests_{};
    std::map<std::uint64_t, Upload> uploads_;
    std::map<std::uint32_t, std::shared_ptr<Segment>> segments_;
    std::uint32_t active_ = 0;
    std::uint64_t live_objects_ = 0;
    std::uint64_t live_object_bytes_ = 0;
    std::uint64_t staged_bytes_ = 0;
    std::uint64_t extent_count_ = 0;
    std::uint64_t compaction_offset_ = 0;
    std::mutex maintenance_;  // one maintain() at a time
};

}  // namespace zetta::data
