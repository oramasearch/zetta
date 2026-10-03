#include "zetta/data/record_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"

#include "../catalog.hpp"

// The keyed record store (#1231). See include/zetta/data/record_store.hpp for the
// design and docs/data/record-store.md for the contract.
namespace zetta::data {
namespace {

using zetta::cluster::ReplicationWrite;

static_assert(catalog::kRecordPrefix == zetta::cluster::kRevisionRegisterPrefix,
              "record keys are the replicated document's revision registers");

constexpr std::size_t kMaxKeyBytes = 512;
constexpr std::size_t kMaxIndexValueBytes = 512;
constexpr std::size_t kMaxOrderBytes = 256;
constexpr std::size_t kMaxValueBytes = 1024 * 1024;  // base64 text
constexpr std::size_t kMaxIndexes = 8;
constexpr std::size_t kMaxListLimit = 1000;
constexpr auto kForwardTimeout = std::chrono::seconds(5);
constexpr auto kReplicateTimeout = std::chrono::seconds(2);

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::uint64_t now_micros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// ---- key scheme --------------------------------------------------------------

std::string to_hex(std::string_view text) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(text.size() * 2);
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

std::optional<std::string> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t position = 0; position < hex.size(); position += 2) {
        const int high = digit(hex[position]);
        const int low = digit(hex[position + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

std::string collection_key(std::string_view collection) {
    return std::string(catalog::kRecordCollectionPrefix) + std::string(collection);
}

std::string record_prefix(std::string_view collection) {
    return std::string(catalog::kRecordPrefix) + std::string(collection) + ":";
}

std::string record_key(std::string_view collection, std::string_view key) {
    return record_prefix(collection) + std::string(key);
}

// record_index:<collection>:<index>:<hex value>:<hex order>.<hex key>.<revision>
// Hex keeps every byte ordered as the original string orders it, and '.' sorts
// below every hex digit, so a shorter order or key sorts first, exactly as in a
// byte comparison. One entry per (record, revision): a newer revision writes a new
// entry and removes the old one, so an entry is never re-set after its removal.
std::string index_prefix(std::string_view collection, std::string_view index,
                         std::string_view value) {
    return std::string(catalog::kRecordIndexPrefix) + std::string(collection) + ":" +
           std::string(index) + ":" + to_hex(value) + ":";
}

std::string padded_revision(std::uint64_t revision) {
    std::string digits = std::to_string(revision);
    return std::string(20 - std::min<std::size_t>(20, digits.size()), '0') + digits;
}

std::string index_position(std::string_view order, std::string_view key) {
    return to_hex(order) + "." + to_hex(key);
}

std::string index_entry_key(std::string_view collection, std::string_view index,
                            std::string_view value, std::string_view order,
                            std::string_view key, std::uint64_t revision) {
    return index_prefix(collection, index, value) + index_position(order, key) + "." +
           padded_revision(revision);
}

struct IndexEntry {
    std::string collection;
    std::string key;
    std::uint64_t revision = 0;
};

std::optional<IndexEntry> parse_index_entry_key(std::string_view entry) {
    if (!entry.starts_with(catalog::kRecordIndexPrefix)) return std::nullopt;
    entry.remove_prefix(catalog::kRecordIndexPrefix.size());
    IndexEntry parsed;
    const std::size_t collection_end = entry.find(':');
    if (collection_end == std::string_view::npos) return std::nullopt;
    parsed.collection = std::string(entry.substr(0, collection_end));
    const std::size_t tail = entry.rfind(':');
    if (tail == std::string_view::npos || tail <= collection_end) return std::nullopt;
    const std::string_view position = entry.substr(tail + 1);
    const std::size_t first_dot = position.find('.');
    const std::size_t last_dot = position.rfind('.');
    if (first_dot == std::string_view::npos || last_dot == first_dot) return std::nullopt;
    const auto key = from_hex(position.substr(first_dot + 1, last_dot - first_dot - 1));
    if (!key) return std::nullopt;
    parsed.key = *key;
    const std::string_view digits = position.substr(last_dot + 1);
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed.revision);
    if (result.ec != std::errc{}) return std::nullopt;
    return parsed;
}

// ---- validation --------------------------------------------------------------

bool valid_name(std::string_view name, std::size_t max_size, bool allow_dot) {
    if (name.empty() || name.size() > max_size) return false;
    const auto lower_or_digit = [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    if (!lower_or_digit(name.front())) return false;
    return std::ranges::all_of(name, [&](char character) {
        return lower_or_digit(character) || character == '_' ||
               (allow_dot && (character == '.' || character == '-'));
    });
}

bool valid_collection_name(std::string_view name) { return valid_name(name, 96, true); }
bool valid_index_name(std::string_view name) { return valid_name(name, 64, false); }

bool valid_key(std::string_view key) {
    if (key.empty() || key.size() > kMaxKeyBytes) return false;
    return std::ranges::none_of(key, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7f;
    });
}

bool valid_base64(std::string_view text) {
    if (text.size() % 4 != 0) return false;
    std::size_t padding = 0;
    for (std::size_t position = 0; position < text.size(); ++position) {
        const char character = text[position];
        if (character == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) return false;  // data after padding
        const bool alphabet = (character >= 'A' && character <= 'Z') ||
                              (character >= 'a' && character <= 'z') ||
                              (character >= '0' && character <= '9') ||
                              character == '+' || character == '/';
        if (!alphabet) return false;
    }
    return padding <= 2;
}

// ---- values ------------------------------------------------------------------

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

// The hand-written, non-throwing parse of a record value read from the
// replicated document, mirroring stored_record_schema().
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

bool visible(const StoredRecord& record, std::int64_t now) {
    return !record.deleted && !(record.expires_ms && *record.expires_ms <= now);
}

// The writes that move a key from `existing` to `next`: drop the old index
// entries, write the record, add the new entries.
std::vector<ReplicationWrite> record_writes(const std::optional<StoredRecord>& existing,
                                            const StoredRecord& next) {
    std::vector<ReplicationWrite> writes;
    if (existing && !existing->deleted) {
        for (const auto& [index, value] : existing->index) {
            writes.push_back({index_entry_key(existing->collection, index, value,
                                              existing->order, existing->key,
                                              existing->revision),
                              std::nullopt});
        }
    }
    writes.push_back({record_key(next.collection, next.key), stored_value(next)});
    if (!next.deleted) {
        for (const auto& [index, value] : next.index) {
            writes.push_back({index_entry_key(next.collection, index, value, next.order,
                                              next.key, next.revision),
                              nlohmann::json{{"rev", next.revision}}});
        }
    }
    return writes;
}

// ---- schema boundary ---------------------------------------------------------

class CollectingErrors : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json&,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

nlohmann::json_schema::json_validator& signal_validator() {
    static nlohmann::json_schema::json_validator validator(record_signal_schema());
    return validator;
}

std::optional<nlohmann::json> decode_signal_payload(std::string_view payload,
                                                    std::string* reason) {
    nlohmann::json document =
        nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        if (reason) *reason = "record signal is not valid JSON";
        return std::nullopt;
    }
    CollectingErrors errors;
    signal_validator().validate(document, errors);
    if (errors.failed) {
        if (reason) *reason = errors.reason;
        return std::nullopt;
    }
    return document;
}

nlohmann::json apply_payload(const StoredRecord& record) {
    return {{"op", "apply"},
            {"collection", record.collection},
            {"key", record.key},
            {"record", stored_value(record)}};
}

nlohmann::json definition_payload(const RecordCollection& collection) {
    return {{"op", "apply"}, {"definition", to_json(collection)}};
}

nlohmann::json write_payload(const RecordWrite& write) {
    nlohmann::json payload{{"op", write.remove ? "delete" : "put"},
                           {"collection", write.collection},
                           {"key", write.key}};
    if (!write.remove) {
        payload["value"] = write.value;
        payload["index"] = write.index;
        payload["order"] = write.order;
    }
    switch (write.condition) {
        case RecordCondition::None: payload["condition"] = "none"; break;
        case RecordCondition::IfAbsent: payload["condition"] = "if_absent"; break;
        case RecordCondition::IfRevision:
            payload["condition"] = "if_revision";
            payload["revision"] = write.revision;
            break;
    }
    return payload;
}

std::optional<RecordWrite> write_from_payload(const nlohmann::json& payload) {
    if (!payload.contains("collection") || !payload.contains("key")) return std::nullopt;
    RecordWrite write;
    write.collection = payload.at("collection").get<std::string>();
    write.key = payload.at("key").get<std::string>();
    write.remove = payload.at("op").get<std::string>() == "delete";
    write.value = payload.value("value", std::string{});
    write.order = payload.value("order", std::string{});
    if (payload.contains("index")) {
        for (const auto& [name, value] : payload.at("index").items()) {
            write.index[name] = value.get<std::string>();
        }
    }
    const std::string condition = payload.value("condition", std::string("none"));
    if (condition == "if_absent") write.condition = RecordCondition::IfAbsent;
    if (condition == "if_revision") {
        write.condition = RecordCondition::IfRevision;
        write.revision = payload.value("revision", std::uint64_t{0});
    }
    return write;
}

std::optional<RecordStatus> status_from_string(std::string_view text) {
    for (const RecordStatus status :
         {RecordStatus::Ok, RecordStatus::InvalidInput, RecordStatus::NotFound,
          RecordStatus::Conflict, RecordStatus::Unavailable}) {
        if (to_string(status) == text) return status;
    }
    return std::nullopt;
}

nlohmann::json outcome_payload(const RecordOutcome& outcome) {
    nlohmann::json payload{{"status", to_string(outcome.status)},
                           {"message", outcome.message},
                           {"path", outcome.path},
                           {"owner", outcome.owner}};
    if (outcome.record) payload["record"] = stored_value(*outcome.record);
    return payload;
}

// A peer's answer is untrusted: every field is type-checked before use.
std::optional<RecordOutcome> outcome_from_payload(std::string_view payload,
                                                  const RecordWrite& write) {
    const nlohmann::json document =
        nlohmann::json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (!document.is_object()) return std::nullopt;
    const auto status = document.find("status");
    if (status == document.end() || !status->is_string()) return std::nullopt;
    const auto parsed = status_from_string(status->get<std::string>());
    if (!parsed) return std::nullopt;
    RecordOutcome outcome;
    outcome.status = *parsed;
    for (const auto& [field, target] :
         {std::pair{"message", &outcome.message}, std::pair{"path", &outcome.path},
          std::pair{"owner", &outcome.owner}}) {
        const auto found = document.find(field);
        if (found != document.end() && found->is_string()) *target = found->get<std::string>();
    }
    if (const auto record = document.find("record"); record != document.end()) {
        outcome.record = parse_stored(write.collection, write.key, *record);
        if (!outcome.record) return std::nullopt;
    }
    if (outcome.ok() && !outcome.record) return std::nullopt;
    return outcome;
}

RecordOutcome failure(RecordStatus status, std::string message, std::string path = {}) {
    RecordOutcome outcome;
    outcome.status = status;
    outcome.message = std::move(message);
    outcome.path = std::move(path);
    return outcome;
}

std::uint64_t rendezvous_weight(std::string_view node, std::string_view collection,
                                std::string_view key) {
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a 64
    const auto mix = [&](std::string_view text) {
        for (const char character : text) {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ull;
        }
        hash ^= 0xff;
        hash *= 1099511628211ull;
    };
    mix(node);
    mix(collection);
    mix(key);
    return hash;
}

}  // namespace

// ---- the journal ---------------------------------------------------------------

// The write-ahead journal: one line per committed write, fsynced before the write
// is acknowledged. The mutex is the lowest-level handoff inside this file adapter
// (API pool workers, the network thread and the owner loop all append or trim).
class RecordStore::Journal {
public:
    explicit Journal(std::filesystem::path path) : path_(std::move(path)) {}
    ~Journal() { close_locked(); }

