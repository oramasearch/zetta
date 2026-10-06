#include "object_log.hpp"

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <system_error>
#include <utility>

// The node-local object log (#1005). See object_log.hpp.
namespace zetta::data {
namespace {

// ---- checksums ------------------------------------------------------------------

using CrcTables = std::array<std::array<std::uint32_t, 256>, 8>;

const CrcTables& crc_tables() {
    static const CrcTables tables = [] {
        CrcTables values{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? 0x82F63B78u ^ (value >> 1) : value >> 1;
            values[0][index] = value;
        }
        for (std::uint32_t index = 0; index < 256; ++index) {
            for (std::size_t table = 1; table < 8; ++table) {
                const std::uint32_t previous = values[table - 1][index];
                values[table][index] = (previous >> 8) ^ values[0][previous & 0xFF];
            }
        }
        return values;
    }();
    return tables;
}

std::uint64_t fnv64(std::string_view text, std::uint64_t hash = 1469598103934665603ull) {
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t mix64(std::uint64_t hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFF;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t version_hash(const std::string& key, std::uint64_t generation, std::uint64_t size,
                           bool deleted) {
    std::uint64_t hash = fnv64(key);
    hash = mix64(hash, generation);
    hash = mix64(hash, deleted ? ~0ull : size);
    return hash | 1;  // never zero, so an XOR in and out always cancels exactly
}

// ---- little-endian encoding ---------------------------------------------------

void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>(value >> 8));
}

void put_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

void put_u64(std::string& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

std::uint64_t get_le(const char* bytes, int width) {
    std::uint64_t value = 0;
    for (int index = width - 1; index >= 0; --index) {
        value = (value << 8) | static_cast<unsigned char>(bytes[index]);
    }
    return value;
}

constexpr std::uint8_t kData = 1;
constexpr std::uint8_t kCommit = 2;
constexpr std::uint8_t kRemove = 3;
constexpr std::uint8_t kPurge = 4;
constexpr std::uint8_t kAppendFlag = 1;  // a data frame that establishes its generation
constexpr std::size_t kFixedHeader = 4 + 4 * 8 + 4 + 4;
constexpr std::uint32_t kMaxHeaderBytes = kFixedHeader + 1024;
constexpr std::uint64_t kCompactionChunk = 8ull * 1024 * 1024;
// The most a compaction group writes under one exclusive hold of the log's lock:
// readers wait at most one such write.
constexpr std::uint64_t kCompactionGroup = 1024 * 1024;

std::int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::filesystem::path segment_path(const std::filesystem::path& directory, std::uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "segment-%08u.log", id);
    return directory / name;
}


}  // namespace

std::uint32_t crc32c(std::string_view bytes, std::uint32_t crc) {
    const CrcTables& tables = crc_tables();
    crc = ~crc;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t length = bytes.size();
    while (length >= 8) {
        const std::uint32_t low = crc ^ (static_cast<std::uint32_t>(data[0]) |
                                         static_cast<std::uint32_t>(data[1]) << 8 |
                                         static_cast<std::uint32_t>(data[2]) << 16 |
                                         static_cast<std::uint32_t>(data[3]) << 24);
        crc = tables[7][low & 0xFF] ^ tables[6][(low >> 8) & 0xFF] ^
              tables[5][(low >> 16) & 0xFF] ^ tables[4][low >> 24] ^ tables[3][data[4]] ^
              tables[2][data[5]] ^ tables[1][data[6]] ^ tables[0][data[7]];
        data += 8;
        length -= 8;
    }
    while (length-- > 0) crc = tables[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// One frame's header.
struct ObjectLog::Frame {
    std::uint8_t type = kData;
    std::uint8_t flags = 0;
    std::string key;
    std::uint64_t a = 0;  // data: generation (append) or upload (stage); commit/remove/purge: generation
    std::uint64_t b = 0;  // data: object offset; commit: upload
    std::uint64_t c = 0;  // commit: size
    std::int64_t t = 0;   // write time, ms
    std::uint32_t payload_length = 0;
    std::uint32_t payload_crc = 0;

    std::string encode() const {
        std::string header;
        header.reserve(kFixedHeader + key.size());
        header.push_back(static_cast<char>(type));
        header.push_back(static_cast<char>(flags));
        put_u16(header, static_cast<std::uint16_t>(key.size()));
        put_u64(header, a);
        put_u64(header, b);
        put_u64(header, c);
        put_u64(header, static_cast<std::uint64_t>(t));
        put_u32(header, payload_length);
        put_u32(header, payload_crc);
        header += key;
        std::string out;
        out.reserve(8 + header.size());
        put_u32(out, static_cast<std::uint32_t>(header.size()));
        put_u32(out, crc32c(header));
        out += header;
        return out;
    }

    // `bytes` starts at a frame; on success fills this and returns the prefix
    // length (8 + header). 0 when there is not a whole valid header.
    std::size_t decode(std::string_view bytes) {
        if (bytes.size() < 8) return 0;
        const auto header_length = static_cast<std::uint32_t>(get_le(bytes.data(), 4));
        if (header_length < kFixedHeader || header_length > kMaxHeaderBytes ||
            bytes.size() < 8 + header_length) {
            return 0;
        }
        const std::string_view header = bytes.substr(8, header_length);
        if (crc32c(header) != static_cast<std::uint32_t>(get_le(bytes.data() + 4, 4))) return 0;
        const char* at = header.data();
        type = static_cast<std::uint8_t>(at[0]);
        flags = static_cast<std::uint8_t>(at[1]);
        const auto key_length = static_cast<std::size_t>(get_le(at + 2, 2));
        if (kFixedHeader + key_length != header_length) return 0;
        a = get_le(at + 4, 8);
        b = get_le(at + 12, 8);
        c = get_le(at + 20, 8);
        t = static_cast<std::int64_t>(get_le(at + 28, 8));
        payload_length = static_cast<std::uint32_t>(get_le(at + 36, 4));
        payload_crc = static_cast<std::uint32_t>(get_le(at + 40, 4));
        key.assign(at + kFixedHeader, key_length);
        if (type < kData || type > kPurge || payload_length > kSliceBytes) return 0;
        if (type != kData && payload_length != 0) return 0;
        return 8 + header_length;
    }
};

// One segment file (or, without a directory, an in-memory buffer). The
// descriptor closes with the last holder, so a read snapshot keeps a retired
// segment readable after compaction has unlinked its file.
struct ObjectLog::Segment {
    std::uint32_t id = 0;
    std::filesystem::path path;  // empty: in memory
    int descriptor = -1;
    std::atomic<std::uint64_t> size{0};  // written past by appends outside the log's lock
    std::uint64_t live_bytes = 0;
    // The bytes a completed fsync covered: set after the fsync to the size read
    // before it. A segment holds unsynced bytes while size > synced, whoever
    // else is fsyncing it.
    std::atomic<std::uint64_t> synced{0};
    // One fsync of this segment at a time: a caller arriving during another's
    // fsync waits for it, then fsyncs again only if it did not cover its bytes.
    std::mutex sync_mutex;
    // In-memory segments only: the bytes and the handoff that guards them (reads
    // happen outside the log's lock).
    mutable std::mutex memory_mutex;
    std::string memory;

    ~Segment() {
        if (descriptor >= 0) ::close(descriptor);
    }

    bool read(std::uint64_t offset, std::uint64_t length, char* out) const {
        if (path.empty()) {
            const std::lock_guard lock(memory_mutex);
            if (offset + length > memory.size()) return false;
            std::memcpy(out, memory.data() + offset, length);
            return true;
        }
        std::uint64_t done = 0;
        while (done < length) {
            const ssize_t got = ::pread(descriptor, out + done, length - done,
                                        static_cast<off_t>(offset + done));
            if (got <= 0) return false;
            done += static_cast<std::uint64_t>(got);
        }
        return true;
    }

    bool read(std::uint64_t offset, std::uint64_t length, std::string& out) const {
        out.resize(length);
        return length == 0 || read(offset, length, out.data());
    }

    bool write(std::string_view prefix, std::string_view payload) {
        if (path.empty()) {
            const std::lock_guard lock(memory_mutex);
            memory.append(prefix);
            memory.append(payload);
        } else {
            std::array<iovec, 2> parts{
                iovec{const_cast<char*>(prefix.data()), prefix.size()},
                iovec{const_cast<char*>(payload.data()), payload.size()}};
            std::size_t total = prefix.size() + payload.size();
            std::size_t done = 0;
            while (done < total) {
                // Skip what was already written of the two parts.
                std::array<iovec, 2> remaining{};
                int count = 0;
                std::size_t skip = done;
                for (const iovec& part : parts) {
                    if (skip >= part.iov_len) {
                        skip -= part.iov_len;
                        continue;
                    }
                    remaining[count++] = iovec{static_cast<char*>(part.iov_base) + skip,
                                               part.iov_len - skip};
                    skip = 0;
                }
                const ssize_t wrote = ::writev(descriptor, remaining.data(), count);
                if (wrote <= 0) return false;
                done += static_cast<std::size_t>(wrote);
            }
        }
        size += prefix.size() + payload.size();
        return true;
    }
};

bool ObjectLog::ReadSnapshot::read_into(std::uint64_t from, std::uint64_t count,
                                        std::string& out) const {
    out.resize(count);
    std::uint64_t position = 0;  // within the snapshot's range
    std::uint64_t written = 0;
    for (const Piece& piece : pieces) {
        if (written == count) break;
        const std::uint64_t piece_end = position + piece.length;
        if (piece_end > from) {
            const std::uint64_t skip = from > position ? from - position : 0;
            const std::uint64_t take = std::min(piece.length - skip, count - written);
            if (!piece.segment->read(piece.file_offset + skip, take, out.data() + written)) {
                return false;
            }
            written += take;
            from += take;
        }
        position = piece_end;
    }
    return written == count;
}

ObjectLog::ObjectLog(std::filesystem::path directory, std::int64_t tombstone_grace_ms,
                     std::uint64_t segment_bytes)
    : directory_(std::move(directory)),
      tombstone_grace_ms_(tombstone_grace_ms),
      segment_bytes_(segment_bytes == 0 ? kSegmentBytes : segment_bytes) {}

ObjectLog::~ObjectLog() {
    sync_observer_ = nullptr;  // what it reports to may already be gone
    if (failed_.load()) return;
    for (const auto& [id, segment] : segments_) flush(*segment);
}

std::size_t ObjectLog::bucket_of(const std::string& key) {
    return static_cast<std::size_t>(fnv64(key) % kBuckets);
}

int ObjectLog::fsync_descriptor(const std::filesystem::path& path, int descriptor) {
    return fsync_ ? fsync_(path, descriptor) : ::fsync(descriptor);
}

void ObjectLog::fail(const std::filesystem::path& path) {
    // After a failed fsync the kernel may have dropped the dirty pages and marked
    // them clean (Linux does), so a later fsync can succeed without them. Nothing
    // the log holds can be trusted to be durable any more: refuse every write
    // and barrier until maintain() has reopened the log from disk.
    if (!failed_.exchange(true)) {
        std::fprintf(stderr, "zetta: object log %s: fsync of %s failed; refusing writes until it is reopened\n",
                     directory_.c_str(), path.c_str());
    }
}

bool ObjectLog::flush(Segment& segment) {
    if (segment.path.empty()) return true;  // in memory
    const std::lock_guard syncing(segment.sync_mutex);
    // Read before the fsync: every byte below it was written before it began.
    const std::uint64_t written = segment.size.load();
    if (segment.synced.load(std::memory_order_acquire) >= written) return true;
    if (fsync_descriptor(segment.path, segment.descriptor) != 0) {
        fail(segment.path);
        return false;
    }
    segment.synced.store(written, std::memory_order_release);
    if (sync_observer_) sync_observer_(segment.id, written);
    return true;
}

bool ObjectLog::sync_directory(const std::filesystem::path& directory) {
    const int descriptor = ::open(directory.c_str(), O_RDONLY);
    if (descriptor < 0) return false;
    const bool synced = fsync_descriptor(directory, descriptor) == 0;
    ::close(descriptor);
    if (!synced) fail(directory);
    return synced;
}

bool ObjectLog::roll_locked(std::unique_lock<std::shared_mutex>* lock) {
    // The prefix guarantee (#1235): the segment rolled past is fsynced before the
    // next one takes its first byte, so a power loss can lose only a suffix of
    // the log. Without it, the next segment's pages could reach the disk while
    // this one's tail did not, and replay (which collects every frame per key)
    // would keep a later write without an earlier one. flush() waits for any
    // fsync of the segment already running, so a barrier or a durable write
    // inside its own fsync cannot make the roll skip. Once per segment (64 MiB).
    const auto previous = segments_.find(active_);
    const std::shared_ptr<Segment> sealing =
        previous == segments_.end() ? nullptr : previous->second;
    auto segment = std::make_shared<Segment>();
    segment->id = active_ + 1;
    if (lock) lock->unlock();
    bool ok = !sealing || flush(*sealing);
    if (ok && !directory_.empty()) {
        segment->path = segment_path(directory_, segment->id);
        segment->descriptor = ::open(segment->path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0600);
        // The new file's directory entry is fsynced too, so a durable write to
        // it means what it says.
        ok = segment->descriptor >= 0 && sync_directory(directory_);
    }
    if (lock) lock->lock();
    if (!ok) return false;
    active_ = segment->id;
    segments_[segment->id] = std::move(segment);
    return true;
}

bool ObjectLog::open(std::string* reason) {
    const std::unique_lock lock(mutex_);
    entries_.clear();
    uploads_.clear();
    segments_.clear();
    digests_ = {};
    active_ = 0;
    live_objects_ = live_object_bytes_ = staged_bytes_ = extent_count_ = 0;
    compaction_offset_ = 0;
    if (directory_.empty()) return roll_locked();

    std::error_code error;
    // The directories this call creates, outermost first: each one's entry is
    // fsynced in its parent, so the bucket's directory survives a power loss.
    std::vector<std::filesystem::path> created;
    for (std::filesystem::path at = directory_; !at.empty() && !std::filesystem::exists(at, error);
         at = at.parent_path()) {
        created.insert(created.begin(), at);
        if (at == at.parent_path()) break;
    }
    std::filesystem::create_directories(directory_, error);
    if (error) {
        if (reason) *reason = "cannot create " + directory_.string() + ": " + error.message();
        return false;
    }
    for (const std::filesystem::path& made : created) {
        const std::filesystem::path parent =
            made.has_parent_path() ? made.parent_path() : std::filesystem::path(".");
        if (!sync_directory(parent)) {
            if (reason) *reason = "cannot fsync " + parent.string();
            return false;
        }
    }
    std::vector<std::uint32_t> ids;
    for (const auto& file : std::filesystem::directory_iterator(directory_, error)) {
        const std::string name = file.path().filename().string();
        std::uint32_t id = 0;
        if (name.size() == 20 && name.starts_with("segment-") && name.ends_with(".log") &&
            std::from_chars(name.data() + 8, name.data() + 16, id).ec == std::errc{}) {
            ids.push_back(id);
        }
    }
    std::ranges::sort(ids);

    // Replay. Every frame of every generation is collected per key, later copies
    // (compaction moved them) replacing earlier ones; the directory is resolved
    // afterwards, so the order frames sit in does not matter.
    struct Marker {
        std::uint64_t upload = 0;
        std::uint64_t size = 0;
        std::int64_t t = 0;
        Location at;
    };
    struct KeyReplay {
        std::uint64_t floor = 0;  // PURGE: nothing at or below survives
        std::map<std::uint64_t, std::map<std::uint64_t, Extent>> appends;  // generation -> offset -> extent
        std::map<std::uint64_t, std::int64_t> append_times;
        std::map<std::uint64_t, Marker> commits;  // generation -> commit
        std::map<std::uint64_t, Marker> removes;  // generation -> tombstone
    };
    std::map<std::string, KeyReplay> replay;
    std::map<std::uint64_t, std::map<std::uint64_t, Extent>> stages;  // upload -> offset -> extent

    for (std::size_t position = 0; position < ids.size(); ++position) {
        // Only the last segment can hold bytes that were never fsynced (a roll
        // fsyncs the segment it leaves, #1235); a log written before that rule
        // can have two. So the last two have their payloads verified, and the
        // first torn frame in them ends the log: the segment is truncated there
        // and any later one deleted, so the log replays as a prefix.
        const bool verify = position + 2 >= ids.size();
        auto segment = std::make_shared<Segment>();
        segment->id = ids[position];
        segment->path = segment_path(directory_, segment->id);
        segment->descriptor = ::open(segment->path.c_str(), O_RDWR | O_APPEND);
        if (segment->descriptor < 0) {
            if (reason) *reason = "cannot open " + segment->path.string();
            return false;
        }
        const std::uint64_t file_size = std::filesystem::file_size(segment->path, error);
        std::uint64_t offset = 0;
        std::string head;
        std::string payload;
        bool torn = false;
        while (offset + 8 <= file_size) {
            const std::uint64_t want = std::min<std::uint64_t>(file_size - offset, 8 + kMaxHeaderBytes);
            if (!segment->read(offset, want, head)) {
                torn = true;
                break;
            }
            Frame frame;
            const std::size_t prefix = frame.decode(head);
            if (prefix == 0 || offset + prefix + frame.payload_length > file_size) {
                torn = true;
                break;
            }
            if (verify && frame.payload_length > 0) {
                if (!segment->read(offset + prefix, frame.payload_length, payload) ||
                    crc32c(payload) != frame.payload_crc) {
                    torn = true;
                    break;
                }
            }
            const Location at{segment->id, offset,
                              static_cast<std::uint32_t>(prefix + frame.payload_length)};
            const Extent extent{frame.b, frame.payload_length, offset + prefix, at};
            KeyReplay& state = replay[frame.key];
            switch (frame.type) {
                case kData:
                    if (frame.flags & kAppendFlag) {
                        state.appends[frame.a][frame.b] = extent;
                        auto& time = state.append_times[frame.a];
                        time = std::max(time, frame.t);
                    } else {
                        stages[frame.a][frame.b] = extent;
                    }
                    break;
                case kCommit: state.commits[frame.a] = Marker{frame.b, frame.c, frame.t, at}; break;
                case kRemove: state.removes[frame.a] = Marker{0, 0, frame.t, at}; break;
                case kPurge: state.floor = std::max(state.floor, frame.a); break;
                default: break;
            }
            offset += at.frame_length;
        }
        const bool cut = (torn || offset < file_size) && verify;
        if (torn || offset < file_size) {
            std::fprintf(stderr, "zetta: object log %s: unreadable frame at %llu of %llu%s\n",
                         segment->path.c_str(), static_cast<unsigned long long>(offset),
                         static_cast<unsigned long long>(file_size),
                         cut ? "; truncating the log there" : "; the rest is skipped");
        }
        if (cut) {
            if (::ftruncate(segment->descriptor, static_cast<off_t>(offset)) != 0) {
                if (reason) *reason = "cannot truncate " + segment->path.string();
                return false;
            }
            // Nothing after the tear may replay: a later segment goes too.
            for (std::size_t later = position + 1; later < ids.size(); ++later) {
                std::filesystem::remove(segment_path(directory_, ids[later]), error);
            }
            ids.resize(position + 1);
        }
        segment->size = cut ? offset : file_size;
        // A segment before the last two was fsynced when the log rolled past it.
        if (!verify) segment->synced = segment->size.load();
        segments_[segment->id] = segment;
        active_ = segment->id;
        // A process that crashed can leave bytes in the page cache that were
        // never fsynced; make them durable before anything is appended after
        // them, or a power loss later could keep the new bytes without these.
        if (verify && !flush(*segment)) {
            if (reason) *reason = "cannot fsync " + segment->path.string();
            return false;
        }
    }
    if (!ids.empty() && !sync_directory(directory_)) {
        if (reason) *reason = "cannot fsync " + directory_.string();
        return false;
    }

    // Resolve: each key's highest generation that is whole.
    const auto contiguous = [](const std::map<std::uint64_t, Extent>& pieces,
                               std::vector<Extent>& out) {
        std::uint64_t end = 0;
        for (const auto& [offset, extent] : pieces) {
            if (offset != end) break;
            if (extent.length > 0) out.push_back(extent);
            end += extent.length;
        }
        return end;
    };
    for (auto& [key, state] : replay) {
        std::set<std::uint64_t, std::greater<>> candidates;
        for (const auto& [generation, _] : state.appends) candidates.insert(generation);
        for (const auto& [generation, _] : state.commits) candidates.insert(generation);
        for (const auto& [generation, _] : state.removes) candidates.insert(generation);
        for (const std::uint64_t generation : candidates) {
            if (generation <= state.floor) break;
            Entry entry;
            entry.generation = generation;
            if (const auto removed = state.removes.find(generation); removed != state.removes.end()) {
                entry.deleted = true;
                entry.updated_ms = removed->second.t;
                entry.marker = removed->second.at;
                place_locked(key, std::move(entry));
                break;
            }
            const auto appended = state.appends.find(generation);
            if (const auto committed = state.commits.find(generation);
                committed != state.commits.end()) {
                std::map<std::uint64_t, Extent> pieces;
                if (const auto staged = stages.find(committed->second.upload); staged != stages.end()) {
                    pieces = staged->second;
                }
                if (appended != state.appends.end()) {
                    for (const auto& [offset, extent] : appended->second) pieces[offset] = extent;
                }
                std::vector<Extent> extents;
                const std::uint64_t end = contiguous(pieces, extents);
                if (end < committed->second.size) continue;  // a torn put: the older version stands
                entry.upload = committed->second.upload;
                entry.size = end;
                entry.extents = std::move(extents);
                entry.marker = committed->second.at;
                entry.updated_ms = committed->second.t;
                if (appended != state.appends.end()) {
                    entry.updated_ms = std::max(entry.updated_ms, state.append_times[generation]);
                }
                place_locked(key, std::move(entry));
                break;
            }
            if (appended == state.appends.end() || !appended->second.contains(0)) continue;
            entry.size = contiguous(appended->second, entry.extents);
            entry.updated_ms = state.append_times[generation];
            if (entry.extents.empty()) entry.marker = appended->second.at(0).frame;
            place_locked(key, std::move(entry));
            break;
        }
    }
    if (segments_.empty() || segments_[active_]->size >= segment_bytes_) return roll_locked();
    return true;
}

// ---- directory bookkeeping ----------------------------------------------------------

void ObjectLog::account_locked(const Location& at, bool live) {
    const auto segment = segments_.find(at.segment);
    if (segment == segments_.end()) return;
    if (live) {
        segment->second->live_bytes += at.frame_length;
    } else {
        segment->second->live_bytes -= std::min<std::uint64_t>(segment->second->live_bytes,
                                                               at.frame_length);
    }
}

void ObjectLog::erase_locked(const std::string& key) {
    const auto found = entries_.find(key);
    if (found == entries_.end()) return;
    const Entry& entry = found->second;
    digests_[bucket_of(key)] ^= version_hash(key, entry.generation, entry.size, entry.deleted);
    for (const Extent& extent : entry.extents) account_locked(extent.frame, false);
    if (entry.marker) account_locked(*entry.marker, false);
    if (!entry.deleted) {
        --live_objects_;
        live_object_bytes_ -= entry.size;
    }
    extent_count_ -= entry.extents.size();
    entries_.erase(found);
}

void ObjectLog::place_locked(const std::string& key, Entry entry) {
    erase_locked(key);
    digests_[bucket_of(key)] ^= version_hash(key, entry.generation, entry.size, entry.deleted);
    for (const Extent& extent : entry.extents) account_locked(extent.frame, true);
    if (entry.marker) account_locked(*entry.marker, true);
    if (!entry.deleted) {
        ++live_objects_;
        live_object_bytes_ += entry.size;
    }
    extent_count_ += entry.extents.size();
    entries_.insert_or_assign(key, std::move(entry));
}

bool ObjectLog::append_locked(std::unique_lock<std::shared_mutex>& lock, const Frame& frame,
                              std::string_view payload, Location& at,
                              std::uint64_t& payload_offset,
                              std::shared_ptr<Segment>& written_to) {
    if (failed_.load()) return false;  // an fsync failed: nothing is written until reopen
    if (segments_[active_]->size >= segment_bytes_) {
        // The roll fsyncs the segment rolled past before this frame is written,
        // so only the newest segment can be torn (#1235).
        if (!roll_locked(&lock)) return false;
    }
    written_to = segments_[active_];
    const std::string prefix = frame.encode();
    at.segment = active_;
    at.frame_offset = written_to->size;
    at.frame_length = static_cast<std::uint32_t>(prefix.size() + payload.size());
    payload_offset = at.frame_offset + prefix.size();
    // The write itself runs with the directory unlocked: the caller holds
    // write_mutex_, so no other writer can move the file's end or the directory
    // meanwhile, and a reader never waits for write(2) (which can stall for
    // milliseconds behind a concurrent fsync of the same file).
    lock.unlock();
    const bool written = written_to->write(prefix, payload);
    lock.lock();
    return written;
}

bool ObjectLog::equal_locked(const Entry& entry, std::uint64_t offset,
                             std::string_view bytes) const {
    std::string held;
    std::uint64_t compared = 0;
    for (const Extent& extent : entry.extents) {
        if (compared == bytes.size()) break;
        const std::uint64_t end = extent.object_offset + extent.length;
        const std::uint64_t at = offset + compared;
        if (end <= at) continue;
        const std::uint64_t skip = at - extent.object_offset;
        const std::uint64_t take = std::min(extent.length - skip, bytes.size() - compared);
        const auto segment = segments_.find(extent.frame.segment);
        if (segment == segments_.end() ||
            !segment->second->read(extent.file_offset + skip, take, held) ||
            held != bytes.substr(compared, take)) {
            return false;
        }
        compared += take;
    }
    return compared == bytes.size();
}

bool ObjectLog::covering(const std::vector<Extent>& extents, std::uint64_t size) const {
    std::uint64_t end = 0;
    for (const Extent& extent : extents) {
        if (extent.object_offset != end) return false;
        end += extent.length;
    }
    return end >= size;
}

ObjectLog::Written ObjectLog::finish(Written written, std::shared_ptr<Segment> segment,
                                     bool sync) {
    // Flushed outside the lock: readers never wait for the disk.
    // Every earlier segment was fsynced when the log rolled past it, so a
    // durable write makes the whole log up to it durable.
    if (sync && segment) written.durable = flush(*segment) && !failed_.load();
    return written;
}

std::optional<ObjectVersion> ObjectLog::stat(const std::string& key) const {
    const std::shared_lock lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end()) return std::nullopt;
    return ObjectVersion{key, found->second.generation, found->second.size,
                         found->second.deleted, found->second.updated_ms};
}

// ---- the ordering node ----------------------------------------------------------------

ObjectLog::Written ObjectLog::append(const std::string& key, std::uint64_t offset,
                                     std::string_view bytes, std::uint64_t fresh, bool sync) {
    const std::uint32_t payload_crc = crc32c(bytes);  // computed before any lock
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        const bool live = found != entries_.end() && !found->second.deleted;
        const auto version = [&] {
            return live ? ObjectVersion{key, found->second.generation, found->second.size, false,
                                        found->second.updated_ms}
                        : ObjectVersion{key, 0, 0, false, 0};
        };
        if (live) {
            Entry& entry = found->second;
            if (offset < entry.size || (offset == entry.size && bytes.empty())) {
                // A retry of bytes already there succeeds and changes nothing.
                if (offset + bytes.size() <= entry.size && equal_locked(entry, offset, bytes)) {
                    written.result = Result::Unchanged;
                    written.version = version();
                    return written;
                }
                written.result = Result::Conflict;
                written.version = version();
                return written;
            }
            if (offset != entry.size) {
                written.result = Result::Conflict;
                written.version = version();
                return written;
            }
            Frame frame;
            frame.type = kData;
            frame.flags = kAppendFlag;
            frame.key = key;
            frame.a = entry.generation;
            frame.b = offset;
            frame.t = wall_ms();
            frame.payload_length = static_cast<std::uint32_t>(bytes.size());
            frame.payload_crc = payload_crc;
            Extent extent;
            extent.object_offset = offset;
            extent.length = bytes.size();
            if (!append_locked(lock, frame, bytes, extent.frame, extent.file_offset, segment)) {
                written.result = Result::Io;
                return written;
            }
            digests_[bucket_of(key)] ^= version_hash(key, entry.generation, entry.size, false);
            entry.extents.push_back(extent);
            entry.size += bytes.size();
            entry.updated_ms = frame.t;
            digests_[bucket_of(key)] ^= version_hash(key, entry.generation, entry.size, false);
            account_locked(extent.frame, true);
            live_object_bytes_ += bytes.size();
            ++extent_count_;
            written.version = version();
            return finish(std::move(written), std::move(segment), sync);
        }
        if (offset != 0) {
            written.result = Result::Conflict;
            written.version = version();
            return written;
        }
        const std::uint64_t floor = found == entries_.end() ? 0 : found->second.generation;
        Frame frame;
        frame.type = kData;
        frame.flags = kAppendFlag;
        frame.key = key;
        frame.a = std::max(fresh, floor + 1);
        frame.b = 0;
        frame.t = wall_ms();
        frame.payload_length = static_cast<std::uint32_t>(bytes.size());
        frame.payload_crc = payload_crc;
        Extent extent;
        extent.length = bytes.size();
        if (!append_locked(lock, frame, bytes, extent.frame, extent.file_offset, segment)) {
            written.result = Result::Io;
            return written;
        }
        Entry entry;
        entry.generation = frame.a;
        entry.size = bytes.size();
        entry.updated_ms = frame.t;
        if (bytes.empty()) {
            entry.marker = extent.frame;  // keeps the empty object's frame live
        } else {
            entry.extents.push_back(extent);
        }
        place_locked(key, std::move(entry));
        written.version = ObjectVersion{key, frame.a, bytes.size(), false, frame.t};
    }
    return finish(std::move(written), std::move(segment), sync);
}

ObjectLog::Written ObjectLog::put(const std::string& key, std::string_view bytes,
                                  std::uint64_t fresh, bool if_absent, bool sync) {
    const std::uint32_t payload_crc = crc32c(bytes);  // computed before any lock
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        const bool live = found != entries_.end() && !found->second.deleted;
        if (live && if_absent) {
            written.result = Result::Conflict;
            written.version = ObjectVersion{key, found->second.generation, found->second.size,
                                            false, found->second.updated_ms};
            return written;
        }
        Frame frame;
        frame.type = kData;
        frame.flags = kAppendFlag;
        frame.key = key;
        frame.a = std::max(fresh, (found == entries_.end() ? 0 : found->second.generation) + 1);
        frame.b = 0;
        frame.t = wall_ms();
        frame.payload_length = static_cast<std::uint32_t>(bytes.size());
        frame.payload_crc = payload_crc;
        Extent extent;
        extent.length = bytes.size();
        if (!append_locked(lock, frame, bytes, extent.frame, extent.file_offset, segment)) {
            written.result = Result::Io;
            return written;
        }
        Entry entry;
        entry.generation = frame.a;
        entry.size = bytes.size();
        entry.updated_ms = frame.t;
        if (bytes.empty()) {
            entry.marker = extent.frame;
        } else {
            entry.extents.push_back(extent);
        }
        place_locked(key, std::move(entry));
        written.version = ObjectVersion{key, frame.a, bytes.size(), false, frame.t};
    }
    return finish(std::move(written), std::move(segment), sync);
}

