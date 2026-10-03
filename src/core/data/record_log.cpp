#include "record_log.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <system_error>

// The node-local record log (#1231). See record_log.hpp.
namespace zetta::data {
namespace {

std::uint32_t crc32(std::string_view bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            values[index] = value;
        }
        return values;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char character : bytes) {
        crc = table[(crc ^ static_cast<unsigned char>(character)) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint64_t fnv64(std::string_view text, std::uint64_t hash = 1469598103934665603ull) {
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t version_hash(const std::string& key, std::uint64_t revision, bool deleted) {
    std::uint64_t hash = fnv64(key);
    hash = fnv64(std::to_string(revision), fnv64("\x1f", hash));
    return fnv64(deleted ? "d" : "l", hash);
}

void put_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

std::uint32_t get_u32(const char* bytes) {
    std::uint32_t value = 0;
    for (int index = 3; index >= 0; --index) {
        value = (value << 8) | static_cast<unsigned char>(bytes[index]);
    }
    return value;
}

std::string frame(const nlohmann::json& body) {
    const std::string text = body.dump();
    std::string out;
    out.reserve(text.size() + 8);
    put_u32(out, static_cast<std::uint32_t>(text.size()));
    put_u32(out, crc32(text));
    out += text;
    return out;
}

std::string index_slot(const std::string& index, const std::string& value) {
    return index + std::string(1, '\0') + value;
}

constexpr std::uint32_t kMaxFrameBytes = 64u * 1024 * 1024;
constexpr std::uint64_t kCompactionChunk = 1024 * 1024;

}  // namespace

nlohmann::json stored_value(const StoredRecord& record) {
    nlohmann::json value{{"rev", record.revision}, {"updated", record.updated_ms}};
    if (record.deleted) {
        value["deleted"] = true;
        return value;
    }
    value["value"] = record.value;
    value["index"] = record.index;
    value["order"] = record.order;
    if (record.expires_ms) value["expires"] = *record.expires_ms;
    return value;
}

std::optional<StoredRecord> parse_stored(std::string collection, std::string key,
                                         const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto revision = value.find("rev");
    const auto updated = value.find("updated");
    if (revision == value.end() || !revision->is_number_unsigned() ||
        updated == value.end() || !updated->is_number_integer()) {
        return std::nullopt;
    }
    StoredRecord record;
    record.collection = std::move(collection);
    record.key = std::move(key);
    record.revision = revision->get<std::uint64_t>();
    record.updated_ms = updated->get<std::int64_t>();
    if (const auto deleted = value.find("deleted"); deleted != value.end()) {
        if (!deleted->is_boolean()) return std::nullopt;
        record.deleted = deleted->get<bool>();
    }
    if (const auto text = value.find("value"); text != value.end()) {
        if (!text->is_string()) return std::nullopt;
        record.value = text->get<std::string>();
    }
    if (const auto order = value.find("order"); order != value.end()) {
        if (!order->is_string()) return std::nullopt;
        record.order = order->get<std::string>();
    }
    if (const auto index = value.find("index"); index != value.end()) {
        if (!index->is_object()) return std::nullopt;
        for (const auto& [name, indexed] : index->items()) {
            if (!indexed.is_string()) return std::nullopt;
            record.index[name] = indexed.get<std::string>();
        }
    }
    if (const auto expires = value.find("expires"); expires != value.end()) {
        if (!expires->is_number_integer()) return std::nullopt;
        record.expires_ms = expires->get<std::int64_t>();
    }
    return record;
}

// One segment file (or, without a directory, an in-memory buffer). The
// descriptor closes with the last holder, so a write that fsyncs after releasing
// the lock is safe against the segment being retired meanwhile.
struct RecordLog::Segment {
    std::uint32_t id = 0;
    std::filesystem::path path;  // empty: in memory
    int descriptor = -1;
    std::string memory;
    std::uint64_t size = 0;
    std::uint64_t live_bytes = 0;
    std::atomic<bool> dirty{false};
    bool retired = false;

    ~Segment() {
        if (descriptor >= 0) ::close(descriptor);
        if (retired && !path.empty()) {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }

    bool read(std::uint64_t offset, std::uint64_t length, std::string& out) const {
        out.resize(length);
        if (path.empty()) {
            if (offset + length > memory.size()) return false;
            std::memcpy(out.data(), memory.data() + offset, length);
            return true;
        }
        std::uint64_t done = 0;
        while (done < length) {
            const ssize_t got = ::pread(descriptor, out.data() + done, length - done,
                                        static_cast<off_t>(offset + done));
            if (got <= 0) return false;
            done += static_cast<std::uint64_t>(got);
        }
        return true;
    }

    bool write(const std::string& bytes) {
        if (path.empty()) {
            memory += bytes;
        } else {
            std::size_t done = 0;
            while (done < bytes.size()) {
                const ssize_t wrote = ::write(descriptor, bytes.data() + done, bytes.size() - done);
                if (wrote <= 0) return false;
                done += static_cast<std::size_t>(wrote);
            }
        }
        size += bytes.size();
        dirty = true;
        return true;
    }

    bool sync() const { return path.empty() || ::fsync(descriptor) == 0; }
};

namespace {

std::filesystem::path segment_path(const std::filesystem::path& directory, std::uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "segment-%08u.log", id);
    return directory / name;
}

}  // namespace

RecordLog::RecordLog(std::filesystem::path directory, std::int64_t tombstone_grace_ms,
                     std::uint64_t segment_bytes)
    : directory_(std::move(directory)),
      tombstone_grace_ms_(tombstone_grace_ms),
      segment_bytes_(segment_bytes) {}

RecordLog::~RecordLog() {
    for (const auto& [id, segment] : segments_) {
        if (segment->dirty) segment->sync();
    }
}

std::size_t RecordLog::bucket_of(const std::string& key) {
    return static_cast<std::size_t>(fnv64(key) % kBuckets);
}

bool RecordLog::roll_locked() {
    auto segment = std::make_shared<Segment>();
    segment->id = active_ + 1;
    if (!directory_.empty()) {
        segment->path = segment_path(directory_, segment->id);
        segment->descriptor = ::open(segment->path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0600);
        if (segment->descriptor < 0) return false;
    }
    active_ = segment->id;
    segments_[segment->id] = std::move(segment);
    return true;
}

bool RecordLog::open(std::string* reason) {
    const std::unique_lock lock(mutex_);
    collections_.clear();
    segments_.clear();
    due_.clear();
    active_ = 0;
    if (directory_.empty()) return roll_locked();

    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        if (reason) *reason = "cannot create " + directory_.string() + ": " + error.message();
        return false;
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
    std::map<std::string, std::uint64_t> purged;  // collection \0 key -> revision
    for (std::size_t position = 0; position < ids.size(); ++position) {
        const bool last = position + 1 == ids.size();
        auto segment = std::make_shared<Segment>();
        segment->id = ids[position];
        segment->path = segment_path(directory_, segment->id);
        segment->descriptor = ::open(segment->path.c_str(), O_RDWR | O_APPEND);
        if (segment->descriptor < 0) {
            if (reason) *reason = "cannot open " + segment->path.string();
            return false;
        }
        const std::uint64_t file_size = std::filesystem::file_size(segment->path, error);
        std::string bytes;
        if (!segment->read(0, file_size, bytes)) bytes.clear();
        std::uint64_t offset = 0;
        while (offset + 8 <= bytes.size()) {
            const std::uint32_t length = get_u32(bytes.data() + offset);
            if (length > kMaxFrameBytes || offset + 8 + length > bytes.size()) break;
            const std::string_view body(bytes.data() + offset + 8, length);
            if (crc32(body) != get_u32(bytes.data() + offset + 4)) break;
            const nlohmann::json document = nlohmann::json::parse(body, nullptr, false);
            const std::uint64_t frame_offset = offset;
            offset += 8 + length;
            if (!document.is_object() || !document.contains("c") || !document.contains("k") ||
                !document["c"].is_string() || !document["k"].is_string()) {
                continue;
            }
            const std::string name = document["c"].get<std::string>();
            const std::string key = document["k"].get<std::string>();
            const std::string slot = name + std::string(1, '\0') + key;
            Collection& collection = collections_[name];
            if (document.contains("p") && document["p"].is_number_unsigned()) {
                const auto revision = document["p"].get<std::uint64_t>();
                auto& floor = purged[slot];
                floor = std::max(floor, revision);
                if (const auto found = collection.records.find(key);
                    found != collection.records.end() && found->second.revision <= revision) {
                    erase_locked(collection, key);
                }
                continue;
            }
            if (!document.contains("r")) continue;
            const auto record = parse_stored(name, key, document["r"]);
            if (!record) continue;
            if (const auto floor = purged.find(slot);
                floor != purged.end() && record->revision <= floor->second) {
                continue;
            }
            if (const auto found = collection.records.find(key);
                found != collection.records.end() && found->second.revision >= record->revision) {
                continue;
            }
            Entry entry;
            entry.revision = record->revision;
            entry.updated_ms = record->updated_ms;
            entry.expires_ms = record->expires_ms;
            entry.deleted = record->deleted;
            entry.index = record->index;
            entry.order = record->order;
            entry.segment = segment->id;
            entry.offset = frame_offset;
            entry.length = 8 + length;
            segments_[segment->id] = segment;  // place_locked accounts live bytes here
            place_locked(collection, name, key, std::move(entry));
        }
        if (offset < bytes.size()) {
            std::fprintf(stderr, "zetta: record log %s: unreadable frame at %llu of %llu%s\n",
                         segment->path.c_str(), static_cast<unsigned long long>(offset),
                         static_cast<unsigned long long>(bytes.size()),
                         last ? "; truncating the torn tail" : "; the rest is skipped");
            if (last && ::ftruncate(segment->descriptor, static_cast<off_t>(offset)) == 0) {
                bytes.resize(offset);
            }
        }
        segment->size = last ? offset : bytes.size();
        segments_[segment->id] = segment;
        active_ = segment->id;
    }
    if (segments_.empty() || segments_[active_]->size >= segment_bytes_) return roll_locked();
    return true;
}

bool RecordLog::append_locked(const std::string& bytes, std::uint32_t& segment,
                              std::uint64_t& offset, std::shared_ptr<Segment>& written_to) {
    if (segments_[active_]->size >= segment_bytes_ && !roll_locked()) return false;
    written_to = segments_[active_];
    offset = written_to->size;
    segment = active_;
    return written_to->write(bytes);
}

void RecordLog::place_locked(Collection& collection, const std::string& name,
                             const std::string& key, Entry entry) {
    erase_locked(collection, key);
    collection.digests[bucket_of(key)] ^= version_hash(key, entry.revision, entry.deleted);
    if (const auto segment = segments_.find(entry.segment); segment != segments_.end()) {
        segment->second->live_bytes += entry.length;
    }
    if (!entry.deleted) {
        for (const auto& [index, value] : entry.index) {
            collection.indexes[index_slot(index, value)].emplace(entry.order, key);
            index_bytes_ += index.size() + value.size() + entry.order.size() + key.size() + 64;
        }
    }
    if (entry.expires_ms) {
        due_.emplace(*entry.expires_ms, Due{name, key, entry.revision});
    } else if (entry.deleted) {
        due_.emplace(entry.updated_ms + tombstone_grace_ms_, Due{name, key, entry.revision});
    }
    collection.records.insert_or_assign(key, std::move(entry));
}

void RecordLog::erase_locked(Collection& collection, const std::string& key) {
    const auto found = collection.records.find(key);
    if (found == collection.records.end()) return;
    const Entry& entry = found->second;
    collection.digests[bucket_of(key)] ^= version_hash(key, entry.revision, entry.deleted);
    if (const auto segment = segments_.find(entry.segment); segment != segments_.end()) {
        segment->second->live_bytes -= std::min<std::uint64_t>(segment->second->live_bytes, entry.length);
    }
    if (!entry.deleted) {
        for (const auto& [index, value] : entry.index) {
            const auto slot = collection.indexes.find(index_slot(index, value));
            if (slot == collection.indexes.end()) continue;
            slot->second.erase({entry.order, key});
            index_bytes_ -= std::min<std::uint64_t>(
                index_bytes_, index.size() + value.size() + entry.order.size() + key.size() + 64);
            if (slot->second.empty()) collection.indexes.erase(slot);
        }
    }
    collection.records.erase(found);
}

std::optional<StoredRecord> RecordLog::load_locked(const std::string& collection,
                                                   const std::string& key, const Entry& entry,
                                                   bool with_value) const {
    StoredRecord record;
    record.collection = collection;
    record.key = key;
    record.revision = entry.revision;
    record.updated_ms = entry.updated_ms;
    record.expires_ms = entry.expires_ms;
    record.deleted = entry.deleted;
    record.index = entry.index;
    record.order = entry.order;
    if (!with_value || entry.deleted) return record;
    const auto segment = segments_.find(entry.segment);
    std::string bytes;
    if (segment == segments_.end() || entry.length < 8 ||
        !segment->second->read(entry.offset + 8, entry.length - 8, bytes)) {
        return std::nullopt;
    }
    const nlohmann::json document = nlohmann::json::parse(bytes, nullptr, false);
    if (!document.is_object() || !document.contains("r") || !document["r"].is_object()) {
        return std::nullopt;
    }
    const auto value = document["r"].find("value");
    if (value == document["r"].end() || !value->is_string()) return std::nullopt;
    record.value = value->get<std::string>();
    return record;
}

RecordLog::Written RecordLog::update(const std::string& name, const std::string& key,
                                     const Decide& decide, bool with_value, bool sync) {
    Written written;
    std::shared_ptr<Segment> segment;
    {
        const std::unique_lock lock(mutex_);
        Collection& collection = collections_[name];
        std::optional<StoredRecord> current;
        if (const auto found = collection.records.find(key); found != collection.records.end()) {
            current = load_locked(name, key, found->second, with_value);
        }
        std::optional<StoredRecord> next = decide(current ? &*current : nullptr);
        if (!next) return written;
        next->collection = name;
        next->key = key;
        const std::string bytes = frame({{"c", name}, {"k", key}, {"r", stored_value(*next)}});
        Entry entry;
        if (!append_locked(bytes, entry.segment, entry.offset, segment)) {
            written.durable = false;
            return written;
        }
        entry.length = static_cast<std::uint32_t>(bytes.size());
        entry.revision = next->revision;
        entry.updated_ms = next->updated_ms;
        entry.expires_ms = next->expires_ms;
        entry.deleted = next->deleted;
        entry.index = next->index;
        entry.order = next->order;
        place_locked(collection, name, key, std::move(entry));
        written.record = std::move(next);
    }
    // Flushed outside the lock: readers never wait for the disk.
    if (sync) written.durable = segment->sync();
    return written;
}

std::optional<StoredRecord> RecordLog::get(const std::string& name, const std::string& key,
                                           bool with_value) const {
    const std::shared_lock lock(mutex_);
    const auto collection = collections_.find(name);
    if (collection == collections_.end()) return std::nullopt;
    const auto found = collection->second.records.find(key);
    if (found == collection->second.records.end()) return std::nullopt;
    return load_locked(name, key, found->second, with_value);
}

void RecordLog::scan_index(const std::string& name, const std::string& index,
                           const std::string& value,
                           const std::optional<std::pair<std::string, std::string>>& after,
                           bool descending, const Visitor& visit) const {
    const std::shared_lock lock(mutex_);
    const auto collection = collections_.find(name);
    if (collection == collections_.end()) return;
    const auto slot = collection->second.indexes.find(index_slot(index, value));
    if (slot == collection->second.indexes.end()) return;
    const auto& members = slot->second;
    const auto emit = [&](const std::pair<std::string, std::string>& member) {
        const auto found = collection->second.records.find(member.second);
        if (found == collection->second.records.end()) return true;
        auto record = load_locked(name, member.second, found->second, true);
        return !record || visit(std::move(*record));
    };
    if (!descending) {
        for (auto member = after ? members.upper_bound(*after) : members.begin();
             member != members.end(); ++member) {
            if (!emit(*member)) return;
        }
        return;
    }
    auto member = after ? members.lower_bound(*after) : members.end();
    while (member != members.begin()) {
        --member;
        if (!emit(*member)) return;
    }
}

void RecordLog::scan_keys(const std::string& name, const std::optional<std::string>& from,
                          bool inclusive, bool descending, const Visitor& visit) const {
    const std::shared_lock lock(mutex_);
    const auto collection = collections_.find(name);
    if (collection == collections_.end()) return;
    const auto& records = collection->second.records;
    const auto emit = [&](const auto& entry) {
        auto record = load_locked(name, entry.first, entry.second, true);
        return !record || visit(std::move(*record));
    };
    if (!descending) {
        auto entry = !from ? records.begin()
                           : (inclusive ? records.lower_bound(*from) : records.upper_bound(*from));
        for (; entry != records.end(); ++entry) {
            if (!emit(*entry)) return;
        }
        return;
    }
    auto entry = !from ? records.end()
                       : (inclusive ? records.upper_bound(*from) : records.lower_bound(*from));
    while (entry != records.begin()) {
        --entry;
        if (!emit(*entry)) return;
    }
}

bool RecordLog::purge(const std::string& name, const std::string& key, std::uint64_t revision) {
    const std::unique_lock lock(mutex_);
    const auto collection = collections_.find(name);
    if (collection == collections_.end()) return false;
    const auto found = collection->second.records.find(key);
    if (found == collection->second.records.end() || found->second.revision != revision) {
        return false;
    }
    // The marker keeps an older frame of this key from coming back on restart.
    const std::string bytes = frame({{"c", name}, {"k", key}, {"p", revision}});
    std::uint32_t segment_id = 0;
    std::uint64_t offset = 0;
    std::shared_ptr<Segment> segment;
    if (!append_locked(bytes, segment_id, offset, segment)) return false;
    erase_locked(collection->second, key);
    if (collection->second.records.empty()) collections_.erase(collection);
    return true;
}

std::vector<RecordLog::Due> RecordLog::take_due(std::int64_t now_ms, std::size_t limit) {
    const std::unique_lock lock(mutex_);
    std::vector<Due> taken;
    while (!due_.empty() && due_.begin()->first <= now_ms && taken.size() < limit) {
        Due due = std::move(due_.begin()->second);
        due_.erase(due_.begin());
        const auto collection = collections_.find(due.collection);
        if (collection == collections_.end()) continue;
        const auto found = collection->second.records.find(due.key);
        if (found == collection->second.records.end() || found->second.revision != due.revision) {
            continue;
        }
        taken.push_back(std::move(due));
    }
    return taken;
}

std::map<std::string, std::array<std::uint64_t, RecordLog::kBuckets>> RecordLog::digests() const {
    const std::shared_lock lock(mutex_);
    std::map<std::string, std::array<std::uint64_t, kBuckets>> result;
    for (const auto& [name, collection] : collections_) {
        if (!collection.records.empty()) result.emplace(name, collection.digests);
    }
    return result;
}

std::vector<RecordVersion> RecordLog::versions(const std::string& name,
                                               const std::vector<std::size_t>& buckets) const {
    std::array<bool, kBuckets> wanted{};
    for (const std::size_t bucket : buckets) {
        if (bucket < kBuckets) wanted[bucket] = true;
    }
    const std::shared_lock lock(mutex_);
    std::vector<RecordVersion> result;
    const auto collection = collections_.find(name);
    if (collection == collections_.end()) return result;
    for (const auto& [key, entry] : collection->second.records) {
        if (!wanted[bucket_of(key)]) continue;
        result.push_back({key, entry.revision, entry.deleted, entry.updated_ms, entry.expires_ms});
    }
    return result;
}

void RecordLog::maintain() {
    const std::unique_lock maintenance(maintenance_, std::try_to_lock);
    if (!maintenance.owns_lock()) return;
    std::vector<std::shared_ptr<Segment>> dirty;
    {
        const std::shared_lock lock(mutex_);
        for (const auto& [id, segment] : segments_) {
            if (segment->dirty) dirty.push_back(segment);
        }
    }
    for (const auto& segment : dirty) {
        segment->dirty = false;
        segment->sync();
    }
    compact_step();
}

void RecordLog::compact_step() {
    // Retire dead sealed segments oldest-first. Oldest-first keeps every purge
    // marker until no older frame of its key remains.
    std::shared_ptr<Segment> oldest;
    {
        const std::unique_lock lock(mutex_);
        while (!segments_.empty()) {
            const auto first = segments_.begin();
            if (first->first == active_ || first->second->live_bytes > 0) break;
            first->second->retired = true;
            segments_.erase(first);
            compaction_offset_ = 0;
        }
        const auto first = segments_.begin();
        if (first == segments_.end() || first->first == active_) return;
        // Move live frames forward from a mostly-dead oldest segment, or from any
        // oldest segment once several have accumulated behind it.
        const bool mostly_dead = first->second->live_bytes * 2 < first->second->size;
        if (!mostly_dead && segments_.size() <= 4) return;
        oldest = first->second;
    }
    // Read and parse a chunk outside the lock: a sealed segment never changes.
    const std::uint64_t begin = compaction_offset_;
    const std::uint64_t end = std::min(oldest->size, begin + kCompactionChunk);
    std::string bytes;
    if (end <= begin || !oldest->read(begin, end - begin, bytes)) {
        compaction_offset_ = 0;
        return;
    }
    struct Candidate {
        std::string collection;
        std::string key;
        std::uint64_t offset;
        std::string frame;
    };
    std::vector<Candidate> candidates;
    std::uint64_t position = 0;
    while (position + 8 <= bytes.size()) {
        const std::uint32_t length = get_u32(bytes.data() + position);
        if (position + 8 + length > bytes.size()) break;
        const nlohmann::json document =
            nlohmann::json::parse(std::string_view(bytes.data() + position + 8, length), nullptr, false);
        if (document.is_object() && document.contains("r") && document.contains("c") &&
            document.contains("k") && document["c"].is_string() && document["k"].is_string()) {
            candidates.push_back({document["c"].get<std::string>(), document["k"].get<std::string>(),
                                  begin + position, bytes.substr(position, 8 + length)});
        }
        position += 8 + length;
    }
    if (position == 0 && end < oldest->size) {
        // A frame larger than the chunk: read it whole next time.
        const std::uint32_t length = get_u32(bytes.data());
        std::string whole;
        if (oldest->read(begin, 8 + length, whole)) {
            const nlohmann::json document =
                nlohmann::json::parse(std::string_view(whole.data() + 8, length), nullptr, false);
            if (document.is_object() && document.contains("r") && document["c"].is_string() &&
                document["k"].is_string()) {
                candidates.push_back({document["c"].get<std::string>(),
                                      document["k"].get<std::string>(), begin, whole});
            }
        }
        position = 8 + length;
    }
    {
        const std::unique_lock lock(mutex_);
        for (Candidate& candidate : candidates) {
            const auto collection = collections_.find(candidate.collection);
            if (collection == collections_.end()) continue;
            const auto found = collection->second.records.find(candidate.key);
            if (found == collection->second.records.end() ||
                found->second.segment != oldest->id || found->second.offset != candidate.offset) {
                continue;  // superseded or purged: dead
            }
            std::uint32_t segment_id = 0;
            std::uint64_t offset = 0;
            std::shared_ptr<Segment> written_to;
            if (!append_locked(candidate.frame, segment_id, offset, written_to)) return;
            oldest->live_bytes -= std::min<std::uint64_t>(oldest->live_bytes, found->second.length);
            found->second.segment = segment_id;
            found->second.offset = offset;
            written_to->live_bytes += found->second.length;
        }
        compaction_offset_ = begin + position >= oldest->size ? 0 : begin + position;
    }
}

RecordStorageStats RecordLog::stats() const {
    const std::shared_lock lock(mutex_);
    RecordStorageStats stats;
    stats.segments = segments_.size();
    for (const auto& [id, segment] : segments_) {
        stats.disk_bytes += segment->size;
        stats.live_bytes += segment->live_bytes;
    }
    for (const auto& [name, collection] : collections_) {
        stats.records += collection.records.size();
        for (const auto& [key, entry] : collection.records) {
            stats.memory_bytes += key.size() + entry.order.size() + sizeof(Entry) + 64;
            for (const auto& [index, value] : entry.index) stats.memory_bytes += index.size() + value.size() + 64;
        }
    }
    stats.memory_bytes += index_bytes_ + due_.size() * (sizeof(Due) + 64);
    return stats;
}

}  // namespace zetta::data