    bool append(const std::string& line) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!open_locked()) return false;
        const std::string framed = line + "\n";
        std::size_t written = 0;
        while (written < framed.size()) {
            const ssize_t result =
                ::write(descriptor_, framed.data() + written, framed.size() - written);
            if (result <= 0) return false;
            written += static_cast<std::size_t>(result);
        }
        return ::fsync(descriptor_) == 0;
    }

    std::uint64_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::error_code error;
        const auto bytes = std::filesystem::file_size(path_, error);
        return error ? 0 : bytes;
    }

    std::vector<std::string> lines() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        std::ifstream input(path_);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty()) result.push_back(std::move(line));
        }
        return result;
    }

    // Drop the first `mark` bytes: everything appended before the document save
    // that preceded this call. Later appends are kept.
    void truncate(std::uint64_t mark) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (mark == 0) return;
        std::string tail;
        {
            std::ifstream input(path_, std::ios::binary);
            if (!input) return;
            input.seekg(static_cast<std::streamoff>(mark));
            std::ostringstream rest;
            rest << input.rdbuf();
            tail = rest.str();
        }
        close_locked();
        const std::filesystem::path temporary = path_.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(tail.data(), static_cast<std::streamsize>(tail.size()));
        }
        const int descriptor = ::open(temporary.c_str(), O_RDWR);
        if (descriptor >= 0) {
            ::fsync(descriptor);
            ::close(descriptor);
        }
        std::error_code error;
        std::filesystem::rename(temporary, path_, error);
    }