ObjectLog::Written ObjectLog::commit(const std::string& key, std::uint64_t upload,
                                     std::uint64_t size, std::uint64_t fresh, bool if_absent,
                                     bool sync) {
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto staged = uploads_.find(upload);
        if (staged == uploads_.end() || staged->second.key != key || staged->second.size != size ||
            !covering(staged->second.extents, size)) {
            written.result = Result::NotFound;
            return written;
        }
        const auto found = entries_.find(key);
        const bool live = found != entries_.end() && !found->second.deleted;
        if (live && if_absent) {
            written.result = Result::Conflict;
            written.version = ObjectVersion{key, found->second.generation, found->second.size,
                                            false, found->second.updated_ms};
            return written;
        }
        Frame frame;
        frame.type = kCommit;
        frame.key = key;
        frame.a = std::max(fresh, (found == entries_.end() ? 0 : found->second.generation) + 1);
        frame.b = upload;
        frame.c = size;
        frame.t = wall_ms();
        Location at;
        std::uint64_t unused = 0;
        if (!append_locked(lock, frame, {}, at, unused, segment)) {
            written.result = Result::Io;
            return written;
        }
        Entry entry;
        entry.generation = frame.a;
        entry.upload = upload;
        entry.size = size;
        entry.updated_ms = frame.t;
        entry.extents = std::move(staged->second.extents);
        entry.marker = at;
        // The staged frames were not counted live while staged.
        staged_bytes_ -= std::min(staged_bytes_, staged->second.size);
        uploads_.erase(staged);
        place_locked(key, std::move(entry));
        written.version = ObjectVersion{key, frame.a, size, false, frame.t};
    }
    return finish(std::move(written), std::move(segment), sync);
}

