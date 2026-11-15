#include "zetta/governance/audit_event.hpp"

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <queue>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::governance {
namespace fs = std::filesystem;

namespace {

// The AuditEvent JSON Schema (draft-07). One declarative source of truth for the
// value shape carried by audit/<uuid>.json (with an optional `id`) and the
// `audit:<uuid>` CRDT value (without it). `action` is a closed enum and the
// shape is sealed (additionalProperties: false), so a malformed record fails
// validation at the boundary instead of corrupting the trail. This embedded schema
// is the source of truth; schemas/audit_event.schema.json is generated from it
// (tests/test_schemas.cpp) and a drift check keeps it in sync.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.AuditEvent",
      "type": "object",
      "properties": {
        "id":      { "type": "string" },
        "action":  { "type": "string",
                     "enum": ["graph_created", "graph_dropped",
                              "graph_deployed", "graph_approved", "data_access",
                              "data_access_summary",
                              "job_created", "job_finished",
                              "key_issued", "key_revoked",
                              "provision_requested",
                              "cloud_inspected", "dataset_added",
                              "consent_requested", "consent_decided",
                              "consent_bypassed", "decision_policy_created",
                              "decision_policy_updated", "decision_approved",
                              "pubsub_topic_updated", "pubsub_event_updated", "pubsub_event_deleted",
                              "audit_purged", "data_changed",
                              "egress_denied", "safe_mode_changed",
                              "safe_mode_confirmed"] },
        "actor":   { "type": "string" },
        "target":  { "type": "string" },
        "allowed": { "type": "boolean" },
        "detail":  { "type": "string" },
        "time":    { "type": "string" },
        "surface": { "type": "string",
                     "enum": ["mcp", "rest", "web", "desktop", "pgwire",
                              "console", "internal"] },
        "summary": {
          "type": "object",
          "properties": {
            "calls": { "type": "integer", "minimum": 1 },
            "events_delivered": { "type": "integer", "minimum": 0 },
            "scope": { "type": "string" },
            "window_started": { "type": "string" },
            "window_ended": { "type": "string" }
          },
          "required": ["calls", "events_delivered", "scope",
                       "window_started", "window_ended"],
          "additionalProperties": false
        }
      },
      "required": ["action", "time"],
      "allOf": [{
        "if": {
          "properties": { "action": { "const": "data_access_summary" } }
        },
        "then": { "required": ["summary"] },
        "else": { "not": { "required": ["summary"] } }
      }],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
// validate() is then cheap and runs at every boundary.
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. Using the error-handler overload of
// validate() (rather than the throwing one) keeps validation failures from
// crossing the validator-library boundary as exceptions — the library's
// recommended path, and robust regardless of how each side is compiled.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

std::string_view to_string(AuditAction action) {
    switch (action) {
        case AuditAction::GraphCreated:  return "graph_created";
        case AuditAction::GraphDropped:  return "graph_dropped";
        case AuditAction::GraphDeployed: return "graph_deployed";
        case AuditAction::GraphApproved: return "graph_approved";
        case AuditAction::DataAccess:    return "data_access";
        case AuditAction::DataAccessSummary: return "data_access_summary";
        case AuditAction::JobCreated:    return "job_created";
        case AuditAction::JobFinished:   return "job_finished";
        case AuditAction::KeyIssued:     return "key_issued";
        case AuditAction::KeyRevoked:    return "key_revoked";
        case AuditAction::ProvisionRequested: return "provision_requested";
        case AuditAction::CloudInspected: return "cloud_inspected";
        case AuditAction::DatasetAdded:   return "dataset_added";
        case AuditAction::ConsentRequested: return "consent_requested";
        case AuditAction::ConsentDecided:   return "consent_decided";
        case AuditAction::ConsentBypassed:  return "consent_bypassed";
        case AuditAction::DecisionPolicyCreated: return "decision_policy_created";
        case AuditAction::DecisionPolicyUpdated: return "decision_policy_updated";
        case AuditAction::DecisionApproved: return "decision_approved";
        case AuditAction::PubSubTopicUpdated: return "pubsub_topic_updated";
        case AuditAction::PubSubEventUpdated: return "pubsub_event_updated";
        case AuditAction::PubSubEventDeleted: return "pubsub_event_deleted";
        case AuditAction::AuditPurged: return "audit_purged";
        case AuditAction::DataChanged: return "data_changed";
        case AuditAction::EgressDenied: return "egress_denied";
        case AuditAction::SafeModeChanged: return "safe_mode_changed";
        case AuditAction::SafeModeConfirmed: return "safe_mode_confirmed";
    }
    return "data_access";
}

std::optional<AuditAction> audit_action_from(std::string_view name) {
    if (name == "graph_created")  return AuditAction::GraphCreated;
    if (name == "graph_dropped")  return AuditAction::GraphDropped;
    if (name == "graph_deployed") return AuditAction::GraphDeployed;
    if (name == "graph_approved") return AuditAction::GraphApproved;
    if (name == "data_access")    return AuditAction::DataAccess;
    if (name == "data_access_summary") return AuditAction::DataAccessSummary;
    if (name == "job_created")    return AuditAction::JobCreated;
    if (name == "job_finished")   return AuditAction::JobFinished;
    if (name == "key_issued")     return AuditAction::KeyIssued;
    if (name == "key_revoked")    return AuditAction::KeyRevoked;
    if (name == "provision_requested") return AuditAction::ProvisionRequested;
    if (name == "cloud_inspected") return AuditAction::CloudInspected;
    if (name == "dataset_added")   return AuditAction::DatasetAdded;
    if (name == "consent_requested") return AuditAction::ConsentRequested;
    if (name == "consent_decided")   return AuditAction::ConsentDecided;
    if (name == "consent_bypassed")  return AuditAction::ConsentBypassed;
    if (name == "decision_policy_created") return AuditAction::DecisionPolicyCreated;
    if (name == "decision_policy_updated") return AuditAction::DecisionPolicyUpdated;
    if (name == "decision_approved") return AuditAction::DecisionApproved;
    if (name == "pubsub_topic_updated") return AuditAction::PubSubTopicUpdated;
    if (name == "pubsub_event_updated") return AuditAction::PubSubEventUpdated;
    if (name == "pubsub_event_deleted") return AuditAction::PubSubEventDeleted;
    if (name == "audit_purged") return AuditAction::AuditPurged;
    if (name == "data_changed") return AuditAction::DataChanged;
    if (name == "egress_denied") return AuditAction::EgressDenied;
    if (name == "safe_mode_changed") return AuditAction::SafeModeChanged;
    if (name == "safe_mode_confirmed") return AuditAction::SafeModeConfirmed;
    return std::nullopt;
}

nlohmann::json to_json(const AuditEventValue& value) {
    nlohmann::json result{{"action", to_string(value.action)},
                          {"actor", value.actor},
                          {"target", value.target},
                          {"allowed", value.allowed},
                          {"detail", value.detail},
                          {"time", value.time}};
    if (!value.surface.empty()) result["surface"] = value.surface;
    if (value.summary) {
        result["summary"] = {
            {"calls", value.summary->calls},
            {"events_delivered", value.summary->events_delivered},
            {"scope", value.summary->scope},
            {"window_started", value.summary->window_started},
            {"window_ended", value.summary->window_ended}};
    }
    return result;
}

const nlohmann::json& audit_event_value_schema() { return schema_document(); }

bool validate_audit_event_value(const nlohmann::json& value,
                                std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<AuditEventValue> parse_audit_event_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_audit_event_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the action enum, so extraction
    // is total: missing optional fields fall back to defaults.
    AuditEventValue parsed;
    parsed.action =
        audit_action_from(value.value("action", std::string{"data_access"}))
            .value_or(AuditAction::DataAccess);
    parsed.actor = value.value("actor", std::string{});
    parsed.target = value.value("target", std::string{});
    parsed.allowed = value.value("allowed", true);
    parsed.detail = value.value("detail", std::string{});
    parsed.time = value.value("time", std::string{});
    parsed.surface = value.value("surface", std::string{});
    if (value.contains("summary")) {
        const nlohmann::json& summary = value.at("summary");
        parsed.summary = AuditReadSummary{
            summary.at("calls").get<std::uint64_t>(),
            summary.at("events_delivered").get<std::uint64_t>(),
            summary.at("scope").get<std::string>(),
            summary.at("window_started").get<std::string>(),
            summary.at("window_ended").get<std::string>()};
    }
    return parsed;
}

// ----- AuditEvent ------------------------------------------------------------

AuditEvent::AuditEvent(std::string id, AuditEventValue value, fs::path dir)
    : id_(std::move(id)), value_(std::move(value)), dir_(std::move(dir)) {}

nlohmann::json AuditEvent::metadata() const { return to_json(value_); }

void AuditEvent::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(file(), std::ios::trunc);
    out << meta.dump(2) << '\n';
}

// ----- AuditTrail -----------------------------------------------------------

namespace {

constexpr std::uint64_t kSegmentUncompressedBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kSyncEveryRecords = 64;
constexpr std::uint32_t kMaximumFrameBytes = 16U * 1024U * 1024U;

std::uint32_t audit_crc32(std::string_view bytes) {
    return static_cast<std::uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef*>(bytes.data()),
                static_cast<uInt>(bytes.size())));
}