private:
    bool open_locked() {
        if (descriptor_ >= 0) return true;
        descriptor_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        return descriptor_ >= 0;
    }
    void close_locked() {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = -1;
    }

    std::filesystem::path path_;
    int descriptor_ = -1;
    mutable std::mutex mutex_;
};

// ---- public helpers ------------------------------------------------------------

std::string_view to_string(RecordStatus status) {
    switch (status) {
        case RecordStatus::Ok: return "ok";
        case RecordStatus::InvalidInput: return "invalid_input";
        case RecordStatus::NotFound: return "not_found";
        case RecordStatus::Conflict: return "conflict";
        case RecordStatus::Unavailable: return "unavailable";
    }
    return "ok";
}

const nlohmann::json& record_collection_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.RecordCollection",
  "description": "A keyed record collection (#1231): the replicated `record_collection:<name>` value. `indexes` names the secondary indexes a record may carry a value for; `retention_ms`, when present, expires a record that long after its last write; `created` is milliseconds since the Unix epoch.",
  "type": "object",
  "required": ["name", "indexes", "created"],
  "additionalProperties": false,
  "properties": {
    "name": {"type": "string", "pattern": "^[a-z0-9][a-z0-9_.-]{0,95}$"},
    "indexes": {
      "type": "array", "maxItems": 8, "uniqueItems": true,
      "items": {"type": "string", "pattern": "^[a-z0-9][a-z0-9_]{0,63}$"}
    },
    "retention_ms": {"type": "integer", "minimum": 1},
    "created": {"type": "integer"}
  }
}
)JSONSCHEMA");
    return schema;
}

const nlohmann::json& stored_record_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.StoredRecord",
  "description": "One keyed record (#1231): the replicated `record:<collection>:<key>` value, a revision register — concurrent values resolve to the highest `rev`. `value` is the caller's opaque bytes as base64; `index` maps a declared index to the record's value in it; `order` orders records within an index value; `updated` and `expires` are milliseconds since the Unix epoch. A deletion keeps `rev`, `updated` and `deleted: true` until the owner purges it.",
  "type": "object",
  "required": ["rev", "updated"],
  "additionalProperties": false,
  "properties": {
    "rev": {"type": "integer", "minimum": 1},
    "updated": {"type": "integer"},
    "deleted": {"type": "boolean"},
    "value": {"type": "string"},
    "index": {"type": "object", "additionalProperties": {"type": "string"}},
    "order": {"type": "string"},
    "expires": {"type": "integer"}
  }
}
)JSONSCHEMA");
    return schema;
}