ObjectLog::Written ObjectLog::remove(const std::string& key, std::uint64_t fresh, bool sync) {
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.deleted) {
            written.result = Result::NotFound;
            return written;
        }
        Frame frame;
        frame.type = kRemove;
        frame.key = key;
        frame.a = std::max(fresh, found->second.generation + 1);
        frame.t = wall_ms();
        Location at;
        std::uint64_t unused = 0;
        if (!append_locked(lock, frame, {}, at, unused, segment)) {
            written.result = Result::Io;
            return written;
        }
        Entry entry;
        entry.generation = frame.a;
        entry.deleted = true;
        entry.updated_ms = frame.t;
        entry.marker = at;
        place_locked(key, std::move(entry));
        written.version = ObjectVersion{key, frame.a, 0, true, frame.t};
    }
    return finish(std::move(written), std::move(segment), sync);
}

// ---- uploads ------------------------------------------------------------------------------

ObjectLog::Written ObjectLog::stage(const std::string& key, std::uint64_t upload,
                                    std::uint64_t offset, std::string_view bytes, bool sync) {
    const std::uint32_t payload_crc = crc32c(bytes);  // computed before any lock
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        Upload& staged = uploads_[upload];
        if (staged.key.empty()) staged.key = key;
        staged.touched_ms = wall_ms();
        if (staged.key != key) {
            written.result = Result::Conflict;
            return written;
        }
        if (offset + bytes.size() <= staged.size) {
            written.result = Result::Unchanged;  // a retried part
            return written;
        }
        if (offset != staged.size) {
            written.result = Result::Gap;
            return written;
        }
        Frame frame;
        frame.type = kData;
        frame.key = key;
        frame.a = upload;
        frame.b = offset;
        frame.t = staged.touched_ms;
        frame.payload_length = static_cast<std::uint32_t>(bytes.size());
        frame.payload_crc = payload_crc;
        Extent extent;
        extent.object_offset = offset;
        extent.length = bytes.size();
        if (!append_locked(lock, frame, bytes, extent.frame, extent.file_offset, segment)) {
            written.result = Result::Io;
            return written;
        }
        if (!bytes.empty()) staged.extents.push_back(extent);
        staged.size += bytes.size();
        staged_bytes_ += bytes.size();
    }
    return finish(std::move(written), std::move(segment), sync);
}