void append_u32(std::string& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

void append_u64(std::string& output, std::uint64_t value) {
    append_u32(output, static_cast<std::uint32_t>(value));
    append_u32(output, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t read_u32(std::string_view input, std::size_t offset) {
    std::uint32_t value = 0;
    for (int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(input[offset + byte]))
                 << (byte * 8);
    }
    return value;
}

std::uint64_t read_u64(std::string_view input, std::size_t offset) {
    return read_u32(input, offset) |
           (static_cast<std::uint64_t>(read_u32(input, offset + 4)) << 32);
}

std::uint64_t audit_id_hash(std::string_view id) {
    // Audit ids are random UUIDs. FNV-1a plus the CRC keeps
    // a compact 96-bit membership fingerprint without retaining full ids.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : id) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint32_t audit_id_checksum(std::string_view id) {
    return audit_crc32(id);
}

// Membership needs only the 96-bit fingerprint; range positions are stored
// separately. Flat slots avoid a node allocation and duplicate location for
// every event. There are no individual deletions: retention rebuilds the index.
class AuditFingerprintIndex {
public:
    bool contains(std::uint64_t hash, std::uint32_t checksum) const {
        if (slots_.empty()) {
            return false;
        }
        std::size_t position = hash & (slots_.size() - 1);
        while (slots_[position].occupied) {
            if (slots_[position].hash == hash && slots_[position].checksum == checksum) {
                return true;
            }
            position = (position + 1) & (slots_.size() - 1);
        }
        return false;
    }

    void insert(std::uint64_t hash, std::uint32_t checksum) {
        if ((size_ + 1) * 10 > slots_.size() * 7) {
            auto previous = std::move(slots_);
            slots_.resize(previous.empty() ? 1024 : previous.size() * 2);
            size_ = 0;
            for (const auto& slot : previous) {
                if (slot.occupied) {
                    insert(slot.hash, slot.checksum);
                }
            }
        }
        std::size_t position = hash & (slots_.size() - 1);
        while (slots_[position].occupied) {
            if (slots_[position].hash == hash && slots_[position].checksum == checksum) {
                return;
            }
            position = (position + 1) & (slots_.size() - 1);
        }
        slots_[position] = {hash, checksum, true};
        ++size_;
    }

    void clear() {
        std::vector<Slot>().swap(slots_);
        size_ = 0;
    }

    std::size_t size() const {
        return size_;
    }

    std::size_t memory_bytes() const {
        return slots_.capacity() * sizeof(Slot);
    }

private:
    struct Slot {
        std::uint64_t hash = 0;
        std::uint32_t checksum = 0;
        bool occupied = false;
    };
    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

std::string segment_name(std::uint32_t number) {
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "segment-%08u.audit.gz", number);
    return buffer.data();
}

std::optional<std::uint32_t> segment_number(const fs::path& path) {
    const std::string name = path.filename().string();
    constexpr std::string_view prefix = "segment-";
    constexpr std::string_view suffix = ".audit.gz";
    if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
        return std::nullopt;
    }
    const std::string digits =
        name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    std::uint32_t number = 0;
    const auto result = std::from_chars(
        digits.data(), digits.data() + digits.size(), number);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return number;
}

bool sync_file(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDWR);
    if (descriptor < 0) {
        return false;
    }
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
}

std::string event_frame(const std::string& id, const AuditEventValue& value,
                        const std::string& origin, std::uint64_t sequence) {
    nlohmann::json record = to_json(value);
    record["id"] = id;
    record["origin"] = origin;
    record["sequence"] = sequence;
    const std::string body = record.dump();
    std::string frame;
    frame.reserve(8 + body.size());
    append_u32(frame, static_cast<std::uint32_t>(body.size()));
    append_u32(frame, audit_crc32(body));
    frame += body;
    return frame;
}