const nlohmann::json& record_signal_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.RecordSignal",
  "description": "The payload of a record-store signal (#1231) and of one journal line. `put`/`delete` forward a write to the key's owner (record.put); `apply` installs a committed record or a collection definition on a replica (record.apply) and is what the journal replays.",
  "type": "object",
  "required": ["op"],
  "additionalProperties": false,
  "properties": {
    "op": {"enum": ["put", "delete", "apply"]},
    "collection": {"type": "string"},
    "key": {"type": "string"},
    "value": {"type": "string"},
    "index": {"type": "object", "additionalProperties": {"type": "string"}},
    "order": {"type": "string"},
    "condition": {"enum": ["none", "if_absent", "if_revision"]},
    "revision": {"type": "integer", "minimum": 0},
    "record": {"type": "object"},
    "definition": {"type": "object"}
  }
}
)JSONSCHEMA");
    return schema;
}

nlohmann::json to_json(const RecordCollection& collection) {
    nlohmann::json value{{"name", collection.name},
                         {"indexes", collection.indexes},
                         {"created", collection.created_ms}};
    if (collection.retention_ms) value["retention_ms"] = *collection.retention_ms;
    return value;
}

nlohmann::json to_json(const StoredRecord& record) {
    nlohmann::json value{{"collection", record.collection},
                         {"key", record.key},
                         {"value", record.value},
                         {"revision", record.revision},
                         {"index", record.index},
                         {"order", record.order},
                         {"updated_ms", record.updated_ms}};
    if (record.expires_ms) value["expires_ms"] = *record.expires_ms;
    return value;
}

