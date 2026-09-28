#include "zetta/governance/audit_event.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>
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
                              "pubsub_topic_updated", "pubsub_event_updated", "pubsub_event_deleted"] },
        "actor":   { "type": "string" },
        "target":  { "type": "string" },
        "allowed": { "type": "boolean" },
        "detail":  { "type": "string" },
        "time":    { "type": "string" },
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
    return std::nullopt;
}

nlohmann::json to_json(const AuditEventValue& value) {
    nlohmann::json result{{"action", to_string(value.action)},
                          {"actor", value.actor},
                          {"target", value.target},
                          {"allowed", value.allowed},
                          {"detail", value.detail},
                          {"time", value.time}};
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

std::uint32_t read_u32(std::string_view input, std::size_t offset) {
    std::uint32_t value = 0;
    for (int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(input[offset + byte]))
                 << (byte * 8);
    }
    return value;
}

std::uint64_t audit_id_hash(std::string_view id) {
    // Audit ids are random UUIDs. FNV-1a plus the CRC stored in Location keeps
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

std::string event_frame(const std::string& id, const AuditEventValue& value) {
    nlohmann::json record = to_json(value);
    record["id"] = id;
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
    const nlohmann::json record =
        nlohmann::json::parse(body, nullptr, false);
    if (!record.is_object() || !record.contains("id") ||
        !record.at("id").is_string()) {
        if (error) {
            *error = "frame is not an audit record";
        }
        return std::nullopt;
    }
    const std::string id = record.at("id").get<std::string>();
    const std::optional<AuditEventValue> value =
        parse_audit_event_value(record, error);
    if (!value) {
        return std::nullopt;
    }
    return DecodedAuditFrame{id, *value, 8 + length};
}

std::optional<std::string> inflate_segment(const fs::path& path,
                                           std::string* error = nullptr) {
    gzFile input = ::gzopen(path.c_str(), "rb");
    if (!input) {
        if (error) {
            *error = "cannot open " + path.string();
        }
        return std::nullopt;
    }
    std::string bytes;
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        const int read = ::gzread(input, buffer.data(),
                                  static_cast<unsigned>(buffer.size()));
        if (read > 0) {
            bytes.append(buffer.data(), static_cast<std::size_t>(read));
            continue;
        }
        if (read < 0) {
            int code = Z_OK;
            const char* message = ::gzerror(input, &code);
            if (error) {
                *error = message ? message : "gzip read failed";
            }
            ::gzclose(input);
            return std::nullopt;
        }
        break;
    }
    ::gzclose(input);
    return bytes;
}

}  // namespace

class AuditTrail::Impl {
public:
    struct Location {
        std::uint64_t offset = 0;
        std::uint32_t segment = 0;
        std::uint32_t id_checksum = 0;
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
        const auto range = index_.equal_range(audit_id_hash(id));
        const std::uint32_t checksum = audit_id_checksum(id);
        for (auto iterator = range.first; iterator != range.second; ++iterator) {
            if (iterator->second.id_checksum == checksum) {
                return true;
            }
        }
        return false;
    }

    void remember(const std::string& id, const AuditEventValue& value,
                  Location location) {
        index_.emplace(audit_id_hash(id), location);
        auto event = std::make_shared<AuditEvent>(id, value, directory_);
        recent_.push_back(std::move(event));
        while (recent_.size() > recent_limit_) {
            recent_.pop_front();
        }
    }