struct DecodedAuditFrame {
    std::string id;
    AuditEventValue value;
    std::size_t consumed = 0;
    std::string origin = "legacy";
    std::uint64_t sequence = 0;
};

std::optional<DecodedAuditFrame> decode_audit_frame(
    std::string_view bytes, std::string* error = nullptr) {
    if (bytes.size() < 8) {
        if (error) {
            *error = "incomplete frame header";
        }
        return std::nullopt;
    }
    const std::uint32_t length = read_u32(bytes, 0);
    const std::uint32_t expected_crc = read_u32(bytes, 4);
    if (length > kMaximumFrameBytes || bytes.size() < 8 + length) {
        if (error) {
            *error = "incomplete or oversized frame body";
        }
        return std::nullopt;
    }
    const std::string_view body = bytes.substr(8, length);
    if (audit_crc32(body) != expected_crc) {
        if (error) {
            *error = "frame checksum mismatch";
        }
        return std::nullopt;
    }
    nlohmann::json record =
        nlohmann::json::parse(body, nullptr, false);
    if (!record.is_object() || !record.contains("id") ||
        !record.at("id").is_string()) {
        if (error) {
            *error = "frame is not an audit record";
        }
        return std::nullopt;
    }
    const std::string id = record.at("id").get<std::string>();
    std::string origin = "legacy";
    std::uint64_t sequence = 0;
    if (record.contains("origin") || record.contains("sequence")) {
        if (!record.contains("origin") || !record["origin"].is_string() ||
            !record.contains("sequence") || !record["sequence"].is_number_unsigned()) {
            if (error) {
                *error = "invalid audit origin or sequence";
            }
            return std::nullopt;
        }
        origin = record["origin"].get<std::string>();
        sequence = record["sequence"].get<std::uint64_t>();
        if (!valid_audit_origin(origin) || sequence == 0) {
            return std::nullopt;
        }
        record.erase("origin");
        record.erase("sequence");
    }
    const std::optional<AuditEventValue> value =
        parse_audit_event_value(record, error);
    if (!value) {
        return std::nullopt;
    }
    return DecodedAuditFrame{id, *value, 8 + length, origin, sequence};
}

bool inflate_segment_into(const fs::path& path, std::string& bytes,
                          std::string* error = nullptr,
                          bool* unfinished = nullptr) {
    gzFile input = ::gzopen(path.c_str(), "rb");
    if (!input) {
        if (error) {
            *error = "cannot open " + path.string();
        }
        return false;
    }
    bytes.clear();
    bytes.reserve(kSegmentUncompressedBytes);
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const int read = ::gzread(input, buffer.data(),
                                  static_cast<unsigned>(buffer.size()));
        if (read > 0) {
            bytes.append(buffer.data(), static_cast<std::size_t>(read));
            continue;
        }
        int code = Z_OK;
        const char* message = ::gzerror(input, &code);
        if (unfinished) {
            *unfinished = code == Z_BUF_ERROR;
        }
        if (read < 0 && code != Z_BUF_ERROR) {
            if (error) {
                *error = message ? message : "gzip read failed";
            }
            ::gzclose(input);
            return false;
        }
        break;
    }
    ::gzclose(input);
    return true;
}

std::optional<std::string> inflate_segment(const fs::path& path,
                                           std::string* error = nullptr,
                                           bool* unfinished = nullptr) {
    std::string bytes;
    if (!inflate_segment_into(path, bytes, error, unfinished)) {
        return std::nullopt;
    }
    return bytes;
}

bool seal_recovered_segment(const fs::path& path, std::string_view bytes) {
    const fs::path temporary = path.string() + ".recovered";
    gzFile output = ::gzopen(temporary.c_str(), "wb6");
    if (!output) {
        return false;
    }
    const auto written = ::gzwrite(output, bytes.data(), static_cast<unsigned>(bytes.size()));
    const auto closed = ::gzclose(output);
    if (written != static_cast<int>(bytes.size()) || closed != Z_OK || !sync_file(temporary)) {
        return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    return !error;
}

}  // namespace

class AuditTrail::Impl {
public:
    struct LogPosition {
        std::uint64_t sequence = 0;
        std::uint32_t offset = 0;
        std::uint32_t segment = 0;
    };

    explicit Impl(fs::path directory, std::size_t recent_limit)
        : directory_(std::move(directory)), recent_limit_(recent_limit) {}

    ~Impl() {
        close_writer();
    }

    fs::path segment_path(std::uint32_t number) const {
        return directory_ / segment_name(number);
    }

    std::vector<std::uint32_t> segments() const {
        std::vector<std::uint32_t> result;
        std::error_code error_code;
        if (!fs::exists(directory_, error_code)) {
            return result;
        }
        for (const fs::directory_entry& entry :
             fs::directory_iterator(directory_, error_code)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (const std::optional<std::uint32_t> number =
                    segment_number(entry.path())) {
                result.push_back(*number);
            }
        }
        std::ranges::sort(result);
        return result;
    }

    bool contains(std::string_view id) const {
        return index_.contains(audit_id_hash(id), audit_id_checksum(id));
    }

    void remember(const std::string& id, const AuditEventValue& value) {
        index_.insert(audit_id_hash(id), audit_id_checksum(id));
        auto event = std::make_shared<AuditEvent>(id, value, directory_);
        recent_.push_back(std::move(event));
        while (recent_.size() > recent_limit_) {
            recent_.pop_front();
        }
    }

    MemoryUsage memory_usage() const {
        MemoryUsage usage;
        usage.items = index_.size();
        usage.index_bytes = index_.memory_bytes();
        for (const auto& [origin, positions] : positions_) {
            usage.index_bytes += origin.capacity() + positions.size() * sizeof(LogPosition) +
                4096 + (positions.size() / (4096 / sizeof(LogPosition)) + 1) * sizeof(void*);
        }
        usage.cache_bytes = cached_bytes_.capacity();
        for (const auto& event : recent_) {
            const auto& value = event->value();
            usage.recent_bytes += sizeof(AuditEvent) + sizeof(void*);
            usage.recent_bytes += event->id().capacity();
            usage.recent_bytes += value.actor.capacity();
            usage.recent_bytes += value.target.capacity();
            usage.recent_bytes += value.detail.capacity();
            usage.recent_bytes += value.time.capacity();
            if (value.summary) {
                usage.recent_bytes += sizeof(AuditReadSummary);
                usage.recent_bytes += value.summary->scope.capacity();
                usage.recent_bytes += value.summary->window_started.capacity();
                usage.recent_bytes += value.summary->window_ended.capacity();
            }
        }
        usage.pending_persist_items = unsynced_records_;
        usage.pending_persist_bytes = unsynced_records_ * 256;
        usage.bytes = usage.index_bytes + usage.cache_bytes + usage.recent_bytes +
                      usage.pending_persist_bytes;
        return usage;
    }