std::optional<RecordCollection> record_collection_from_json(const nlohmann::json& value,
                                                            std::string* reason) {
    const auto fail = [&](std::string why) -> std::optional<RecordCollection> {
        if (reason) *reason = std::move(why);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("/: not an object");
    for (const auto& [field, _] : value.items()) {
        if (field != "name" && field != "indexes" && field != "retention_ms" &&
            field != "created") {
            return fail("/" + field + ": unknown field");
        }
    }
    const auto name = value.find("name");
    if (name == value.end() || !name->is_string() ||
        !valid_collection_name(name->get<std::string>())) {
        return fail("/name: want a collection name");
    }
    RecordCollection collection;
    collection.name = name->get<std::string>();
    const auto indexes = value.find("indexes");
    if (indexes == value.end() || !indexes->is_array() || indexes->size() > kMaxIndexes) {
        return fail("/indexes: want at most 8 index names");
    }
    for (const auto& index : *indexes) {
        if (!index.is_string() || !valid_index_name(index.get<std::string>())) {
            return fail("/indexes: want index names");
        }
        const std::string text = index.get<std::string>();
        if (std::ranges::find(collection.indexes, text) != collection.indexes.end()) {
            return fail("/indexes: '" + text + "' is declared twice");
        }
        collection.indexes.push_back(text);
    }
    if (const auto retention = value.find("retention_ms"); retention != value.end()) {
        if (!retention->is_number_integer() || retention->get<std::int64_t>() < 1) {
            return fail("/retention_ms: want a positive integer");
        }
        collection.retention_ms = retention->get<std::int64_t>();
    }
    const auto created = value.find("created");
    if (created == value.end() || !created->is_number_integer()) {
        return fail("/created: want an integer");
    }
    collection.created_ms = created->get<std::int64_t>();
    return collection;
}

// ---- the store -----------------------------------------------------------------

RecordStore::RecordStore(zetta::cluster::ReplicationManager& replication,
                         std::string node_id, std::filesystem::path journal_path)
    : replication_(replication), node_id_(std::move(node_id)) {
    if (!journal_path.empty()) journal_ = std::make_unique<Journal>(std::move(journal_path));
}

RecordStore::~RecordStore() { detach(); }

void RecordStore::attach(zetta::cluster::Cluster& cluster) {
    detach();
    delegate_.store(cluster.signal_handler());
    cluster_.store(&cluster);
    cluster.set_signal_handler(this);
}

void RecordStore::detach() {
    zetta::cluster::Cluster* cluster = cluster_.exchange(nullptr);
    if (!cluster) return;
    if (cluster->signal_handler() == this) cluster->set_signal_handler(delegate_.load());
    delegate_.store(nullptr);
}

std::vector<std::string> RecordStore::live_peers() const {
    std::vector<std::string> peers;
    if (zetta::cluster::Cluster* cluster = cluster_.load()) {
        for (const zetta::cluster::ClusterPeer& peer : cluster->peers()) {
            if (peer.node_id != node_id_) peers.push_back(peer.node_id);
        }
    }
    return peers;
}

std::string RecordStore::owner_of(const std::string& collection,
                                  const std::string& key) const {
    std::vector<std::string> candidates = live_peers();
    candidates.push_back(node_id_);
    // Ephemeral (burst) nodes may vanish at any moment, so they order writes only
    // when no permanent node is reachable (#809's durability rule).
    {
        const auto view = replication_.shared_snapshot();
        const auto ephemeral = [&](const std::string& node) {
            const auto found = view->find(catalog::node_key(node));
            return found != view->end() && found->is_object() &&
                   found->value("node_type", std::string()) == "ephemeral";
        };
        std::vector<std::string> permanent;
        std::ranges::copy_if(candidates, std::back_inserter(permanent),
                             [&](const std::string& node) { return !ephemeral(node); });
        if (!permanent.empty()) candidates = std::move(permanent);
    }
    std::string owner = node_id_;
    std::uint64_t best = 0;
    bool first = true;
    for (const std::string& node : candidates) {
        const std::uint64_t weight = rendezvous_weight(node, collection, key);
        if (first || weight > best || (weight == best && node > owner)) {
            owner = node;
            best = weight;
            first = false;
        }
    }
    return owner;
}

std::optional<RecordCollection> RecordStore::find_collection(const std::string& name) const {
    const auto view = replication_.shared_snapshot();
    const auto found = view->find(collection_key(name));
    if (found == view->end()) return std::nullopt;
    return record_collection_from_json(*found);
}

bool RecordStore::install_collection(const RecordCollection& collection) {
    return replication_.update(
        collection_key(collection.name),
        [&](const nlohmann::json* current) -> std::optional<std::vector<ReplicationWrite>> {
            if (current) return std::nullopt;  // the first definition stands
            return std::vector<ReplicationWrite>{
                {collection_key(collection.name), to_json(collection)}};
        });
}

RecordOutcome RecordStore::create_collection(const RecordCollection& requested) {
    RecordCollection collection = requested;
    if (collection.created_ms == 0) collection.created_ms = now_ms();
    std::string reason;
    if (!record_collection_from_json(to_json(collection), &reason)) {
        const std::size_t colon = reason.find(':');
        return failure(RecordStatus::InvalidInput, reason,
                       colon == std::string::npos ? "" : reason.substr(0, colon));
    }
    RecordOutcome outcome;
    outcome.owner = node_id_;
    bool conflict = false;
    std::optional<RecordCollection> existing;
    const bool created = replication_.update(
        collection_key(collection.name),
        [&](const nlohmann::json* current) -> std::optional<std::vector<ReplicationWrite>> {
            if (current) {
                existing = record_collection_from_json(*current);
                conflict = !existing || existing->indexes != collection.indexes ||
                           existing->retention_ms != collection.retention_ms;
                return std::nullopt;
            }
            return std::vector<ReplicationWrite>{
                {collection_key(collection.name), to_json(collection)}};
        });
    if (conflict) {
        outcome = failure(RecordStatus::Conflict,
                          "collection '" + collection.name +
                              "' already exists with a different definition",
                          "/collection");
        outcome.collection = existing;
        return outcome;
    }
    outcome.created = created;
    outcome.collection = created ? collection : existing;
    if (created && journal_ && !journal_->append(definition_payload(collection).dump())) {
        return failure(RecordStatus::Unavailable,
                       "the collection was created in memory but the journal could not "
                       "be synced to disk");
    }
    replicate(definition_payload(*outcome.collection), {}, outcome);
    return outcome;
}

RecordOutcome RecordStore::commit(const RecordWrite& write, std::int64_t now) {
    const std::optional<RecordCollection> definition = find_collection(write.collection);
    if (!definition) {
        return failure(RecordStatus::NotFound,
                       "no record collection '" + write.collection + "'", "/collection");
    }
    for (const auto& [index, value] : write.index) {
        if (std::ranges::find(definition->indexes, index) == definition->indexes.end()) {
            return failure(RecordStatus::InvalidInput,
                           "collection '" + write.collection + "' declares no index '" +
                               index + "'",
                           "/index/" + index);
        }
    }
    RecordOutcome outcome;
    outcome.owner = node_id_;
    std::optional<StoredRecord> committed;
    replication_.update(
        record_key(write.collection, write.key),
        [&](const nlohmann::json* current) -> std::optional<std::vector<ReplicationWrite>> {
            std::optional<StoredRecord> existing;
            if (current) existing = parse_stored(write.collection, write.key, *current);
            const bool live = existing && visible(*existing, now);
            if (write.condition == RecordCondition::IfAbsent && live) {
                outcome = failure(RecordStatus::Conflict,
                                  "record '" + write.key + "' already exists at revision " +
                                      std::to_string(existing->revision),
                                  "/if_absent");
                outcome.record = existing;
                return std::nullopt;
            }
            if (write.condition == RecordCondition::IfRevision &&
                (!live || existing->revision != write.revision)) {
                outcome = failure(
                    RecordStatus::Conflict,
                    live ? "record '" + write.key + "' is at revision " +
                               std::to_string(existing->revision) + ", not " +
                               std::to_string(write.revision)
                         : "record '" + write.key + "' does not exist",
                    "/if_revision");
                if (live) outcome.record = existing;
                return std::nullopt;
            }
            if (write.remove && !live) {
                outcome = failure(RecordStatus::NotFound,
                                  "no record '" + write.key + "' in '" + write.collection + "'",
                                  "/key");
                return std::nullopt;
            }
            StoredRecord next;
            next.collection = write.collection;
            next.key = write.key;
            // Revisions keep rising across a deletion and a purge: never below the
            // previous one + 1, and never below the clock in microseconds, so a
            // re-created key never reuses a revision a caller still holds.
            next.revision = std::max(existing ? existing->revision + 1 : 1, now_micros());
            next.updated_ms = now;
            next.deleted = write.remove;
            if (!write.remove) {
                next.value = write.value;
                next.index = write.index;
                next.order = write.order;
                if (definition->retention_ms) next.expires_ms = now + *definition->retention_ms;
            }
            committed = next;
            return record_writes(existing, next);
        });
    if (!committed) {
        outcome.owner = node_id_;
        return outcome;
    }
    if (journal_ && !journal_->append(apply_payload(*committed).dump())) {
        return failure(RecordStatus::Unavailable,
                       "the write was applied in memory but the journal could not be synced "
                       "to disk");
    }
    outcome.record = committed;
    return outcome;
}

bool RecordStore::install(const StoredRecord& record) {
    return replication_.update(
        record_key(record.collection, record.key),
        [&](const nlohmann::json* current) -> std::optional<std::vector<ReplicationWrite>> {
            std::optional<StoredRecord> existing;
            if (current) existing = parse_stored(record.collection, record.key, *current);
            if (existing && existing->revision >= record.revision) return std::nullopt;
            return record_writes(existing, record);
        });
}

void RecordStore::replicate(const nlohmann::json& payload, const std::string& except,
                            RecordOutcome& outcome) {
    std::vector<std::string> peers = live_peers();
    std::erase(peers, except);
    outcome.replicas_expected = peers.size() + (except.empty() ? 0 : 1);
    zetta::cluster::Cluster* cluster = cluster_.load();
    if (!cluster || peers.empty()) return;
    const std::vector<zetta::cluster::SignalResponse> responses = cluster->request_each(
        peers, std::string(kApplyMethod), payload.dump(), kReplicateTimeout);
    outcome.replicas += static_cast<std::size_t>(std::ranges::count_if(
        responses, [](const zetta::cluster::SignalResponse& response) { return response.ok; }));
}

RecordOutcome RecordStore::write(const RecordWrite& write) {
    if (!valid_collection_name(write.collection)) {
        return failure(RecordStatus::InvalidInput, "want a collection name", "/collection");
    }
    if (!valid_key(write.key)) {
        return failure(RecordStatus::InvalidInput,
                       "want a key of 1 to 512 bytes with no control characters", "/key");
    }
    if (!write.remove) {
        if (write.value.size() > kMaxValueBytes || !valid_base64(write.value)) {
            return failure(RecordStatus::InvalidInput,
                           "want base64 of at most 1048576 characters", "/value");
        }
        if (write.order.size() > kMaxOrderBytes) {
            return failure(RecordStatus::InvalidInput, "order is longer than 256 bytes",
                           "/order");
        }
        for (const auto& [index, value] : write.index) {
            if (!valid_index_name(index) || value.size() > kMaxIndexValueBytes) {
                return failure(RecordStatus::InvalidInput,
                               "want index values of at most 512 bytes", "/index/" + index);
            }
        }
    }

    const std::string owner = owner_of(write.collection, write.key);
    if (owner == node_id_) {
        RecordOutcome outcome = commit(write, now_ms());
        if (outcome.ok()) replicate(apply_payload(*outcome.record), {}, outcome);
        return outcome;
    }
    zetta::cluster::Cluster* cluster = cluster_.load();
    if (!cluster) {
        return failure(RecordStatus::Unavailable, "no cluster to reach the owner " + owner);
    }
    const zetta::cluster::SignalResponse response = cluster->request(
        owner, std::string(kPutMethod), write_payload(write).dump(), kForwardTimeout);
    if (!response.ok) {
        return failure(RecordStatus::Unavailable,
                       "the key's owner " + owner + " did not answer: " + response.error +
                           "; retry");
    }
    std::optional<RecordOutcome> outcome = outcome_from_payload(response.payload, write);
    if (!outcome) {
        return failure(RecordStatus::Unavailable,
                       "the key's owner " + owner + " sent an unreadable answer; retry");
    }
    if (!outcome->ok()) return *outcome;
    outcome->owner = owner;
    install(*outcome->record);
    outcome->replicas = 1;  // the owner holds it
    replicate(apply_payload(*outcome->record), owner, *outcome);
    return *outcome;
}

RecordOutcome RecordStore::get(const std::string& collection, const std::string& key) const {
    if (!valid_collection_name(collection)) {
        return failure(RecordStatus::InvalidInput, "want a collection name", "/collection");
    }
    const auto view = replication_.shared_snapshot();
    if (!view->contains(collection_key(collection))) {
        return failure(RecordStatus::NotFound,
                       "no record collection '" + collection + "'", "/collection");
    }
    const auto found = view->find(record_key(collection, key));
    std::optional<StoredRecord> record;
    if (found != view->end()) record = parse_stored(collection, key, *found);
    if (!record || !visible(*record, now_ms())) {
        return failure(RecordStatus::NotFound,
                       "no record '" + key + "' in '" + collection + "'", "/key");
    }
    RecordOutcome outcome;
    outcome.record = std::move(record);
    return outcome;
}

RecordPage RecordStore::list(const RecordListRequest& request) const {
    RecordPage page;
    const auto fail = [&](RecordStatus status, std::string message, std::string path) {
        page.status = status;
        page.message = std::move(message);
        page.path = std::move(path);
        return page;
    };
    if (!valid_collection_name(request.collection)) {
        return fail(RecordStatus::InvalidInput, "want a collection name", "/collection");
    }
    const std::size_t limit = std::clamp<std::size_t>(request.limit, 1, kMaxListLimit);
    const std::int64_t now = now_ms();
    const auto view = replication_.shared_snapshot();
    const auto definition_entry = view->find(collection_key(request.collection));
    if (definition_entry == view->end()) {
        return fail(RecordStatus::NotFound,
                    "no record collection '" + request.collection + "'", "/collection");
    }
    const auto& entries = view->get_ref<const nlohmann::json::object_t&>();

    std::string prefix;
    std::string start;  // the first key a page may return (ascending), or the bound below it
    if (request.index.empty()) {
        prefix = record_prefix(request.collection);
        if (!request.after.empty()) {
            const auto after = from_hex(request.after);
            if (!after) return fail(RecordStatus::InvalidInput, "not a cursor", "/after");
            start = prefix + *after;
        }
    } else {
        const auto definition = record_collection_from_json(*definition_entry);
        if (!definition || std::ranges::find(definition->indexes, request.index) ==
                               definition->indexes.end()) {
            return fail(RecordStatus::InvalidInput,
                        "collection '" + request.collection + "' declares no index '" +
                            request.index + "'",
                        "/index");
        }
        prefix = index_prefix(request.collection, request.index, request.value);
        if (!request.after.empty()) {
            if (request.after.find_first_not_of("0123456789abcdef.") != std::string::npos) {
                return fail(RecordStatus::InvalidInput, "not a cursor", "/after");
            }
            start = prefix + request.after;
        }
    }

    // Resolve one entry to its record (index entries must match the record's
    // current revision; anything else is stale and skipped).
    const auto resolve = [&](const std::string& entry_key,
                             const nlohmann::json& entry_value)
        -> std::optional<std::pair<StoredRecord, std::string>> {
        if (request.index.empty()) {
            std::string key = entry_key.substr(prefix.size());
            auto record = parse_stored(request.collection, key, entry_value);
            if (!record || !visible(*record, now)) return std::nullopt;
            return std::pair{std::move(*record), to_hex(key)};
        }
        const auto parsed = parse_index_entry_key(entry_key);
        if (!parsed) return std::nullopt;
        const auto found = view->find(record_key(request.collection, parsed->key));
        if (found == view->end()) return std::nullopt;
        auto record = parse_stored(request.collection, parsed->key, *found);
        if (!record || record->revision != parsed->revision || !visible(*record, now)) {
            return std::nullopt;
        }
        const std::string tail = entry_key.substr(prefix.size());
        return std::pair{std::move(*record), tail.substr(0, tail.rfind('.'))};
    };

    std::string last_position;
    bool more = false;
    const auto take = [&](const std::string& entry_key, const nlohmann::json& entry_value) {
        auto resolved = resolve(entry_key, entry_value);
        if (!resolved) return true;
        if (resolved->second == last_position) return true;  // a stale duplicate
        if (page.records.size() == limit) {
            more = true;
            return false;
        }
        last_position = resolved->second;
        page.records.push_back(std::move(resolved->first));
        return true;
    };

    if (!request.descending) {
        // Strictly after the cursor: past every entry at the cursor's position.
        auto entry = start.empty() ? entries.lower_bound(prefix)
                                   : entries.upper_bound(start + (request.index.empty()
                                                                      ? std::string()
                                                                      : std::string(".\x7f")));
        for (; entry != entries.end() && std::string_view(entry->first).starts_with(prefix);
             ++entry) {
            if (!take(entry->first, entry->second)) break;
        }
    } else {
        // Strictly before the cursor, walking down from the end of the range.
        std::string prefix_end = prefix;  // the first key past the prefix range
        ++prefix_end.back();
        auto bound = start.empty() ? entries.lower_bound(prefix_end)
                                   : entries.lower_bound(start + (request.index.empty()
                                                                      ? std::string()
                                                                      : std::string(".")));
        while (bound != entries.begin()) {
            --bound;
            if (!std::string_view(bound->first).starts_with(prefix)) break;
            if (!take(bound->first, bound->second)) break;
        }
    }
    if (more) page.next_after = last_position;
    return page;
}

void RecordStore::sweep(std::int64_t now) {
    struct Candidate {
        std::string key;
        std::string collection;
        std::string record;
        std::uint64_t revision = 0;
    };
    std::vector<Candidate> records;
    std::vector<std::string> stale_entries;
    {
        const auto view = replication_.shared_snapshot();
        catalog::for_each_with_prefix(
            *view, catalog::kRecordPrefix,
            [&](const std::string& key, const nlohmann::json& value) {
                const std::string_view rest =
                    std::string_view(key).substr(catalog::kRecordPrefix.size());
                const std::size_t colon = rest.find(':');
                if (colon == std::string_view::npos) return;
                std::string collection(rest.substr(0, colon));
                std::string record(rest.substr(colon + 1));
                const auto parsed = parse_stored(collection, record, value);
                if (!parsed) return;
                const bool expired = parsed->expires_ms && *parsed->expires_ms <= now;
                const bool purgeable =
                    parsed->deleted && now - parsed->updated_ms >= kTombstoneGraceMs;
                if (!expired && !purgeable) return;
                records.push_back({key, std::move(collection), std::move(record),
                                   parsed->revision});
            });
        catalog::for_each_with_prefix(
            *view, catalog::kRecordIndexPrefix,
            [&](const std::string& key, const nlohmann::json&) {
                const auto entry = parse_index_entry_key(key);
                if (!entry) return;
                const auto found = view->find(record_key(entry->collection, entry->key));
                std::optional<StoredRecord> record;
                if (found != view->end()) {
                    record = parse_stored(entry->collection, entry->key, *found);
                }
                if (record && record->revision == entry->revision && !record->deleted) return;
                // Revisions are at least the commit time in microseconds, so the
                // entry's age is readable from its revision.
                const auto written_ms = static_cast<std::int64_t>(entry->revision / 1000);
                if (now - written_ms < kTombstoneGraceMs) return;
                if (owner_of(entry->collection, entry->key) != node_id_) return;
                stale_entries.push_back(key);
            });
    }  // release the view before writing (#1227)

    for (const Candidate& candidate : records) {
        if (owner_of(candidate.collection, candidate.record) != node_id_) continue;
        replication_.update(
            candidate.key,
            [&](const nlohmann::json* current) -> std::optional<std::vector<ReplicationWrite>> {
                if (!current) return std::nullopt;
                const auto record =
                    parse_stored(candidate.collection, candidate.record, *current);
                if (!record || record->revision != candidate.revision) return std::nullopt;
                std::vector<ReplicationWrite> writes{{candidate.key, std::nullopt}};
                if (!record->deleted) {
                    for (const auto& [index, value] : record->index) {
                        writes.push_back({index_entry_key(candidate.collection, index, value,
                                                          record->order, candidate.record,
                                                          record->revision),
                                          std::nullopt});
                    }
                }
                return writes;
            });
    }
    for (const std::string& key : stale_entries) replication_.remove(key);
}

std::size_t RecordStore::replay_journal() {
    if (!journal_) return 0;
    std::size_t applied = 0;
    for (const std::string& line : journal_->lines()) {
        std::string reason;
        const auto payload = decode_signal_payload(line, &reason);
        if (!payload || payload->value("op", std::string()) != "apply") {
            std::fprintf(stderr, "zetta: skipping an unreadable record journal line: %s\n",
                         reason.c_str());
            continue;
        }
        if (payload->contains("definition")) {
            if (auto collection = record_collection_from_json(payload->at("definition"))) {
                applied += install_collection(*collection) ? 1 : 0;
            }
            continue;
        }
        if (!payload->contains("collection") || !payload->contains("key") ||
            !payload->contains("record")) {
            continue;
        }
        if (auto record = parse_stored(payload->at("collection").get<std::string>(),
                                       payload->at("key").get<std::string>(),
                                       payload->at("record"))) {
            applied += install(*record) ? 1 : 0;
        }
    }
    return applied;
}

std::uint64_t RecordStore::journal_mark() const { return journal_ ? journal_->size() : 0; }

void RecordStore::journal_truncate(std::uint64_t mark) {
    if (journal_) journal_->truncate(mark);
}

zetta::cluster::SignalResponse RecordStore::on_request(
    const zetta::cluster::Signal& request) {
    if (request.method != kPutMethod && request.method != kApplyMethod) {
        if (auto* delegate = delegate_.load()) return delegate->on_request(request);
        return {.ok = false, .payload = {}, .error = "no handler"};
    }
    std::string reason;
    const auto payload = decode_signal_payload(request.payload, &reason);
    if (!payload) {
        return {.ok = false, .payload = {}, .error = std::string(request.method) + ": " + reason};
    }
    const std::string op = payload->at("op").get<std::string>();
    if (request.method == kApplyMethod) {
        if (op != "apply") return {.ok = false, .payload = {}, .error = "record.apply: want op apply"};
        if (payload->contains("definition")) {
            std::string why;
            const auto collection = record_collection_from_json(payload->at("definition"), &why);
            if (!collection) return {.ok = false, .payload = {}, .error = "record.apply: " + why};
            install_collection(*collection);
            return {.ok = true, .payload = {}, .error = {}};
        }
        if (!payload->contains("collection") || !payload->contains("key") ||
            !payload->contains("record")) {
            return {.ok = false, .payload = {}, .error = "record.apply: want a record"};
        }
        const std::string collection = payload->at("collection").get<std::string>();
        const std::string key = payload->at("key").get<std::string>();
        if (!valid_collection_name(collection) || !valid_key(key)) {
            return {.ok = false, .payload = {}, .error = "record.apply: bad collection or key"};
        }
        const auto record = parse_stored(collection, key, payload->at("record"));
        if (!record) return {.ok = false, .payload = {}, .error = "record.apply: bad record"};
        install(*record);
        return {.ok = true, .payload = {}, .error = {}};
    }
    if (op == "apply") return {.ok = false, .payload = {}, .error = "record.put: want put or delete"};
    const auto write = write_from_payload(*payload);
    if (!write) return {.ok = false, .payload = {}, .error = "record.put: want a collection and key"};
    RecordOutcome outcome;
    if (!valid_collection_name(write->collection) || !valid_key(write->key)) {
        outcome = failure(RecordStatus::InvalidInput, "bad collection or key");
    } else if (const std::string owner = owner_of(write->collection, write->key);
               owner != node_id_) {
        // Two nodes disagree about membership right now; the caller retries.
        outcome = failure(RecordStatus::Unavailable,
                          "ownership of this key is moving (this node sees " + owner +
                              " as its owner); retry");
    } else {
        outcome = commit(*write, now_ms());
    }
    outcome.owner = node_id_;
    return {.ok = true, .payload = outcome_payload(outcome).dump(), .error = {}};
}

void RecordStore::on_event(const zetta::cluster::Signal& event) {
    if (auto* delegate = delegate_.load()) delegate->on_event(event);
}

void RecordStore::on_task(const zetta::cluster::Signal& task) {
    if (auto* delegate = delegate_.load()) delegate->on_task(task);
}

}  // namespace zetta::data