void ObjectLog::abort(std::uint64_t upload) {
    const std::lock_guard writing(write_mutex_);
    std::unique_lock lock(mutex_);
    const auto found = uploads_.find(upload);
    if (found == uploads_.end()) return;
    staged_bytes_ -= std::min(staged_bytes_, found->second.size);
    uploads_.erase(found);
}

void ObjectLog::expire_uploads(std::int64_t idle_before_ms) {
    const std::lock_guard writing(write_mutex_);
    std::unique_lock lock(mutex_);
    for (auto upload = uploads_.begin(); upload != uploads_.end();) {
        if (upload->second.touched_ms < idle_before_ms) {
            staged_bytes_ -= std::min(staged_bytes_, upload->second.size);
            upload = uploads_.erase(upload);
        } else {
            ++upload;
        }
    }
}

std::uint64_t ObjectLog::staged_bytes() const {
    const std::shared_lock lock(mutex_);
    return staged_bytes_;
}

// ---- replicas -------------------------------------------------------------------------------

ObjectLog::Written ObjectLog::apply_append(const std::string& key, std::uint64_t generation,
                                           std::uint64_t offset, std::string_view bytes,
                                           bool sync) {
    const std::uint32_t payload_crc = crc32c(bytes);  // computed before any lock
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end() && found->second.generation > generation) {
            written.result = Result::Stale;
            return written;
        }
        const bool same = found != entries_.end() && found->second.generation == generation &&
                          !found->second.deleted;
        if (same && offset + bytes.size() <= found->second.size) {
            written.result = Result::Unchanged;
            return written;
        }
        if ((same && offset != found->second.size) || (!same && offset != 0)) {
            written.result = Result::Gap;
            if (found != entries_.end()) {
                written.version = ObjectVersion{key, found->second.generation, found->second.size,
                                                found->second.deleted, found->second.updated_ms};
            }
            return written;
        }
        Frame frame;
        frame.type = kData;
        frame.flags = kAppendFlag;
        frame.key = key;
        frame.a = generation;
        frame.b = offset;
        frame.t = wall_ms();
        frame.payload_length = static_cast<std::uint32_t>(bytes.size());
        frame.payload_crc = payload_crc;
        Extent extent;
        extent.object_offset = offset;
        extent.length = bytes.size();
        if (!append_locked(lock, frame, bytes, extent.frame, extent.file_offset, segment)) {
            written.result = Result::Io;
            return written;
        }
        if (same) {
            Entry& entry = found->second;
            digests_[bucket_of(key)] ^= version_hash(key, entry.generation, entry.size, false);
            if (!bytes.empty()) entry.extents.push_back(extent);
            entry.size += bytes.size();
            entry.updated_ms = frame.t;
            digests_[bucket_of(key)] ^= version_hash(key, entry.generation, entry.size, false);
            account_locked(extent.frame, true);
            live_object_bytes_ += bytes.size();
            if (!bytes.empty()) ++extent_count_;
            written.version = ObjectVersion{key, generation, entry.size, false, frame.t};
        } else {
            Entry entry;
            entry.generation = generation;
            entry.size = bytes.size();
            entry.updated_ms = frame.t;
            if (bytes.empty()) {
                entry.marker = extent.frame;
            } else {
                entry.extents.push_back(extent);
            }
            place_locked(key, std::move(entry));
            written.version = ObjectVersion{key, generation, bytes.size(), false, frame.t};
        }
    }
    return finish(std::move(written), std::move(segment), sync);
}