    bool write_index(const std::string& id, const std::string& origin,
                     const LogPosition& position, std::uint32_t length) {
        std::string body;
        append_u32(body, static_cast<std::uint32_t>(origin.size()));
        body += origin;
        append_u64(body, audit_id_hash(id));
        append_u32(body, audit_id_checksum(id));
        append_u32(body, position.segment);
        append_u64(body, position.offset);
        append_u64(body, position.sequence);
        append_u32(body, length);
        append_u32(body, audit_crc32(body));
        std::ofstream output(directory_ / "index.bin", std::ios::binary | std::ios::app);
        output.write(body.data(), static_cast<std::streamsize>(body.size()));
        return output.good();
    }

    bool load_index(const std::vector<std::uint32_t>& available_segments,
                    std::string* error) {
        std::ifstream input(directory_ / "index.bin", std::ios::binary);
        if (!input) {
            return false;
        }
        std::deque<LogPosition> recent_positions;
        while (input.peek() != std::char_traits<char>::eof()) {
            std::string size_bytes(4, '\0');
            if (!input.read(size_bytes.data(), 4)) {
                return false;
            }
            const auto origin_size = read_u32(size_bytes, 0);
            if (origin_size == 0 || origin_size > 128) {
                return false;
            }
            std::string body(origin_size + 40, '\0');
            if (!input.read(body.data(), body.size())) {
                return false;
            }
            if (audit_crc32(size_bytes + body.substr(0, body.size() - 4)) !=
                read_u32(body, body.size() - 4)) {
                return false;
            }
            const std::string origin = body.substr(0, origin_size);
            const std::string_view fields = std::string_view(body).substr(origin_size);
            const auto hash = read_u64(fields, 0);
            const auto checksum = read_u32(fields, 8);
            const auto offset = read_u64(fields, 16);
            const auto length = read_u32(fields, 32);
            const LogPosition position{read_u64(fields, 24), static_cast<std::uint32_t>(offset),
                                       read_u32(fields, 12)};
            if (!valid_audit_origin(origin) || position.sequence == 0 ||
                offset > kMaximumFrameBytes || length > kMaximumFrameBytes + 8 ||
                position.sequence <= watermarks_[origin].high) {
                return false;
            }
            if (!std::binary_search(available_segments.begin(), available_segments.end(), position.segment)) {
                if (error) {
                    *error = "missing audit segment " + segment_path(position.segment).string();
                }
                return false;
            }
            index_.insert(hash, checksum);
            positions_[origin].push_back(position);
            watermarks_[origin].high = position.sequence;
            recent_positions.push_back(position);
            while (recent_positions.size() > recent_limit_) {
                recent_positions.pop_front();
            }
            if (position.segment >= current_segment_) {
                current_segment_ = position.segment;
                current_offset_ = offset + length;
            }
        }
        for (const auto& position : recent_positions) {
            const auto frame = frame_at(position);
            if (!frame) {
                return false;
            }
            const auto decoded = decode_audit_frame(*frame);
            if (!decoded) {
                return false;
            }
            recent_.push_back(std::make_shared<AuditEvent>(
                decoded->id, decoded->value, directory_));
        }
        return true;
    }

    const std::string* segment_bytes(std::uint32_t number) const {
        if (cached_segment_ != number) {
            cached_segment_ = std::numeric_limits<std::uint32_t>::max();
            if (!inflate_segment_into(segment_path(number), cached_bytes_)) {
                return nullptr;
            }
            cached_segment_ = number;
        }
        return &cached_bytes_;
    }

    std::optional<std::string_view> frame_at(const LogPosition& position) const {
        const auto* bytes = segment_bytes(position.segment);
        if (!bytes || position.offset > bytes->size() || bytes->size() - position.offset < 8) {
            return std::nullopt;
        }
        const auto length = read_u32(*bytes, position.offset);
        if (length > kMaximumFrameBytes || length + 8 > bytes->size() - position.offset) {
            return std::nullopt;
        }
        return std::string_view(*bytes).substr(position.offset, length + 8);
    }

    template <typename Visitor>
    bool visit_positions(Visitor&& visitor) const {
        struct Cursor {
            std::reference_wrapper<const std::string> origin;
            std::deque<LogPosition>::const_iterator position;
            std::deque<LogPosition>::const_iterator end;
        };
        const auto later = [](const Cursor& left, const Cursor& right) {
            return std::pair{left.position->segment, left.position->offset} >
                std::pair{right.position->segment, right.position->offset};
        };
        std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> pending(later);
        for (const auto& [origin, positions] : positions_) {
            if (!positions.empty()) {
                pending.push({std::cref(origin), positions.begin(), positions.end()});
            }
        }
        while (!pending.empty()) {
            auto cursor = pending.top();
            pending.pop();
            if (!visitor(cursor.origin.get(), *cursor.position)) {
                return false;
            }
            ++cursor.position;
            if (cursor.position != cursor.end) {
                pending.push(cursor);
            }
        }
        return true;
    }