    bool load_segment(std::uint32_t number, std::string* error) {
        const std::optional<std::string> bytes =
            inflate_segment(segment_path(number), error);
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
                remember(decoded->id, decoded->value,
                         Location{offset, number,
                                  audit_id_checksum(decoded->id)});
            }
            offset += decoded->consumed;
        }
        if (number == current_segment_) {
            current_offset_ = offset;
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
        return true;
    }

    bool append(std::string id, AuditEventValue value, std::string* error) {
        if (contains(id)) {
            return true;
        }
        const std::string frame = event_frame(id, value);
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
        const Location location{current_offset_, current_segment_,
                                audit_id_checksum(id)};
        current_offset_ += frame.size();
        ++unsynced_records_;
        remember(id, value, location);
        if (::gzflush(writer_, Z_SYNC_FLUSH) != Z_OK) {
            if (error) {
                *error = "cannot flush audit segment " +
                         segment_path(current_segment_).string();
            }
            return false;
        }
        if (unsynced_records_ >= kSyncEveryRecords &&
            !sync_file(segment_path(current_segment_))) {
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
        if (!found.empty()) {
            current_segment_ = found.back();
            for (const std::uint32_t number : found) {
                if (!load_segment(number, error)) {
                    return false;
                }
            }
        }
        if (!open_writer(error)) {
            return false;
        }
        return migrate_legacy_files(error);
    }

    fs::path directory_;
    std::size_t recent_limit_ = 0;
    std::unordered_multimap<std::uint64_t, Location> index_;
    std::deque<std::shared_ptr<AuditEvent>> recent_;
    std::uint32_t current_segment_ = 0;
    std::uint64_t current_offset_ = 0;
    std::uint32_t unsynced_records_ = 0;
    gzFile writer_ = nullptr;
};

AuditTrail::AuditTrail(fs::path directory, std::size_t recent_limit)
    : impl_(std::make_unique<Impl>(std::move(directory), recent_limit)) {}

AuditTrail::~AuditTrail() = default;

bool AuditTrail::open(std::string* error) {
    return impl_->open(error);
}

bool AuditTrail::append(std::string id, AuditEventValue value,
                        std::string* error) {
    return impl_->append(std::move(id), std::move(value), error);
}

bool AuditTrail::contains(std::string_view id) const {
    return impl_->contains(id);
}

std::size_t AuditTrail::size() const {
    return impl_->index_.size();
}

std::vector<std::shared_ptr<AuditEvent>> AuditTrail::events() const {
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
    std::vector<std::shared_ptr<AuditEvent>> result{
        impl_->recent_.begin(), impl_->recent_.end()};
    std::stable_sort(result.begin(), result.end(),
                     [](const auto& left, const auto& right) {
                         return left->time() < right->time();
                     });
    return result;
}

void AuditTrail::for_each(const EventVisitor& visitor) const {
    impl_->for_each(visitor);
}

bool AuditTrail::retain_since(std::string_view cutoff,
                              const RemovedVisitor& removed,
                              std::string* error) {
    const fs::path original = impl_->directory_;
    const fs::path temporary = original.parent_path() /
        (original.filename().string() + ".retained");
    const fs::path backup = original.parent_path() /
        (original.filename().string() + ".previous");
    std::error_code error_code;
    fs::remove_all(temporary, error_code);
    fs::remove_all(backup, error_code);

    std::vector<std::string> removed_ids;
    for_each([&](const std::shared_ptr<AuditEvent>& event) {
        if (event->action() != AuditAction::DataAccessSummary &&
            event->time() < cutoff) {
            removed_ids.push_back(event->id());
        }
    });
    if (removed_ids.empty()) {
        return true;
    }

    {
        AuditTrail replacement(temporary, impl_->recent_limit_);
        if (!replacement.open(error)) {
            return false;
        }
        bool write_ok = true;
        for_each([&](const std::shared_ptr<AuditEvent>& event) {
            if (!write_ok) {
                return;
            }
            const bool keep = event->action() == AuditAction::DataAccessSummary ||
                              event->time() >= cutoff;
            if (keep) {
                write_ok = replacement.append(event->id(), event->value(), error);
            }
        });
        if (!replacement.impl_->sync_writer()) {
            if (error) {
                *error = "cannot sync retained audit segments";
            }
            return false;
        }
        if (!write_ok) {
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
    fs::remove_all(backup, error_code);
    if (!impl_->open(error)) {
        return false;
    }
    for (const std::string& id : removed_ids) {
        removed(id);
    }
    return true;
}

void AuditTrail::flush() {
    impl_->sync_writer();
}

}  // namespace zetta::governance