ObjectLog::Written ObjectLog::apply_put(const std::string& key, std::uint64_t generation,
                                        std::string_view bytes, bool sync) {
    const std::uint32_t payload_crc = crc32c(bytes);  // computed before any lock
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end() && found->second.generation >= generation) {
            written.result = Result::Stale;
            return written;
        }
        Frame frame;
        frame.type = kData;
        frame.flags = kAppendFlag;
        frame.key = key;
        frame.a = generation;
        frame.t = wall_ms();
        frame.payload_length = static_cast<std::uint32_t>(bytes.size());
        frame.payload_crc = payload_crc;
        Extent extent;
        extent.length = bytes.size();
        if (!append_locked(lock, frame, bytes, extent.frame, extent.file_offset, segment)) {
            written.result = Result::Io;
            return written;
        }
        Entry entry;
        entry.generation = generation;
        entry.size = bytes.size();
        entry.updated_ms = frame.t;
        if (bytes.empty()) {
            entry.marker = extent.frame;
        } else {
            entry.extents.push_back(extent);
        }
        place_locked(key, std::move(entry));
        written.version = ObjectVersion{key, generation, bytes.size(), false, frame.t};
    }
    return finish(std::move(written), std::move(segment), sync);
}

ObjectLog::Written ObjectLog::apply_commit(const std::string& key, std::uint64_t upload,
                                           std::uint64_t generation, std::uint64_t size,
                                           bool sync) {
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto staged = uploads_.find(upload);
        const auto found = entries_.find(key);
        if (found != entries_.end() && found->second.generation >= generation) {
            if (staged != uploads_.end()) {
                staged_bytes_ -= std::min(staged_bytes_, staged->second.size);
                uploads_.erase(staged);
            }
            written.result = Result::Stale;
            return written;
        }
        if (staged == uploads_.end() || staged->second.key != key || staged->second.size != size ||
            !covering(staged->second.extents, size)) {
            written.result = Result::NotFound;
            return written;
        }
        Frame frame;
        frame.type = kCommit;
        frame.key = key;
        frame.a = generation;
        frame.b = upload;
        frame.c = size;
        frame.t = wall_ms();
        Location at;
        std::uint64_t unused = 0;
        if (!append_locked(lock, frame, {}, at, unused, segment)) {
            written.result = Result::Io;
            return written;
        }
        Entry entry;
        entry.generation = generation;
        entry.upload = upload;
        entry.size = size;
        entry.updated_ms = frame.t;
        entry.extents = std::move(staged->second.extents);
        entry.marker = at;
        staged_bytes_ -= std::min(staged_bytes_, staged->second.size);
        uploads_.erase(staged);
        place_locked(key, std::move(entry));
        written.version = ObjectVersion{key, generation, size, false, frame.t};
    }
    return finish(std::move(written), std::move(segment), sync);
}