    bool persist_watermarks() {
        std::string bytes;
        for (const auto& [origin, watermark] : watermarks_) {
            append_u32(bytes, static_cast<std::uint32_t>(origin.size()));
            bytes += origin;
            append_u64(bytes, watermark.high);
            append_u64(bytes, watermark.low);
            append_u64(bytes, watermark.summary_low);
        }
        append_u32(bytes, audit_crc32(bytes));
        const fs::path temporary = directory_ / "watermarks.tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(bytes.data(), bytes.size());
            if (!output.good()) {
                return false;
            }
        }
        if (!sync_file(temporary)) {
            return false;
        }
        std::error_code error;
        fs::rename(temporary, directory_ / "watermarks.bin", error);
        return !error;
    }

    bool load_watermarks() {
        std::ifstream input(directory_ / "watermarks.bin", std::ios::binary);
        if (!input) {
            return true;
        }
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        if (bytes.size() < 4 || audit_crc32(std::string_view(bytes).substr(0, bytes.size() - 4)) !=
            read_u32(bytes, bytes.size() - 4)) {
            return false;
        }
        const auto parse = [&](bool has_summary_low)
            -> std::optional<std::map<std::string, AuditWatermark>> {
            std::map<std::string, AuditWatermark> parsed;
            std::size_t offset = 0;
            const std::size_t watermark_bytes = has_summary_low ? 24 : 16;
            while (offset + 4 < bytes.size()) {
                const auto length = read_u32(bytes, offset);
                offset += 4;
                if (length == 0 || length > 128 ||
                    offset + length + watermark_bytes > bytes.size() - 4) {
                    return std::nullopt;
                }
                const auto origin = bytes.substr(offset, length);
                offset += length;
                AuditWatermark watermark;
                watermark.high = read_u64(bytes, offset);
                watermark.low = read_u64(bytes, offset + 8);
                if (has_summary_low) {
                    watermark.summary_low = read_u64(bytes, offset + 16);
                }
                offset += watermark_bytes;
                if (!valid_audit_origin(origin) ||
                    watermark.low > watermark.high ||
                    watermark.summary_low > watermark.high ||
                    !parsed.emplace(origin, watermark).second) {
                    return std::nullopt;
                }
            }
            if (offset != bytes.size() - 4) {
                return std::nullopt;
            }
            return parsed;
        };
        auto parsed = parse(true);
        if (!parsed) {
            parsed = parse(false);
        }
        if (!parsed) {
            return false;
        }
        for (const auto& [origin, watermark] : *parsed) {
            auto& local = watermarks_[origin];
            if (watermark.high > local.high &&
                watermark.high > std::max(watermark.low,
                                          watermark.summary_low)) {
                return false;
            }
            local.high = std::max(local.high, watermark.high);
            local.low = std::max(local.low, watermark.low);
            local.summary_low =
                std::max(local.summary_low, watermark.summary_low);
        }
        return true;
    }

    bool load_segment(std::uint32_t number, std::string* error) {
        bool unfinished = false;
        const std::optional<std::string> bytes =
            inflate_segment(segment_path(number), error, &unfinished);
        if (!bytes) {
            return false;
        }
        std::size_t offset = 0;
        while (offset < bytes->size()) {
            std::string frame_error;
            const std::optional<DecodedAuditFrame> decoded =
                decode_audit_frame(std::string_view(*bytes).substr(offset),
                                   &frame_error);
            if (!decoded) {
                std::printf("zetta: audit segment %s stops at byte %zu (%s)\n",
                            segment_path(number).filename().string().c_str(),
                            offset, frame_error.c_str());
                break;
            }
            if (!contains(decoded->id)) {
                const auto sequence = decoded->sequence == 0
                    ? watermarks_[decoded->origin].high + 1 : decoded->sequence;
                const LogPosition position{sequence, static_cast<std::uint32_t>(offset), number};
                remember(decoded->id, decoded->value);
                positions_[decoded->origin].push_back(position);
                watermarks_[decoded->origin].high =
                    std::max(watermarks_[decoded->origin].high, sequence);
                if (!write_index(decoded->id, decoded->origin, position,
                                 static_cast<std::uint32_t>(decoded->consumed))) {
                    return false;
                }
            }
            offset += decoded->consumed;
        }
        if (number == current_segment_) {
            current_offset_ = offset;
            // A crashed writer may leave a valid Z_SYNC_FLUSH prefix without a
            // gzip trailer. Seal that bounded prefix before appending a new
            // member; otherwise later members would be unreadable on restart.
            if ((unfinished || offset != bytes->size()) &&
                !seal_recovered_segment(segment_path(number), std::string_view(*bytes).substr(0, offset))) {
                if (error) {
                    *error = "cannot seal recovered audit tail";
                }
                return false;
            }
        } else if (unfinished || offset != bytes->size()) {
            if (error) {
                *error = "damaged historical audit segment";
            }
            return false;
        }
        return true;
    }

    bool open_writer(std::string* error) {
        if (writer_) {
            return true;
        }
        const fs::path path = segment_path(current_segment_);
        writer_ = ::gzopen(path.c_str(), "ab6");
        if (!writer_) {
            if (error) {
                *error = "cannot open audit segment " + path.string();
            }
            return false;
        }
        return true;
    }

    void close_writer() {
        if (!writer_) {
            return;
        }
        ::gzflush(writer_, Z_FINISH);
        ::gzclose(writer_);
        writer_ = nullptr;
        sync_file(segment_path(current_segment_));
        unsynced_records_ = 0;
    }

    bool sync_writer() {
        if (!writer_) {
            return true;
        }
        if (::gzflush(writer_, Z_SYNC_FLUSH) != Z_OK) {
            return false;
        }
        if (!sync_file(segment_path(current_segment_))) {
            return false;
        }
        unsynced_records_ = 0;
        return sync_file(directory_ / "index.bin") && persist_watermarks();
    }

    bool append(std::string id, AuditEventValue value, std::string* error,
                const std::string& origin = "legacy", std::uint64_t sequence = 0) {
        if (contains(id)) {
            return true;
        }
        if (!valid_audit_origin(origin) ||
            watermarks_[origin].high == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        if (sequence == 0) {
            sequence = watermarks_[origin].high + 1;
        }
        const std::string frame = event_frame(id, value, origin, sequence);
        if (frame.size() > kMaximumFrameBytes) {
            if (error) {
                *error = "audit event exceeds maximum frame size";
            }
            return false;
        }
        if (current_offset_ > 0 &&
            current_offset_ + frame.size() > kSegmentUncompressedBytes) {
            close_writer();
            ++current_segment_;
            current_offset_ = 0;
        }
        if (!open_writer(error)) {
            return false;
        }
        const int written = ::gzwrite(writer_, frame.data(),
                                      static_cast<unsigned>(frame.size()));
        if (written != static_cast<int>(frame.size())) {
            if (error) {
                int code = Z_OK;
                const char* message = ::gzerror(writer_, &code);
                *error = message ? message : "gzip write failed";
            }
            return false;
        }
        const LogPosition position{sequence, static_cast<std::uint32_t>(current_offset_), current_segment_};
        current_offset_ += frame.size();
        ++unsynced_records_;
        remember(id, value);
        positions_[origin].push_back(position);
        watermarks_[origin].high = std::max(watermarks_[origin].high, sequence);
        if (cached_segment_ == current_segment_) {
            cached_segment_ = std::numeric_limits<std::uint32_t>::max();
        }
        if (::gzflush(writer_, Z_SYNC_FLUSH) != Z_OK) {
            if (error) {
                *error = "cannot flush audit segment " +
                         segment_path(current_segment_).string();
            }
            return false;
        }
        if (!write_index(id, origin, position, static_cast<std::uint32_t>(frame.size()))) {
            if (error) {
                *error = "cannot append audit index";
            }
            return false;
        }
        if (unsynced_records_ >= kSyncEveryRecords && !sync_writer()) {
            if (error) {
                *error = "cannot sync audit segment " +
                         segment_path(current_segment_).string();
            }
            return false;
        }
        if (unsynced_records_ >= kSyncEveryRecords) {
            unsynced_records_ = 0;
        }
        return true;
    }

    void for_each(const EventVisitor& visitor) const {
        for (const std::uint32_t number : segments()) {
            const std::optional<std::string> bytes =
                inflate_segment(segment_path(number));
            if (!bytes) {
                continue;
            }
            std::size_t offset = 0;
            while (offset < bytes->size()) {
                const std::optional<DecodedAuditFrame> decoded =
                    decode_audit_frame(std::string_view(*bytes).substr(offset));
                if (!decoded) {
                    break;
                }
                visitor(std::make_shared<AuditEvent>(
                    decoded->id, decoded->value, directory_));
                offset += decoded->consumed;
            }
        }
    }

    bool migrate_legacy_files(std::string* error) {
        std::vector<fs::path> legacy_files;
        std::vector<fs::path> durable_files;
        std::error_code error_code;
        for (const fs::directory_entry& entry :
             fs::directory_iterator(directory_, error_code)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                legacy_files.push_back(entry.path());
            }
        }
        std::ranges::sort(legacy_files);
        const auto remove_durable_files = [&]() {
            if (durable_files.empty()) {
                return true;
            }
            if (!sync_writer()) {
                if (error) {
                    *error = "cannot sync migrated audit records";
                }
                return false;
            }
            for (const fs::path& durable : durable_files) {
                error_code.clear();
                fs::remove(durable, error_code);
                if (error_code) {
                    if (error) {
                        *error = "cannot remove migrated audit file " +
                                 durable.string();
                    }
                    return false;
                }
            }
            durable_files.clear();
            return true;
        };
        for (const fs::path& path : legacy_files) {
            std::ifstream input(path);
            const nlohmann::json record =
                nlohmann::json::parse(input, nullptr, false);
            std::string parse_error;
            const std::optional<AuditEventValue> value =
                record.is_discarded()
                    ? std::nullopt
                    : parse_audit_event_value(record, &parse_error);
            if (!value) {
                std::printf("zetta: skipping invalid audit event %s (%s)\n",
                            path.filename().string().c_str(),
                            record.is_discarded() ? "unparseable JSON"
                                                  : parse_error.c_str());
                continue;
            }
            if (!append(path.stem().string(), *value, error)) {
                return false;
            }
            durable_files.push_back(path);
            if (durable_files.size() >= kSyncEveryRecords &&
                !remove_durable_files()) {
                return false;
            }
        }
        return remove_durable_files();
    }

    bool open(std::string* error) {
        close_writer();
        index_.clear();
        recent_.clear();
        positions_.clear();
        watermarks_.clear();
        cached_segment_ = std::numeric_limits<std::uint32_t>::max();
        current_segment_ = 0;
        current_offset_ = 0;
        std::error_code error_code;
        const fs::path temporary = directory_.parent_path() /
            (directory_.filename().string() + ".retained");
        const fs::path backup = directory_.parent_path() /
            (directory_.filename().string() + ".previous");
        if (!fs::exists(directory_, error_code) &&
            fs::exists(backup, error_code)) {
            fs::rename(backup, directory_, error_code);
            if (error_code) {
                if (error) {
                    *error = "cannot restore audit directory " +
                             backup.string();
                }
                return false;
            }
        }
        fs::create_directories(directory_, error_code);
        if (error_code) {
            if (error) {
                *error = "cannot create audit directory " + directory_.string();
            }
            return false;
        }
        fs::remove_all(backup, error_code);
        fs::remove_all(temporary, error_code);
        const std::vector<std::uint32_t> found = segments();
        std::string index_error;
        if (!load_index(found, &index_error)) {
            if (!index_error.empty()) {
                if (error) {
                    *error = std::move(index_error);
                }
                return false;
            }
            index_.clear();
            recent_.clear();
            positions_.clear();
            watermarks_.clear();
            std::ofstream reset_index(directory_ / "index.bin", std::ios::binary | std::ios::trunc);
            reset_index.close();
            current_segment_ = found.empty() ? 0 : found.back();
            current_offset_ = 0;
            for (const std::uint32_t number : found) {
                if (!load_segment(number, error)) {
                    return false;
                }
            }
        } else if (!found.empty()) {
            // Only the active tail can be newer than its sidecar after a crash.
            // Historical payloads are never inflated during a normal reopen.
            current_segment_ = found.back();
            if (!load_segment(current_segment_, error)) {
                return false;
            }
        }
        if (!load_watermarks()) {
            if (error) {
                *error = "invalid audit watermarks";
            }
            return false;
        }
        if (!open_writer(error)) {
            return false;
        }
        return migrate_legacy_files(error);
    }

    fs::path directory_;
    std::size_t recent_limit_ = 0;
    AuditFingerprintIndex index_;
    std::map<std::string, std::deque<LogPosition>> positions_;
    std::map<std::string, AuditWatermark> watermarks_;
    mutable std::uint32_t cached_segment_ = std::numeric_limits<std::uint32_t>::max();
    mutable std::string cached_bytes_;
    std::deque<std::shared_ptr<AuditEvent>> recent_;
    std::uint32_t current_segment_ = 0;
    std::uint64_t current_offset_ = 0;
    std::uint32_t unsynced_records_ = 0;
    gzFile writer_ = nullptr;
};

