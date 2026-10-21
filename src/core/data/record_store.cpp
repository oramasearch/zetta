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
#include "record_log.hpp"

// The keyed record store (#1231). See include/zetta/data/record_store.hpp for the
// design and docs/data/record-store.md for the contract.
namespace zetta::data {
namespace {

using zetta::cluster::ReplicationWrite;


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

bool visible(const StoredRecord& record, std::int64_t now) {
    return !record.deleted && !(record.expires_ms && *record.expires_ms <= now);
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
  "description": "One keyed record (#1231), as a record-log frame holds it and a record.* signal carries it. `rev` is the record's revision: a copy is kept only when it is newer. `value` is the caller's opaque bytes as base64 (an engine-internal collection keeps its own text); `index` maps a declared index to the record's value in it; `order` orders records within an index value; `updated` and `expires` are milliseconds since the Unix epoch. A deletion keeps `rev`, `updated` and `deleted: true` until its grace period ends.",
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
  "description": "The payload of a record-store signal (#1231). `put`/`delete` forward a write to the key's owner (record.put); `apply` installs a committed record or a collection definition on a replica (record.apply); `digest`, `keys` and `fetch` are anti-entropy: a peer's bucket digests (record.digest), the versions in some buckets of one collection (record.keys), and some records by key (record.fetch).",
  "type": "object",
  "required": ["op"],
  "additionalProperties": false,
  "properties": {
    "op": {"enum": ["put", "delete", "apply", "digest", "keys", "fetch"]},
    "buckets": {"type": "array", "maxItems": 256, "items": {"type": "integer", "minimum": 0, "maximum": 255}},
    "keys": {"type": "array", "maxItems": 256, "items": {"type": "string"}},
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

namespace {

constexpr std::size_t kBucketsPerRequest = 32;
constexpr std::size_t kKeysPerFetch = 256;
constexpr auto kAntiEntropyTimeout = std::chrono::seconds(10);

// Engine-internal collections (the time series' buckets) start with '_', which
// no API collection name can.
bool valid_stored_collection(std::string_view name) {
    return valid_collection_name(name) ||
           (name.size() > 1 && name.front() == '_' && valid_name(name.substr(1), 160, true));
}

// Whether a peer's version of a key is worth pulling: newer than ours, and not
// already expired, and not a deletion past its grace (both would only be swept
// again, and a purged key must not come back).
bool worth_pulling(const RecordVersion& theirs, const std::optional<StoredRecord>& ours,
                   std::int64_t now) {
    if (ours && ours->revision >= theirs.revision) return false;
    if (theirs.expires_ms && *theirs.expires_ms <= now) return false;
    if (theirs.deleted && now - theirs.updated_ms >= RecordStore::kTombstoneGraceMs) return false;
    return true;
}

}  // namespace

RecordStore::RecordStore(zetta::cluster::ReplicationManager& replication,
                         std::string node_id, std::filesystem::path directory,
                         std::uint64_t segment_bytes)
    : replication_(replication),
      node_id_(std::move(node_id)),
      log_(std::make_unique<RecordLog>(std::move(directory), kTombstoneGraceMs,
                                       segment_bytes == 0 ? RecordLog::kSegmentBytes
                                                          : segment_bytes)) {}

RecordStore::~RecordStore() { detach(); }

bool RecordStore::open(std::string* reason) { return log_->open(reason); }

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
    const RecordLog::Written written = log_->update(
        write.collection, write.key,
        [&](const StoredRecord* existing) -> std::optional<StoredRecord> {
            const bool live = existing && visible(*existing, now);
            if (write.condition == RecordCondition::IfAbsent && live) {
                outcome = failure(RecordStatus::Conflict,
                                  "record '" + write.key + "' already exists at revision " +
                                      std::to_string(existing->revision),
                                  "/if_absent");
                outcome.record = *existing;
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
                if (live) outcome.record = *existing;
                return std::nullopt;
            }
            if (write.remove && !live) {
                outcome = failure(RecordStatus::NotFound,
                                  "no record '" + write.key + "' in '" + write.collection + "'",
                                  "/key");
                return std::nullopt;
            }
            StoredRecord next;
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
            return next;
        },
        /*with_value=*/false, /*sync=*/true);
    outcome.owner = node_id_;
    if (!written.record) {
        if (outcome.ok()) {
            return failure(RecordStatus::Unavailable,
                           "the record log could not be written on " + node_id_);
        }
        return outcome;
    }
    if (!written.durable) {
        return failure(RecordStatus::Unavailable,
                       "the write was applied but could not be synced to disk on " + node_id_);
    }
    outcome.record = written.record;
    return outcome;
}

bool RecordStore::install(const StoredRecord& record, std::int64_t now) {
    if (record.expires_ms && *record.expires_ms <= now) return false;
    return log_->update(record.collection, record.key,
                        [&](const StoredRecord* existing) -> std::optional<StoredRecord> {
                            if (existing && existing->revision >= record.revision) return std::nullopt;
                            return record;
                        },
                        /*with_value=*/false, /*sync=*/false)
        .record.has_value();
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
    install(*outcome->record, now_ms());
    outcome->replicas = 1;  // the owner holds it
    replicate(apply_payload(*outcome->record), owner, *outcome);
    return *outcome;
}

RecordOutcome RecordStore::get(const std::string& collection, const std::string& key) const {
    if (!valid_collection_name(collection)) {
        return failure(RecordStatus::InvalidInput, "want a collection name", "/collection");
    }
    if (!find_collection(collection)) {
        return failure(RecordStatus::NotFound,
                       "no record collection '" + collection + "'", "/collection");
    }
    std::optional<StoredRecord> record = log_->get(collection, key, /*with_value=*/true);
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
    const auto definition = find_collection(request.collection);
    if (!definition) {
        return fail(RecordStatus::NotFound,
                    "no record collection '" + request.collection + "'", "/collection");
    }
    // The cursor is the last record's position: hex(order) "." hex(key) in an
    // index listing, hex(key) in a key listing.
    std::string last_position;
    bool more = false;
    const auto take = [&](StoredRecord&& record, std::string position) {
        if (!visible(record, now)) return true;
        if (page.records.size() == limit) {
            more = true;
            return false;
        }
        last_position = std::move(position);
        page.records.push_back(std::move(record));
        return true;
    };
    if (request.index.empty()) {
        std::optional<std::string> after;
        if (!request.after.empty()) {
            after = from_hex(request.after);
            if (!after) return fail(RecordStatus::InvalidInput, "not a cursor", "/after");
        }
        log_->scan_keys(request.collection, after, /*inclusive=*/false, request.descending,
                        [&](StoredRecord&& record) {
                            std::string position = to_hex(record.key);
                            return take(std::move(record), std::move(position));
                        });
    } else {
        if (std::ranges::find(definition->indexes, request.index) == definition->indexes.end()) {
            return fail(RecordStatus::InvalidInput,
                        "collection '" + request.collection + "' declares no index '" +
                            request.index + "'",
                        "/index");
        }
        std::optional<std::pair<std::string, std::string>> after;
        if (!request.after.empty()) {
            const std::size_t dot = request.after.find('.');
            const auto order = dot == std::string::npos ? std::nullopt
                                                        : from_hex(request.after.substr(0, dot));
            const auto key = dot == std::string::npos ? std::nullopt
                                                      : from_hex(request.after.substr(dot + 1));
            if (!order || !key) return fail(RecordStatus::InvalidInput, "not a cursor", "/after");
            after = std::pair{*order, *key};
        }
        log_->scan_index(request.collection, request.index, request.value, after,
                         request.descending, [&](StoredRecord&& record) {
                             std::string position = to_hex(record.order) + "." + to_hex(record.key);
                             return take(std::move(record), std::move(position));
                         });
    }
    if (more) page.next_after = last_position;
    return page;
}

void RecordStore::sweep(std::int64_t now) {
    // Expiry and the deletion grace are properties of the record, the same on
    // every node, so each node removes its own copies; the revision check makes a
    // removal lose to a write that landed since the key was queued.
    for (;;) {
        const std::vector<RecordLog::Due> due = log_->take_due(now, 4096);
        for (const RecordLog::Due& entry : due) log_->purge(entry.collection, entry.key, entry.revision);
        if (due.size() < 4096) break;
    }
}

void RecordStore::maintain() { log_->maintain(); }

RecordStorageStats RecordStore::storage_stats() const { return log_->stats(); }

bool RecordStore::fold(const std::string& collection, const std::string& key,
                       const std::function<std::optional<std::string>(const std::string*)>& fold,
                       std::optional<std::int64_t> expires_ms) {
    const std::int64_t now = now_ms();
    return log_->update(collection, key,
                        [&](const StoredRecord* existing) -> std::optional<StoredRecord> {
                            std::optional<std::string> value =
                                fold(existing && !existing->deleted ? &existing->value : nullptr);
                            if (!value) return std::nullopt;
                            StoredRecord next;
                            next.revision = std::max(existing ? existing->revision + 1 : 1, now_micros());
                            next.updated_ms = now;
                            next.value = std::move(*value);
                            next.expires_ms = expires_ms;
                            return next;
                        },
                        /*with_value=*/true, /*sync=*/false)
        .record.has_value();
}

void RecordStore::scan(const std::string& collection, const std::string& from_key,
                       const std::function<bool(const std::string&, const std::string&)>& visit) const {
    const std::int64_t now = now_ms();
    log_->scan_keys(collection, from_key, /*inclusive=*/true, /*descending=*/false,
                    [&](StoredRecord&& record) {
                        if (!visible(record, now)) return true;
                        return visit(record.key, record.value);
                    });
}

bool RecordStore::put_internal(const std::string& collection, const std::string& key,
                               std::string value) {
    if (!valid_stored_collection(collection) || valid_collection_name(collection) ||
        !valid_key(key)) {
        return false;
    }
    const std::int64_t now = now_ms();
    const RecordLog::Written written = log_->update(
        collection, key,
        [&](const StoredRecord* existing) -> std::optional<StoredRecord> {
            StoredRecord next;
            next.revision = std::max(existing ? existing->revision + 1 : 1, now_micros());
            next.updated_ms = now;
            next.value = std::move(value);
            return next;
        },
        /*with_value=*/true, /*sync=*/true);
    if (!written.record || !written.durable) return false;
    RecordOutcome outcome;
    replicate(apply_payload(*written.record), {}, outcome);
    return true;
}

bool RecordStore::remove_internal(const std::string& collection, const std::string& key) {
    if (!valid_stored_collection(collection) || valid_collection_name(collection) ||
        !valid_key(key)) {
        return false;
    }
    const std::int64_t now = now_ms();
    const RecordLog::Written written = log_->update(
        collection, key,
        [&](const StoredRecord* existing) -> std::optional<StoredRecord> {
            if (!existing || existing->deleted) return std::nullopt;
            StoredRecord next;
            next.revision = std::max(existing->revision + 1, now_micros());
            next.updated_ms = now;
            next.deleted = true;
            return next;
        },
        /*with_value=*/false, /*sync=*/true);
    if (!written.record) return true;  // nothing to remove
    if (!written.durable) return false;
    RecordOutcome outcome;
    replicate(apply_payload(*written.record), {}, outcome);
    return true;
}

std::size_t RecordStore::pull_from(const std::string& peer) {
    zetta::cluster::Cluster* cluster = cluster_.load();
    if (!cluster) return 0;
    const auto ask = [&](std::string_view method, const nlohmann::json& payload)
        -> std::optional<nlohmann::json> {
        const zetta::cluster::SignalResponse response =
            cluster->request(peer, std::string(method), payload.dump(), kAntiEntropyTimeout);
        if (!response.ok) return std::nullopt;
        nlohmann::json document = nlohmann::json::parse(response.payload, nullptr, false);
        if (document.is_discarded()) return std::nullopt;
        return document;
    };
    const auto theirs = ask(kDigestMethod, {{"op", "digest"}});
    if (!theirs || !theirs->is_object()) return 0;
    const auto ours = log_->digests();
    const std::int64_t now = now_ms();
    std::size_t pulled = 0;
    for (const auto& [collection, digests] : theirs->items()) {
        if (!valid_stored_collection(collection) || !digests.is_array() ||
            digests.size() != RecordLog::kBuckets) {
            continue;
        }
        const auto mine = ours.find(collection);
        std::vector<std::size_t> differing;
        for (std::size_t bucket = 0; bucket < RecordLog::kBuckets; ++bucket) {
            if (!digests[bucket].is_number_unsigned()) continue;
            const std::uint64_t mine_digest = mine == ours.end() ? 0 : mine->second[bucket];
            if (digests[bucket].get<std::uint64_t>() != mine_digest) differing.push_back(bucket);
        }
        for (std::size_t start = 0; start < differing.size(); start += kBucketsPerRequest) {
            const std::vector<std::size_t> buckets(
                differing.begin() + static_cast<std::ptrdiff_t>(start),
                differing.begin() + static_cast<std::ptrdiff_t>(
                                        std::min(differing.size(), start + kBucketsPerRequest)));
            const auto versions =
                ask(kKeysMethod, {{"op", "keys"}, {"collection", collection}, {"buckets", buckets}});
            if (!versions || !versions->is_array()) continue;
            std::vector<std::string> wanted;
            for (const nlohmann::json& version : *versions) {
                if (!version.is_object() || !version.contains("k") || !version["k"].is_string() ||
                    !version.contains("r") || !version["r"].is_number_unsigned()) {
                    continue;
                }
                RecordVersion parsed;
                parsed.key = version["k"].get<std::string>();
                parsed.revision = version["r"].get<std::uint64_t>();
                parsed.deleted = version.value("d", false);
                parsed.updated_ms = version.value("u", std::int64_t{0});
                if (version.contains("e") && version["e"].is_number_integer()) {
                    parsed.expires_ms = version["e"].get<std::int64_t>();
                }
                if (!valid_key(parsed.key)) continue;
                if (worth_pulling(parsed, log_->get(collection, parsed.key, false), now)) {
                    wanted.push_back(parsed.key);
                }
            }
            for (std::size_t from = 0; from < wanted.size(); from += kKeysPerFetch) {
                const std::vector<std::string> keys(
                    wanted.begin() + static_cast<std::ptrdiff_t>(from),
                    wanted.begin() + static_cast<std::ptrdiff_t>(
                                         std::min(wanted.size(), from + kKeysPerFetch)));
                const auto records =
                    ask(kFetchMethod, {{"op", "fetch"}, {"collection", collection}, {"keys", keys}});
                if (!records || !records->is_array()) continue;
                for (const nlohmann::json& entry : *records) {
                    if (!entry.is_object() || !entry.contains("k") || !entry["k"].is_string() ||
                        !entry.contains("r")) {
                        continue;
                    }
                    const auto record = parse_stored(collection, entry["k"].get<std::string>(), entry["r"]);
                    if (record && valid_key(record->key) && install(*record, now)) ++pulled;
                }
            }
        }
    }
    return pulled;
}

std::size_t RecordStore::anti_entropy() {
    std::size_t pulled = 0;
    for (const std::string& peer : live_peers()) pulled += pull_from(peer);
    return pulled;
}

zetta::cluster::SignalResponse RecordStore::on_request(
    const zetta::cluster::Signal& request) {
    const bool ours = request.method == kPutMethod || request.method == kApplyMethod ||
                      request.method == kDigestMethod || request.method == kKeysMethod ||
                      request.method == kFetchMethod;
    if (!ours) {
        if (auto* delegate = delegate_.load()) return delegate->on_request(request);
        return {.ok = false, .payload = {}, .error = "no handler"};
    }
    std::string reason;
    const auto payload = decode_signal_payload(request.payload, &reason);
    if (!payload) {
        return {.ok = false, .payload = {}, .error = std::string(request.method) + ": " + reason};
    }
    const std::string op = payload->at("op").get<std::string>();
    const auto refuse = [&](std::string why) {
        return zetta::cluster::SignalResponse{
            .ok = false, .payload = {}, .error = std::string(request.method) + ": " + why};
    };
    const auto answer = [](const nlohmann::json& body) {
        return zetta::cluster::SignalResponse{.ok = true, .payload = body.dump(), .error = {}};
    };
    const std::string collection = payload->value("collection", std::string());

    if (request.method == kDigestMethod) {
        nlohmann::json digests = nlohmann::json::object();
        for (const auto& [name, buckets] : log_->digests()) digests[name] = buckets;
        return answer(digests);
    }
    if (request.method == kKeysMethod) {
        if (op != "keys" || !valid_stored_collection(collection) || !payload->contains("buckets")) {
            return refuse("want a collection and buckets");
        }
        nlohmann::json versions = nlohmann::json::array();
        for (const RecordVersion& version :
             log_->versions(collection, payload->at("buckets").get<std::vector<std::size_t>>())) {
            nlohmann::json entry{{"k", version.key}, {"r", version.revision}, {"u", version.updated_ms}};
            if (version.deleted) entry["d"] = true;
            if (version.expires_ms) entry["e"] = *version.expires_ms;
            versions.push_back(std::move(entry));
        }
        return answer(versions);
    }
    if (request.method == kFetchMethod) {
        if (op != "fetch" || !valid_stored_collection(collection) || !payload->contains("keys")) {
            return refuse("want a collection and keys");
        }
        nlohmann::json records = nlohmann::json::array();
        for (const std::string& key : payload->at("keys").get<std::vector<std::string>>()) {
            if (auto record = log_->get(collection, key, /*with_value=*/true)) {
                records.push_back({{"k", key}, {"r", stored_value(*record)}});
            }
        }
        return answer(records);
    }
    if (request.method == kApplyMethod) {
        if (op != "apply") return refuse("want op apply");
        if (payload->contains("definition")) {
            std::string why;
            const auto definition = record_collection_from_json(payload->at("definition"), &why);
            if (!definition) return refuse(why);
            install_collection(*definition);
            return {.ok = true, .payload = {}, .error = {}};
        }
        const std::string key = payload->value("key", std::string());
        if (!valid_stored_collection(collection) || !valid_key(key) || !payload->contains("record")) {
            return refuse("want a collection, a key and a record");
        }
        const auto record = parse_stored(collection, key, payload->at("record"));
        if (!record) return refuse("bad record");
        install(*record, now_ms());
        return {.ok = true, .payload = {}, .error = {}};
    }
    if (op != "put" && op != "delete") return refuse("want put or delete");
    const auto write = write_from_payload(*payload);
    if (!write) return refuse("want a collection and key");
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
    return answer(outcome_payload(outcome));
}

void RecordStore::on_event(const zetta::cluster::Signal& event) {
    if (auto* delegate = delegate_.load()) delegate->on_event(event);
}

void RecordStore::on_task(const zetta::cluster::Signal& task) {
    if (auto* delegate = delegate_.load()) delegate->on_task(task);
}

}  // namespace zetta::data