ObjectLog::Written ObjectLog::apply_remove(const std::string& key, std::uint64_t generation,
                                           std::int64_t updated_ms, bool sync) {
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::lock_guard writing(write_mutex_);
        std::unique_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end() && found->second.generation >= generation) {
            written.result = Result::Stale;
            return written;
        }
        Frame frame;
        frame.type = kRemove;
        frame.key = key;
        frame.a = generation;
        frame.t = updated_ms;
        Location at;
        std::uint64_t unused = 0;
        if (!append_locked(lock, frame, {}, at, unused, segment)) {
            written.result = Result::Io;
            return written;
        }
        Entry entry;
        entry.generation = generation;
        entry.deleted = true;
        entry.updated_ms = updated_ms;
        entry.marker = at;
        place_locked(key, std::move(entry));
        written.version = ObjectVersion{key, generation, 0, true, updated_ms};
    }
    return finish(std::move(written), std::move(segment), sync);
}

bool ObjectLog::purge(const std::string& key, std::uint64_t generation) {
    const std::lock_guard writing(write_mutex_);
    std::unique_lock lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() || found->second.generation > generation) return false;
    // The marker keeps an older frame of this key from coming back on restart.
    Frame frame;
    frame.type = kPurge;
    frame.key = key;
    frame.a = generation;
    frame.t = wall_ms();
    Location at;
    std::uint64_t unused = 0;
    std::shared_ptr<Segment> segment;
    if (!append_locked(lock, frame, {}, at, unused, segment)) return false;
    erase_locked(key);
    return true;
}

// ---- reads ---------------------------------------------------------------------------------------