AuditTrail::AuditTrail(fs::path directory, std::size_t recent_limit)
    : impl_(std::make_unique<Impl>(std::move(directory), recent_limit)) {}

AuditTrail::~AuditTrail() = default;

bool valid_audit_origin(std::string_view origin) {
    return !origin.empty() && origin.size() <= 128 &&
        std::ranges::all_of(origin, [](char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '-' || character == '_';
        });
}

bool AuditTrail::open(std::string* error) {
    const std::lock_guard lock(mutex_);
    return impl_->open(error);
}

bool AuditTrail::append(std::string id, AuditEventValue value,
                        std::string* error) {
    const std::lock_guard lock(mutex_);
    return impl_->append(std::move(id), std::move(value), error);
}

bool AuditTrail::append_local(const std::string& origin, std::string id,
                              AuditEventValue value, std::string* error) {
    const std::lock_guard lock(mutex_);
    if (origin == "legacy") {
        if (error) {
            *error = "legacy is reserved for migration";
        }
        return false;
    }
    return impl_->append(std::move(id), std::move(value), error, origin);
}

std::map<std::string, AuditWatermark> AuditTrail::watermarks() const {
    const std::lock_guard lock(mutex_);
    return impl_->watermarks_;
}

std::size_t AuditTrail::memory_bytes() const {
    const std::lock_guard lock(mutex_);
    return memory_usage().bytes;
}

void AuditTrail::release_unused_memory() {
#if defined(__APPLE__)
    ::malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
    ::malloc_trim(0);
#endif
}

std::optional<AuditLogBatch> AuditTrail::read_range(
    const std::string& origin, std::uint64_t after, std::string* error) {
    const std::lock_guard lock(mutex_);
    const auto watermark = impl_->watermarks_.find(origin);
    if (origin == "legacy" || watermark == impl_->watermarks_.end() ||
        after > watermark->second.high || !flush()) {
        if (error) {
            *error = "audit origin unavailable or range ahead of durable log";
        }
        return std::nullopt;
    }
    AuditLogBatch result{origin, after, after, watermark->second, {}};
    const auto found = impl_->positions_.find(origin);
    if (found == impl_->positions_.end()) {
        result.next = result.watermark.high;
        return result;
    }
    const auto& positions = found->second;
    auto iterator = std::upper_bound(positions.begin(), positions.end(), after,
        [](std::uint64_t sequence, const Impl::LogPosition& position) {
            return sequence < position.sequence;
        });
    constexpr std::size_t kPageBytes = 64 * 1024;
    if (iterator != positions.end()) {
        result.frames.reserve(kPageBytes);
    }
    for (; iterator != positions.end(); ++iterator) {
        const auto frame = impl_->frame_at(*iterator);
        if (!frame) {
            if (error) {
                *error = "audit index points beyond a readable segment";
            }
            return std::nullopt;
        }
        if (!result.frames.empty() && result.frames.size() + frame->size() > kPageBytes) {
            break;
        }
        result.frames.append(*frame);
        result.next = iterator->sequence;
    }
    if (iterator == positions.end()) {
        result.next = result.watermark.high;
    }
    return result;
}

bool AuditTrail::receive(const AuditLogBatch& batch, std::string* error) {
    const std::lock_guard lock(mutex_);
    if (!valid_audit_origin(batch.origin) || batch.origin == "legacy" ||
        batch.next < batch.after ||
        batch.next > batch.watermark.high ||
        batch.watermark.low > batch.watermark.high ||
        batch.watermark.summary_low > batch.watermark.high ||
        batch.frames.size() > kMaximumFrameBytes) {
        if (error) {
            *error = "invalid audit range or missing predecessor range";
        }
        return false;
    }
    auto& local = impl_->watermarks_[batch.origin];
    if (batch.after > local.high) {
        if (error) {
            *error = "audit range is missing its predecessor";
        }
        return false;
    }
    // Validate the complete page before mutating the trail. This bounded pass
    // prevents a bad trailing frame from acknowledging a partially valid page.
    std::size_t offset = 0;
    std::uint64_t sequence = batch.after;
    const std::uint64_t expired_through =
        std::max(batch.watermark.low, batch.watermark.summary_low);
    while (offset < batch.frames.size()) {
        const auto decoded = decode_audit_frame(std::string_view(batch.frames).substr(offset), error);
        if (!decoded || decoded->origin != batch.origin || decoded->sequence <= sequence ||
            decoded->sequence > batch.next ||
            (decoded->sequence - sequence > 1 &&
             decoded->sequence - 1 > expired_through)) {
            return false;
        }
        sequence = decoded->sequence;
        offset += decoded->consumed;
    }
    if (sequence < batch.next && batch.next > expired_through) {
        if (error) {
            *error = "audit range omits unexpired records";
        }
        return false;
    }
    offset = 0;
    while (offset < batch.frames.size()) {
        const auto decoded = decode_audit_frame(std::string_view(batch.frames).substr(offset));
        const bool summary =
            decoded->value.action == AuditAction::DataAccessSummary;
        const bool retained = summary
            ? decoded->sequence > local.summary_low
            : decoded->sequence > local.low;
        if (decoded->sequence > local.high && retained &&
            !impl_->append(decoded->id, decoded->value, error, batch.origin, decoded->sequence)) {
            return false;
        }
        offset += decoded->consumed;
    }
    local.high = std::max(local.high, batch.next);
    const auto previous_low = local.low;
    const auto previous_summary_low = local.summary_low;
    local.low = std::max(local.low, std::min(batch.watermark.low, batch.next));
    local.summary_low = std::max(
        local.summary_low,
        std::min(batch.watermark.summary_low, batch.next));
    if (!flush()) {
        return false;
    }
    return (previous_low == local.low &&
            previous_summary_low == local.summary_low) ||
        retain_since("", "", [](const std::string&) {}, error);
}

bool AuditTrail::contains(std::string_view id) const {
    const std::lock_guard lock(mutex_);
    return impl_->contains(id);
}

std::size_t AuditTrail::size() const {
    const std::lock_guard lock(mutex_);
    return impl_->index_.size();
}

AuditTrail::MemoryUsage AuditTrail::memory_usage() const {
    const std::lock_guard lock(mutex_);
    return impl_->memory_usage();
}

std::vector<std::shared_ptr<AuditEvent>> AuditTrail::events() const {
    const std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<AuditEvent>> result;
    result.reserve(size());
    for_each([&result](const std::shared_ptr<AuditEvent>& event) {
        result.push_back(event);
    });
    std::stable_sort(result.begin(), result.end(),
                     [](const auto& left, const auto& right) {
                         return left->time() < right->time();
                     });
    return result;
}