ObjectLog::ReadStatus ObjectLog::snapshot_locked(const Entry& entry, std::uint64_t offset,
                                                 std::uint64_t length, const std::string& key,
                                                 ReadSnapshot& out) const {
    out.version = ObjectVersion{key, entry.generation, entry.size, false, entry.updated_ms};
    out.offset = offset;
    out.length = length;
    out.pieces.clear();
    if (length == 0) return ReadStatus::Ok;
    const std::uint64_t end = offset + length;
    // The first extent that ends after `offset`.
    auto extent = std::upper_bound(entry.extents.begin(), entry.extents.end(), offset,
                                   [](std::uint64_t at, const Extent& candidate) {
                                       return at < candidate.object_offset + candidate.length;
                                   });
    for (; extent != entry.extents.end() && extent->object_offset < end; ++extent) {
        const auto segment = segments_.find(extent->frame.segment);
        if (segment == segments_.end()) return ReadStatus::NotFound;
        const std::uint64_t from = std::max(offset, extent->object_offset);
        const std::uint64_t to = std::min(end, extent->object_offset + extent->length);
        out.pieces.push_back(
            Piece{segment->second, extent->file_offset + (from - extent->object_offset), to - from});
    }
    return ReadStatus::Ok;
}

ObjectLog::ReadStatus ObjectLog::snapshot(const std::string& key, std::uint64_t offset,
                                          std::optional<std::uint64_t> length,
                                          ReadSnapshot& out) const {
    const std::shared_lock lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() || found->second.deleted) return ReadStatus::NotFound;
    const Entry& entry = found->second;
    if (offset > entry.size || (length && offset + *length > entry.size)) {
        out.version = ObjectVersion{key, entry.generation, entry.size, false, entry.updated_ms};
        return ReadStatus::OutOfRange;
    }
    return snapshot_locked(entry, offset, length ? *length : entry.size - offset, key, out);
}

ObjectLog::ReadStatus ObjectLog::snapshot_generation(const std::string& key,
                                                     std::uint64_t generation, std::uint64_t offset,
                                                     std::uint64_t length, ReadSnapshot& out) const {
    const std::shared_lock lock(mutex_);
    const auto found = entries_.find(key);
    if (found == entries_.end() || found->second.deleted || found->second.generation != generation) {
        return ReadStatus::NotFound;
    }
    if (offset + length > found->second.size) return ReadStatus::OutOfRange;
    return snapshot_locked(found->second, offset, length, key, out);
}

void ObjectLog::list(const std::string& prefix, const std::string& after,
                     const std::function<bool(const ObjectVersion&)>& visit,
                     bool tombstones) const {
    const std::shared_lock lock(mutex_);
    // The keys with `prefix` are one contiguous run starting at lower_bound(prefix).
    auto entry = after.empty() || after < prefix ? entries_.lower_bound(prefix)
                                                 : entries_.upper_bound(after);
    for (; entry != entries_.end() && entry->first.starts_with(prefix); ++entry) {
        if (entry->second.deleted && !tombstones) continue;
        if (!visit(ObjectVersion{entry->first, entry->second.generation, entry->second.size,
                                 entry->second.deleted, entry->second.updated_ms})) {
            return;
        }
    }
}

void ObjectLog::each(const std::function<void(const ObjectVersion&)>& visit) const {
    const std::shared_lock lock(mutex_);
    for (const auto& [key, entry] : entries_) {
        visit(ObjectVersion{key, entry.generation, entry.size, entry.deleted, entry.updated_ms});
    }
}

std::array<std::uint64_t, ObjectLog::kBuckets> ObjectLog::digests(
    const std::function<bool(const std::string&)>& keep) const {
    const std::shared_lock lock(mutex_);
    if (!keep) return digests_;
    std::array<std::uint64_t, kBuckets> result{};
    for (const auto& [key, entry] : entries_) {
        if (!keep(key)) continue;
        result[bucket_of(key)] ^= version_hash(key, entry.generation, entry.size, entry.deleted);
    }
    return result;
}

std::vector<ObjectVersion> ObjectLog::versions(
    const std::vector<std::size_t>& buckets,
    const std::function<bool(const std::string&)>& keep) const {
    std::array<bool, kBuckets> wanted{};
    for (const std::size_t bucket : buckets) {
        if (bucket < kBuckets) wanted[bucket] = true;
    }
    const std::shared_lock lock(mutex_);
    std::vector<ObjectVersion> result;
    for (const auto& [key, entry] : entries_) {
        if (!wanted[bucket_of(key)] || (keep && !keep(key))) continue;
        result.push_back({key, entry.generation, entry.size, entry.deleted, entry.updated_ms});
    }
    return result;
}

std::uint64_t ObjectLog::object_count() const {
    const std::shared_lock lock(mutex_);
    return live_objects_;
}

std::uint64_t ObjectLog::object_bytes() const {
    const std::shared_lock lock(mutex_);
    return live_object_bytes_;
}

bool ObjectLog::sync() {
    // Every segment holding bytes no completed fsync covered. flush() waits for
    // an fsync of the segment already running (a durable write's, a roll's,
    // compaction's) and fsyncs again unless that one covered the bytes, so the
    // barrier never answers before what it covers is on disk.
    if (failed_.load()) return false;
    std::vector<std::shared_ptr<Segment>> unsynced;
    {
        const std::shared_lock lock(mutex_);
        for (const auto& [id, segment] : segments_) {
            if (segment->synced.load(std::memory_order_acquire) < segment->size.load()) {
                unsynced.push_back(segment);
            }
        }
    }
    bool ok = true;
    for (const auto& segment : unsynced) ok = flush(*segment) && ok;
    return ok && !failed_.load();
}

void ObjectLog::maintain(std::int64_t now_ms) {
    const std::unique_lock maintenance(maintenance_, std::try_to_lock);
    if (!maintenance.owns_lock()) return;
    if (failed_.load()) {
        // An fsync failed. Rebuild the log from what is on disk, as a restart
        // would; writes and barriers fail until this succeeds.
        const std::lock_guard writing(write_mutex_);
        std::string reason;
        if (open(&reason)) {
            failed_.store(false);
            std::fprintf(stderr, "zetta: object log %s: reopened after an fsync failure\n",
                         directory_.c_str());
        } else {
            std::fprintf(stderr, "zetta: object log %s: reopen failed: %s\n", directory_.c_str(),
                         reason.c_str());
        }
        return;
    }
    // Tombstones past their grace go: a node away longer than the grace can bring
    // the object back when it rejoins (docs/data/object-store.md).
    std::vector<std::pair<std::string, std::uint64_t>> expired;
    {
        const std::shared_lock lock(mutex_);
        for (const auto& [key, entry] : entries_) {
            if (entry.deleted && entry.updated_ms + tombstone_grace_ms_ <= now_ms) {
                expired.emplace_back(key, entry.generation);
            }
        }
    }
    for (const auto& [key, generation] : expired) purge(key, generation);
    compact_step();
}

// The extent or marker a frame at `at` backs, or none when the frame is dead.
// Caller holds the lock.
ObjectLog::Backing ObjectLog::backing_locked(const Frame& frame, const Location& at) {
    Backing backing;
    const auto here = [&](const Location& location) {
        return location.segment == at.segment && location.frame_offset == at.frame_offset;
    };
    if (frame.type == kData && !(frame.flags & kAppendFlag)) {
        if (const auto upload = uploads_.find(frame.a); upload != uploads_.end()) {
            for (Extent& candidate : upload->second.extents) {
                if (here(candidate.frame)) {
                    backing.extent = &candidate;
                    backing.staged = true;
                    return backing;
                }
            }
        }
    }
    const auto found = entries_.find(frame.key);
    if (found == entries_.end()) return backing;
    Entry& entry = found->second;
    if (entry.marker && here(*entry.marker)) {
        backing.marker = &*entry.marker;
        return backing;
    }
    if (frame.type != kData) return backing;
    // Extents are in object order: find the one at this frame's offset.
    const auto candidate = std::ranges::lower_bound(entry.extents, frame.b, {},
                                                    [](const Extent& e) { return e.object_offset; });
    if (candidate != entry.extents.end() && candidate->object_offset == frame.b &&
        here(candidate->frame)) {
        backing.extent = &*candidate;
    }
    return backing;
}

void ObjectLog::compact_step() {
    std::vector<std::shared_ptr<Segment>> retired;
    compact_pass(retired);
    if (retired.empty()) return;
    // A segment is dead because newer frames replaced every frame in it (or
    // compaction moved them forward). Its file may go only once those frames are
    // durable, or a power loss could keep neither the old frames nor the new
    // ones (#1235). Every segment before the active one was fsynced when the log
    // rolled past it (a roll changes active_ only after its fsync), so the active
    // segment is the only one to fsync.
    std::shared_ptr<Segment> active;
    {
        const std::shared_lock lock(mutex_);
        active = segments_[active_];
    }
    // On failure the files stay; the log has failed and its reopen replays them.
    if (!flush(*active) || directory_.empty()) return;
    // Oldest first, as they were retired (that order keeps a purge frame until
    // no older frame of its key remains), then the directory, so the removals
    // themselves are durable. A reader holding a segment keeps its descriptor.
    std::error_code error;
    for (const auto& segment : retired) std::filesystem::remove(segment->path, error);
    sync_directory(directory_);
}

void ObjectLog::compact_pass(std::vector<std::shared_ptr<Segment>>& retired) {
    if (failed_.load()) return;
    // One step moves at most kCompactionChunk bytes of live frames, and sweeps as
    // many oldest segments as that allows: a dead frame costs one header read, so
    // a store whose old data is mostly dead is reclaimed as fast as it is written.
    // The log's lock is taken once per batch, not once per frame, so writers are
    // not held up behind a sweep.
    struct Candidate {
        Frame frame;
        Location at;
        std::size_t prefix = 0;
    };
    std::uint64_t moved = 0;
    std::uint32_t swept = 0;  // the segment the last pass scanned to its end
    std::string head;
    while (moved < kCompactionChunk) {
        std::shared_ptr<Segment> oldest;
        {
            // Retire dead segments oldest-first. Oldest-first keeps every purge
            // marker until no older frame of its key remains.
            const std::lock_guard writing(write_mutex_);
            const std::unique_lock lock(mutex_);
            while (!segments_.empty()) {
                const auto first = segments_.begin();
                if (first->first == active_ || first->second->live_bytes > 0) break;
                // A staged upload's frames are not counted live; keep their segment.
                const bool staged = std::ranges::any_of(uploads_, [&](const auto& upload) {
                    return std::ranges::any_of(upload.second.extents, [&](const Extent& extent) {
                        return extent.frame.segment == first->first;
                    });
                });
                if (staged) break;
                retired.push_back(first->second);
                segments_.erase(first);
                compaction_offset_ = 0;
            }
            const auto first = segments_.begin();
            if (first == segments_.end() || first->first == active_) return;
            // Move live frames forward only from a mostly-dead oldest segment, and
            // sweep a segment once per step.
            if (first->second->live_bytes * 2 >= first->second->size || first->first == swept) {
                return;
            }
            oldest = first->second;
        }
        // Every frame header from where the last step stopped; a sealed segment
        // never changes, so no lock.
        std::vector<Candidate> frames;
        std::uint64_t position = compaction_offset_;
        while (position + 8 <= oldest->size) {
            const std::uint64_t want =
                std::min<std::uint64_t>(oldest->size - position, 8 + kMaxHeaderBytes);
            if (!oldest->read(position, want, head)) break;
            Candidate candidate;
            candidate.prefix = candidate.frame.decode(head);
            if (candidate.prefix == 0) break;  // unreadable: nothing past here was live
            candidate.at = Location{oldest->id, position,
                                    static_cast<std::uint32_t>(candidate.prefix +
                                                               candidate.frame.payload_length)};
            position += candidate.at.frame_length;
            if (candidate.frame.type != kPurge) frames.push_back(std::move(candidate));
        }
        // Which of them are live, under one shared lock.
        std::vector<Candidate*> live;
        {
            const std::shared_lock lock(mutex_);
            for (Candidate& candidate : frames) {
                const Backing backing = backing_locked(candidate.frame, candidate.at);
                if (backing.extent || backing.marker) live.push_back(&candidate);
            }
        }
        // Move them in groups that fit the budget: read outside the lock, then
        // re-check and append each group under one exclusive lock.
        std::size_t next = 0;
        while (next < live.size() && moved < kCompactionChunk) {
            std::vector<std::pair<Candidate*, std::string>> group;
            std::uint64_t group_bytes = 0;
            while (next < live.size() && moved + group_bytes < kCompactionChunk &&
                   (group.empty() || group_bytes < kCompactionGroup)) {
                Candidate* candidate = live[next++];
                std::string bytes;
                if (!oldest->read(candidate->at.frame_offset, candidate->at.frame_length, bytes)) {
                    return;
                }
                group_bytes += bytes.size();
                group.emplace_back(candidate, std::move(bytes));
            }
            // Writers wait for the group; readers only for each pointer update.
            const std::lock_guard writing(write_mutex_);
            std::unique_lock lock(mutex_);
            if (failed_.load()) return;
            for (auto& [candidate, bytes] : group) {
                const Backing backing = backing_locked(candidate->frame, candidate->at);
                if (!backing.extent && !backing.marker) continue;  // gone meanwhile
                if (segments_[active_]->size >= segment_bytes_ && !roll_locked(&lock)) return;
                const std::shared_ptr<Segment> target = segments_[active_];
                const Location to{active_, target->size, candidate->at.frame_length};
                const std::string_view frame(bytes);
                // No other writer runs (write_mutex_), so `backing` stays valid
                // while the directory is unlocked for the write.
                lock.unlock();
                const bool written =
                    target->write(frame.substr(0, candidate->prefix), frame.substr(candidate->prefix));
                lock.lock();
                if (!written) return;
                if (backing.extent) {
                    backing.extent->frame = to;
                    backing.extent->file_offset = to.frame_offset + candidate->prefix;
                } else {
                    *backing.marker = to;
                }
                if (!backing.staged) {  // staged frames are not counted live until their commit
                    account_locked(candidate->at, false);
                    account_locked(to, true);
                }
                moved += candidate->at.frame_length;
            }
        }
        if (next < live.size()) {
            compaction_offset_ = live[next]->at.frame_offset;  // the budget ran out mid-segment
            return;
        }
        compaction_offset_ = 0;
        swept = oldest->id;
    }
}

ObjectLog::Stats ObjectLog::stats() const {
    const std::shared_lock lock(mutex_);
    Stats stats;
    stats.objects = live_objects_;
    stats.tombstones = entries_.size() - live_objects_;
    stats.segments = segments_.size();
    for (const auto& [id, segment] : segments_) {
        stats.disk_bytes += segment->size;
        stats.live_bytes += segment->live_bytes;
    }
    for (const auto& [key, entry] : entries_) stats.memory_bytes += key.size() + sizeof(Entry) + 64;
    stats.memory_bytes += extent_count_ * sizeof(Extent);
    for (const auto& [id, upload] : uploads_) {
        stats.memory_bytes += upload.key.size() + upload.extents.size() * sizeof(Extent) + 96;
    }
    return stats;
}

}  // namespace zetta::data