std::vector<std::shared_ptr<AuditEvent>> AuditTrail::recent_events() const {
    const std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<AuditEvent>> result{
        impl_->recent_.begin(), impl_->recent_.end()};
    std::stable_sort(result.begin(), result.end(),
                     [](const auto& left, const auto& right) {
                         return left->time() < right->time();
                     });
    return result;
}

void AuditTrail::for_each(const EventVisitor& visitor) const {
    const std::lock_guard lock(mutex_);
    impl_->for_each(visitor);
}

bool AuditTrail::retain_since(std::string_view cutoff,
                              std::string_view summary_cutoff,
                              const RemovedVisitor& removed,
                              std::string* error) {
    const std::lock_guard lock(mutex_);
    const fs::path original = impl_->directory_;
    const fs::path temporary = original.parent_path() /
        (original.filename().string() + ".retained");
    const fs::path backup = original.parent_path() /
        (original.filename().string() + ".previous");
    std::error_code error_code;
    fs::remove_all(temporary, error_code);
    fs::remove_all(backup, error_code);

    auto retained_watermarks = impl_->watermarks_;
    bool changed = false;
    for (const auto& [origin, positions] : impl_->positions_) {
        auto& watermark = retained_watermarks[origin];
        bool ordinary_prefix_open = true;
        bool summary_prefix_open = true;
        for (const auto& position : positions) {
            const auto frame = impl_->frame_at(position);
            const auto decoded = frame ? decode_audit_frame(*frame) : std::nullopt;
            if (!decoded) {
                return false;
            }
            const bool summary =
                decoded->value.action == AuditAction::DataAccessSummary;
            std::uint64_t& low = summary
                ? watermark.summary_low
                : watermark.low;
            bool& prefix_open = summary
                ? summary_prefix_open
                : ordinary_prefix_open;
            const std::string_view class_cutoff = summary
                ? summary_cutoff
                : cutoff;
            if (position.sequence <= low) {
                changed = true;
                continue;
            }
            if (!prefix_open || class_cutoff.empty() ||
                decoded->value.time >= class_cutoff) {
                prefix_open = false;
                continue;
            }
            low = position.sequence;
            changed = true;
        }
    }
    if (!changed) {
        return true;
    }

    {
        AuditTrail replacement(temporary, impl_->recent_limit_);
        if (!replacement.open(error)) {
            return false;
        }
        // Preserve physical append order, including the bounded recent window,
        // while retaining each origin's original sequence (only O(origins) cursors).
        const bool copied = impl_->visit_positions([&](const auto& origin, const auto& position) {
            const auto frame = impl_->frame_at(position);
            const auto decoded = frame ? decode_audit_frame(*frame) : std::nullopt;
            if (!decoded) {
                return false;
            }
            const bool summary =
                decoded->value.action == AuditAction::DataAccessSummary;
            const std::uint64_t low = summary
                ? retained_watermarks[origin].summary_low
                : retained_watermarks[origin].low;
            const bool keep = position.sequence > low;
            return !keep || replacement.impl_->append(decoded->id, decoded->value,
                                                       error, origin, position.sequence);
        });
        if (!copied) {
            return false;
        }
        replacement.impl_->watermarks_ = retained_watermarks;
        if (!replacement.impl_->sync_writer()) {
            if (error) {
                *error = "cannot sync retained audit segments";
            }
            return false;
        }
    }

    impl_->close_writer();
    fs::rename(original, backup, error_code);
    if (error_code) {
        if (error) {
            *error = "cannot stage old audit segments: " + error_code.message();
        }
        impl_->open(nullptr);
        return false;
    }
    fs::rename(temporary, original, error_code);
    if (error_code) {
        std::error_code restore_error;
        fs::rename(backup, original, restore_error);
        if (error) {
            *error = "cannot install retained audit segments: " +
                     error_code.message();
        }
        impl_->open(nullptr);
        return false;
    }
    // Report discarded ids by streaming the old store; no per-event removal
    // vector or replicated tombstone remains resident.
    Impl previous(backup, 0);
    const bool reported = impl_->visit_positions([&](const auto& origin, const auto& position) {
        const auto frame = previous.frame_at(position);
        const auto decoded = frame ? decode_audit_frame(*frame) : std::nullopt;
        if (!decoded) {
            return false;
        }
        const bool summary =
            decoded->value.action == AuditAction::DataAccessSummary;
        const std::uint64_t low = summary
            ? retained_watermarks[origin].summary_low
            : retained_watermarks[origin].low;
        if (position.sequence <= low) {
            removed(decoded->id);
        }
        return true;
    });
    if (!reported) {
        impl_->open(error);
        return false;
    }
    fs::remove_all(backup, error_code);
    if (!impl_->open(error)) {
        return false;
    }
    return true;
}

bool AuditTrail::flush() {
    const std::lock_guard lock(mutex_);
    return impl_->sync_writer();
}

// ---- the acting identity (#847) ---------------------------------------------

namespace {
thread_local std::optional<std::string> t_audit_actor;
}  // namespace

AuditActorScope::AuditActorScope(std::string actor)
    : previous_(std::move(t_audit_actor)) {
    t_audit_actor = actor.empty() ? std::string("local") : std::move(actor);
}

AuditActorScope::AuditActorScope(std::optional<std::string> actor)
    : previous_(std::move(t_audit_actor)) {
    if (actor && actor->empty()) actor = std::string("local");
    t_audit_actor = std::move(actor);
}

AuditActorScope::~AuditActorScope() { t_audit_actor = std::move(previous_); }

std::optional<std::string> current_audit_actor() { return t_audit_actor; }

// ---- the surface (#1328) ------------------------------------------------------

namespace {
thread_local std::optional<std::string> t_audit_surface;
}  // namespace

bool is_audit_surface(std::string_view surface) {
    for (const std::string_view known : kAuditSurfaces) {
        if (known == surface) return true;
    }
    return false;
}

AuditSurfaceScope::AuditSurfaceScope(std::optional<std::string> surface)
    : previous_(t_audit_surface) {
    // An unknown spelling never reaches the trail; the outer surface stays.
    if (surface && is_audit_surface(*surface)) t_audit_surface = std::move(surface);
    else if (!surface) t_audit_surface = std::nullopt;
}

AuditSurfaceScope::~AuditSurfaceScope() { t_audit_surface = std::move(previous_); }

std::optional<std::string> current_audit_surface() { return t_audit_surface; }

AuditOrigin capture_audit_origin() { return {t_audit_actor, t_audit_surface}; }

}  // namespace zetta::governance
