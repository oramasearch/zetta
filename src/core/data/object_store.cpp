#include "zetta/data/object_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <random>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"

#include "../catalog.hpp"
#include "object_log.hpp"

// The object capability (#1005). See include/zetta/data/object_store.hpp for the
// design and docs/data/object-store.md for the contract.
namespace zetta::data {
namespace {

using zetta::cluster::ReplicationWrite;
using zetta::cluster::SignalResponse;

constexpr std::size_t kMaxKeyBytes = 1024;
constexpr std::size_t kMaxHolderBytes = 256;
constexpr auto kForwardTimeout = std::chrono::seconds(10);
constexpr auto kReplicateTimeout = std::chrono::seconds(10);
constexpr auto kAntiEntropyTimeout = std::chrono::seconds(10);
constexpr std::size_t kBucketsPerRequest = 32;
constexpr std::size_t kKeysPerRequest = 256;
constexpr std::string_view kLeaseKey = "\x01lease";  // never a valid object key

constexpr std::string_view kWriteMethod = "object.write";
constexpr std::string_view kApplyMethod = "object.apply";
constexpr std::string_view kStageMethod = "object.stage";
constexpr std::string_view kAbortMethod = "object.abort";
constexpr std::string_view kBarrierMethod = "object.barrier";
constexpr std::string_view kReadMethod = "object.read";
constexpr std::string_view kStatMethod = "object.stat";
constexpr std::string_view kListMethod = "object.list";
constexpr std::string_view kUsageMethod = "object.usage";
constexpr std::string_view kDigestMethod = "object.digest";
constexpr std::string_view kKeysMethod = "object.keys";
constexpr std::string_view kLeaseMethod = "object.lease";
constexpr std::string_view kLeaseApplyMethod = "object.lease_apply";
constexpr std::string_view kDefineMethod = "object.define";

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

// An upload id: 53 random bits, so it survives every JSON reader as an exact
// integer (the signal schema's validator compares above 2^63 as a double).
std::uint64_t random_id() {
    thread_local std::mt19937_64 generator{std::random_device{}() ^ now_micros()};
    std::uint64_t id = 0;
    while (id == 0) id = generator() >> 11;
    return id;
}

std::string bucket_key(std::string_view bucket) {
    return std::string(catalog::kObjectBucketPrefix) + std::string(bucket);
}

std::string lease_key(std::string_view bucket) {
    return std::string(catalog::kObjectLeasePrefix) + std::string(bucket);
}

bool valid_holder(std::string_view holder) {
    if (holder.empty() || holder.size() > kMaxHolderBytes) return false;
    return std::ranges::none_of(holder, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7f;
    });
}

std::uint64_t rendezvous_weight(std::string_view node, std::string_view bucket,
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
    mix(bucket);
    mix(key);
    // A final avalanche: FNV's low bits correlate across similar node names.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    return hash;
}

ObjectOutcome failure(ObjectStatus status, std::string message, std::string path = {}) {
    ObjectOutcome outcome;
    outcome.status = status;
    outcome.message = std::move(message);
    outcome.path = std::move(path);
    return outcome;
}

std::optional<ObjectStatus> status_from_string(std::string_view text) {
    for (const ObjectStatus status :
         {ObjectStatus::Ok, ObjectStatus::InvalidInput, ObjectStatus::NotFound,
          ObjectStatus::Conflict, ObjectStatus::Fenced, ObjectStatus::Held,
          ObjectStatus::OutOfRange, ObjectStatus::QuotaExceeded, ObjectStatus::Unavailable}) {
        if (to_string(status) == text) return status;
    }
    return std::nullopt;
}

// ---- the signal boundary ----------------------------------------------------------
//
// Every object.* payload is one JSON header line, then (for the methods that move
// bytes) the raw bytes. The signal envelope base64s the whole payload, so the
// bytes are carried once, not base64 inside JSON.

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

std::string encode_payload(const nlohmann::json& header, std::string_view bytes = {}) {
    std::string out = header.dump();
    out.push_back('\n');
    out.append(bytes);
    return out;
}

// Split a payload into its header and bytes. `validate` checks the header
// against the signal schema (requests); a response header is type-checked by its
// reader instead.
bool decode_payload(std::string_view payload, nlohmann::json& header, std::string_view& bytes,
                    bool validate, std::string* reason) {
    const std::size_t newline = payload.find('\n');
    const std::string_view text = payload.substr(0, newline);
    header = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    bytes = newline == std::string_view::npos ? std::string_view{} : payload.substr(newline + 1);
    if (header.is_discarded()) {
        if (reason) *reason = "object signal header is not valid JSON";
        return false;
    }
    if (!validate) return true;
    static nlohmann::json_schema::json_validator validator(object_signal_schema());
    CollectingErrors errors;
    validator.validate(header, errors);
    if (errors.failed) {
        if (reason) *reason = errors.reason;
        return false;
    }
    return true;
}

nlohmann::json info_json(const ObjectInfo& info) {
    return {{"k", info.key}, {"g", info.generation}, {"s", info.size}, {"u", info.updated_ms}};
}

std::optional<ObjectInfo> info_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto key = value.find("k");
    const auto generation = value.find("g");
    const auto size = value.find("s");
    if (key == value.end() || !key->is_string() || generation == value.end() ||
        !generation->is_number_unsigned() || size == value.end() || !size->is_number_unsigned()) {
        return std::nullopt;
    }
    ObjectInfo info;
    info.key = key->get<std::string>();
    info.generation = generation->get<std::uint64_t>();
    info.size = size->get<std::uint64_t>();
    if (const auto updated = value.find("u"); updated != value.end() && updated->is_number_integer()) {
        info.updated_ms = updated->get<std::int64_t>();
    }
    return info;
}

nlohmann::json outcome_json(const ObjectOutcome& outcome) {
    nlohmann::json value{{"status", to_string(outcome.status)},
                         {"message", outcome.message},
                         {"path", outcome.path}};
    if (outcome.object) value["object"] = info_json(*outcome.object);
    if (outcome.unchanged) value["unchanged"] = true;
    if (outcome.fence != 0) value["fence"] = outcome.fence;
    if (outcome.lease) value["lease"] = to_json(*outcome.lease);
    return value;
}

// A peer's answer is untrusted: every field is type-checked before use.
std::optional<ObjectOutcome> outcome_from_json(const nlohmann::json& document,
                                               const std::string& bucket) {
    if (!document.is_object()) return std::nullopt;
    const auto status = document.find("status");
    if (status == document.end() || !status->is_string()) return std::nullopt;
    const auto parsed = status_from_string(status->get<std::string>());
    if (!parsed) return std::nullopt;
    ObjectOutcome outcome;
    outcome.status = *parsed;
    for (const auto& [field, target] :
         {std::pair{"message", &outcome.message}, std::pair{"path", &outcome.path}}) {
        const auto found = document.find(field);
        if (found != document.end() && found->is_string()) *target = found->get<std::string>();
    }
    if (const auto object = document.find("object"); object != document.end()) {
        outcome.object = info_from_json(*object);
        if (!outcome.object) return std::nullopt;
    }
    if (const auto unchanged = document.find("unchanged");
        unchanged != document.end() && unchanged->is_boolean()) {
        outcome.unchanged = unchanged->get<bool>();
    }
    if (const auto fence = document.find("fence");
        fence != document.end() && fence->is_number_unsigned()) {
        outcome.fence = fence->get<std::uint64_t>();
    }
    if (const auto lease = document.find("lease"); lease != document.end()) {
        outcome.lease = object_lease_from_json(bucket, *lease);
        if (!outcome.lease) return std::nullopt;
    }
    return outcome;
}

ObjectInfo info_of(const ObjectVersion& version) {
    return ObjectInfo{version.key, version.size, version.generation, version.updated_ms};
}

}  // namespace

// ---- public helpers ------------------------------------------------------------------

std::string_view to_string(ObjectStatus status) {
    switch (status) {
        case ObjectStatus::Ok: return "ok";
        case ObjectStatus::InvalidInput: return "invalid_input";
        case ObjectStatus::NotFound: return "not_found";
        case ObjectStatus::Conflict: return "conflict";
        case ObjectStatus::Fenced: return "fenced";
        case ObjectStatus::Held: return "held";
        case ObjectStatus::OutOfRange: return "range_not_satisfiable";
        case ObjectStatus::QuotaExceeded: return "quota_exceeded";
        case ObjectStatus::Unavailable: return "unavailable";
    }
    return "ok";
}

bool valid_bucket_name(std::string_view name) {
    if (name.empty() || name.size() > 96) return false;
    const auto lower_or_digit = [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    };
    if (!lower_or_digit(name.front())) return false;
    return std::ranges::all_of(name, [&](char character) {
        return lower_or_digit(character) || character == '_' || character == '.' ||
               character == '-';
    });
}

bool valid_object_key(std::string_view key) {
    if (key.empty() || key.size() > kMaxKeyBytes) return false;
    return std::ranges::none_of(key, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x20 || byte == 0x7f;
    });
}

const nlohmann::json& object_bucket_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.ObjectBucket",
  "description": "An object bucket (#1005): the replicated `object_bucket:<name>` value. `quota_bytes`, when present, caps the bucket's object bytes (a write past it is refused); `rf`, when present, is how many nodes hold each object on a multi-node cluster (default min(3, nodes)); `created` is milliseconds since the Unix epoch. The objects themselves live in each owner's node-local object log, never here.",
  "type": "object",
  "required": ["name", "created"],
  "additionalProperties": false,
  "properties": {
    "name": {"type": "string", "pattern": "^[a-z0-9][a-z0-9_.-]{0,95}$"},
    "quota_bytes": {"type": "integer", "minimum": 0},
    "rf": {"type": "integer", "minimum": 1, "maximum": 16},
    "created": {"type": "integer"}
  }
}
)JSONSCHEMA");
    return schema;
}

const nlohmann::json& object_lease_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.ObjectLease",
  "description": "An object bucket's owner lease (#1005): the replicated `object_lease:<name>` value. `fence` rises every time a new holder acquires the lease, and every mutation of a bucket that has had a holder must carry it; `holder` is empty once released; `expires` is milliseconds since the Unix epoch; `version` orders lease states (the higher one wins).",
  "type": "object",
  "required": ["holder", "fence", "expires", "version"],
  "additionalProperties": false,
  "properties": {
    "holder": {"type": "string", "maxLength": 256},
    "fence": {"type": "integer", "minimum": 0},
    "expires": {"type": "integer"},
    "version": {"type": "integer", "minimum": 0}
  }
}
)JSONSCHEMA");
    return schema;
}

const nlohmann::json& object_signal_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "zetta.ObjectSignal",
  "description": "The JSON header of an object-store signal (#1005); for the methods that move bytes, the raw bytes follow the header after one newline. object.write forwards a mutation to the key's primary owner (`op` append, put, commit or remove); object.apply installs a decided version on another owner; object.stage and object.abort carry an upload's parts; object.barrier fsyncs a bucket; object.read, object.stat and object.list read; object.usage counts a bucket's objects; object.digest and object.keys are anti-entropy (for the owner named in `for`); object.lease forwards a lease decision (`op` acquire, renew or release) and object.lease_apply installs one; object.define installs a bucket definition.",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "op": {"enum": ["append", "put", "commit", "remove", "acquire", "renew", "release"]},
    "bucket": {"type": "string", "pattern": "^[a-z0-9][a-z0-9_.-]{0,95}$"},
    "key": {"type": "string", "minLength": 1, "maxLength": 1024},
    "keys": {"type": "array", "maxItems": 256, "items": {"type": "string"}},
    "buckets": {"type": "array", "maxItems": 256, "items": {"type": "integer", "minimum": 0, "maximum": 255}},
    "offset": {"type": "integer", "minimum": 0},
    "length": {"type": "integer", "minimum": 0},
    "probe": {"type": "boolean"},
    "upload": {"type": "integer", "minimum": 1},
    "generation": {"type": "integer", "minimum": 1},
    "size": {"type": "integer", "minimum": 0},
    "updated": {"type": "integer"},
    "if_absent": {"type": "boolean"},
    "durable": {"type": "boolean"},
    "fence": {"type": "integer", "minimum": 0},
    "holder": {"type": "string", "maxLength": 256},
    "ttl_ms": {"type": "integer", "minimum": 1},
    "prefix": {"type": "string", "maxLength": 1024},
    "after": {"type": "string", "maxLength": 1024},
    "limit": {"type": "integer", "minimum": 1, "maximum": 1001},
    "for": {"type": "string", "maxLength": 128},
    "lease": {"type": "object"},
    "definition": {"type": "object"}
  }
}
)JSONSCHEMA");
    return schema;
}

nlohmann::json to_json(const ObjectBucket& bucket) {
    nlohmann::json value{{"name", bucket.name}, {"created", bucket.created_ms}};
    if (bucket.quota_bytes) value["quota_bytes"] = *bucket.quota_bytes;
    if (bucket.rf) value["rf"] = *bucket.rf;
    return value;
}

nlohmann::json to_json(const ObjectLease& lease) {
    return {{"holder", lease.holder},
            {"fence", lease.fence},
            {"expires", lease.expires_ms},
            {"version", lease.version}};
}

std::optional<ObjectBucket> object_bucket_from_json(const nlohmann::json& value,
                                                    std::string* reason) {
    const auto fail = [&](std::string why) -> std::optional<ObjectBucket> {
        if (reason) *reason = std::move(why);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("/: not an object");
    for (const auto& [field, _] : value.items()) {
        if (field != "name" && field != "quota_bytes" && field != "rf" && field != "created") {
            return fail("/" + field + ": unknown field");
        }
    }
    const auto name = value.find("name");
    if (name == value.end() || !name->is_string() || !valid_bucket_name(name->get<std::string>())) {
        return fail("/name: want a bucket name");
    }
    ObjectBucket bucket;
    bucket.name = name->get<std::string>();
    if (const auto quota = value.find("quota_bytes"); quota != value.end()) {
        if (!quota->is_number_unsigned()) return fail("/quota_bytes: want a non-negative integer");
        bucket.quota_bytes = quota->get<std::uint64_t>();
    }
    if (const auto rf = value.find("rf"); rf != value.end()) {
        if (!rf->is_number_unsigned() || rf->get<std::uint64_t>() < 1 ||
            rf->get<std::uint64_t>() > 16) {
            return fail("/rf: want an integer from 1 to 16");
        }
        bucket.rf = static_cast<std::uint32_t>(rf->get<std::uint64_t>());
    }
    const auto created = value.find("created");
    if (created == value.end() || !created->is_number_integer()) {
        return fail("/created: want an integer");
    }
    bucket.created_ms = created->get<std::int64_t>();
    return bucket;
}

std::optional<ObjectLease> object_lease_from_json(const std::string& bucket,
                                                  const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    const auto holder = value.find("holder");
    const auto fence = value.find("fence");
    const auto expires = value.find("expires");
    const auto version = value.find("version");
    if (holder == value.end() || !holder->is_string() || fence == value.end() ||
        !fence->is_number_unsigned() || expires == value.end() || !expires->is_number_integer() ||
        version == value.end() || !version->is_number_unsigned()) {
        return std::nullopt;
    }
    ObjectLease lease;
    lease.bucket = bucket;
    lease.holder = holder->get<std::string>();
    lease.fence = fence->get<std::uint64_t>();
    lease.expires_ms = expires->get<std::int64_t>();
    lease.version = version->get<std::uint64_t>();
    if (lease.holder.size() > kMaxHolderBytes) return std::nullopt;
    return lease;
}

// ---- the store -------------------------------------------------------------------------

namespace {

// What a bucket whose log has failed an fsync answers (#1235): not "absent",
// not empty, but unavailable until the log is reopened from disk.
ObjectOutcome failed_log(const std::string& bucket, const std::string& node) {
    return failure(ObjectStatus::Unavailable,
                   "the object log of bucket '" + bucket + "' on " + node +
                       " failed an fsync and is being reopened from disk; retry");
}

}  // namespace

// One bucket's node-local state.
struct ObjectStore::Bucket {
    std::unique_ptr<ObjectLog> log;
    // Writes hold it shared across their fence check and their append; a lease
    // install holds it exclusively, so once a new fence is installed here no write
    // checked against the old one can still land.
    std::shared_mutex fence_mutex;
    std::optional<ObjectLease> lease;  // the newest pushed to this node (guarded by fence_mutex)
    std::mutex watch_mutex;
    std::vector<std::function<void()>> watchers;
    std::mutex peers_mutex;
    std::map<std::string, std::uint64_t> peer_held;  // node -> bytes it holds, from anti-entropy
};

// A mutation as the primary decides it or an owner installs it.
struct ObjectStore::Mutation {
    enum class Op { Append, Put, Commit, Remove };
    Op op = Op::Append;
    std::string bucket;
    std::string key;
    std::uint64_t offset = 0;      // Append
    std::uint64_t upload = 0;      // Commit
    std::uint64_t size = 0;        // Commit
    bool if_absent = false;        // Put, Commit
    ObjectWrite options;
    std::uint64_t generation = 0;  // install: the decided version
    std::int64_t updated_ms = 0;   // install of a Remove

    static std::string_view op_name(Op op) {
        switch (op) {
            case Op::Append: return "append";
            case Op::Put: return "put";
            case Op::Commit: return "commit";
            case Op::Remove: return "remove";
        }
        return "append";
    }

    nlohmann::json header() const {
        nlohmann::json value{{"op", op_name(op)}, {"bucket", bucket}, {"key", key}};
        if (op == Op::Append) value["offset"] = offset;
        if (op == Op::Commit) {
            value["upload"] = upload;
            value["size"] = size;
        }
        if (if_absent) value["if_absent"] = true;
        if (options.durable) value["durable"] = true;
        if (options.fence) value["fence"] = *options.fence;
        if (generation != 0) value["generation"] = generation;
        if (updated_ms != 0) value["updated"] = updated_ms;
        return value;
    }

    static std::optional<Mutation> from_header(const nlohmann::json& header) {
        if (!header.contains("op") || !header.contains("bucket") || !header.contains("key")) {
            return std::nullopt;
        }
        Mutation mutation;
        const std::string op = header.at("op").get<std::string>();
        if (op == "append") mutation.op = Op::Append;
        else if (op == "put") mutation.op = Op::Put;
        else if (op == "commit") mutation.op = Op::Commit;
        else if (op == "remove") mutation.op = Op::Remove;
        else return std::nullopt;
        mutation.bucket = header.at("bucket").get<std::string>();
        mutation.key = header.at("key").get<std::string>();
        mutation.offset = header.value("offset", std::uint64_t{0});
        mutation.upload = header.value("upload", std::uint64_t{0});
        mutation.size = header.value("size", std::uint64_t{0});
        mutation.if_absent = header.value("if_absent", false);
        mutation.options.durable = header.value("durable", false);
        if (header.contains("fence")) mutation.options.fence = header.at("fence").get<std::uint64_t>();
        mutation.generation = header.value("generation", std::uint64_t{0});
        mutation.updated_ms = header.value("updated", std::int64_t{0});
        if (mutation.op == Op::Commit && mutation.upload == 0) return std::nullopt;
        return mutation;
    }
};

namespace {

// A read of this node's copy: the snapshot holds the segments it reads from.
class LocalReader final : public ObjectReader {
public:
    explicit LocalReader(ObjectLog::ReadSnapshot snapshot)
        : snapshot_(std::move(snapshot)), info_(info_of(snapshot_.version)) {}
    const ObjectInfo& info() const override { return info_; }
    std::uint64_t offset() const override { return snapshot_.offset; }
    std::uint64_t length() const override { return snapshot_.length; }
    bool read(std::uint64_t from, std::uint64_t count, std::string& out) override {
        if (from + count > snapshot_.length) return false;
        return snapshot_.read_into(from, count, out);
    }

private:
    ObjectLog::ReadSnapshot snapshot_;
    ObjectInfo info_;
};

// A read of a peer's copy, pinned to the version's generation, a slice a request.
class RemoteReader final : public ObjectReader {
public:
    RemoteReader(zetta::cluster::Cluster& cluster, std::string peer, std::string bucket,
                 ObjectInfo info, std::uint64_t offset, std::uint64_t length)
        : cluster_(cluster), peer_(std::move(peer)), bucket_(std::move(bucket)),
          info_(std::move(info)), offset_(offset), length_(length) {}
    const ObjectInfo& info() const override { return info_; }
    std::uint64_t offset() const override { return offset_; }
    std::uint64_t length() const override { return length_; }
    bool read(std::uint64_t from, std::uint64_t count, std::string& out) override {
        if (from + count > length_) return false;
        out.clear();
        out.reserve(count);
        while (out.size() < count) {
            const std::uint64_t take =
                std::min<std::uint64_t>(count - out.size(), ObjectStore::kSliceBytes);
            const nlohmann::json request{{"bucket", bucket_},
                                         {"key", info_.key},
                                         {"generation", info_.generation},
                                         {"offset", offset_ + from + out.size()},
                                         {"length", take}};
            const SignalResponse response = cluster_.request(
                peer_, std::string(kReadMethod), encode_payload(request), kForwardTimeout);
            if (!response.ok) return false;
            nlohmann::json header;
            std::string_view bytes;
            if (!decode_payload(response.payload, header, bytes, false, nullptr) ||
                header.value("status", std::string()) != "ok" || bytes.size() != take) {
                return false;
            }
            out.append(bytes);
        }
        return true;
    }

private:
    zetta::cluster::Cluster& cluster_;
    std::string peer_;
    std::string bucket_;
    ObjectInfo info_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

ObjectOutcome from_written(const ObjectLog::Written& written, const std::string& node,
                           const std::string& key) {
    ObjectOutcome outcome;
    outcome.owner = node;
    switch (written.result) {
        case ObjectLog::Result::Ok:
        case ObjectLog::Result::Stale:
            break;
        case ObjectLog::Result::Unchanged:
            outcome.unchanged = true;
            break;
        case ObjectLog::Result::Conflict: {
            outcome.status = ObjectStatus::Conflict;
            const std::uint64_t size = written.version ? written.version->size : 0;
            outcome.message = "object '" + key + "' is " + std::to_string(size) +
                              " bytes; the write does not fit at that offset";
            outcome.path = "/offset";
            break;
        }
        case ObjectLog::Result::Gap:
            outcome.status = ObjectStatus::Unavailable;
            outcome.message = "owner " + node + " is missing earlier bytes of '" + key +
                              "' and is being repaired; retry";
            break;
        case ObjectLog::Result::NotFound:
            outcome.status = ObjectStatus::NotFound;
            outcome.message = "no object '" + key + "'";
            outcome.path = "/key";
            break;
        case ObjectLog::Result::Io:
            outcome.status = ObjectStatus::Unavailable;
            outcome.message = "the object log could not be written on " + node;
            break;
    }
    if (written.version) outcome.object = info_of(*written.version);
    if (outcome.ok() && !written.durable) {
        return failure(ObjectStatus::Unavailable,
                       "the write was applied but could not be synced to disk on " + node);
    }
    return outcome;
}

}  // namespace

ObjectStore::ObjectStore(zetta::cluster::ReplicationManager& replication, std::string node_id,
                         std::filesystem::path directory, std::uint64_t segment_bytes)
    : replication_(replication),
      node_id_(std::move(node_id)),
      directory_(std::move(directory)),
      segment_bytes_(segment_bytes) {}

ObjectStore::~ObjectStore() { detach(); }

bool ObjectStore::open(std::string* reason) {
    if (directory_.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        if (reason) *reason = "cannot create " + directory_.string() + ": " + error.message();
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_directory() || !valid_bucket_name(name)) continue;
        auto state = std::make_unique<Bucket>();
        state->log = std::make_unique<ObjectLog>(entry.path(), kTombstoneGraceMs, segment_bytes_);
        if (fsync_) state->log->set_fsync(fsync_);
        if (!state->log->open(reason)) return false;
        const std::unique_lock lock(buckets_mutex_);
        buckets_[name] = std::move(state);
    }
    return true;
}

void ObjectStore::attach(zetta::cluster::Cluster& cluster) {
    detach();
    delegate_.store(cluster.signal_handler());
    cluster_.store(&cluster);
    cluster.set_signal_handler(this);
}

void ObjectStore::detach() {
    zetta::cluster::Cluster* cluster = cluster_.exchange(nullptr);
    if (!cluster) return;
    if (cluster->signal_handler() == this) cluster->set_signal_handler(delegate_.load());
    delegate_.store(nullptr);
}

ObjectStore::Bucket* ObjectStore::bucket_state(const std::string& name, bool create) {
    {
        const std::shared_lock lock(buckets_mutex_);
        if (const auto found = buckets_.find(name); found != buckets_.end()) {
            return found->second.get();
        }
    }
    if (!create || !valid_bucket_name(name)) return nullptr;
    const std::unique_lock lock(buckets_mutex_);
    if (const auto found = buckets_.find(name); found != buckets_.end()) return found->second.get();
    auto state = std::make_unique<Bucket>();
    state->log = std::make_unique<ObjectLog>(
        directory_.empty() ? std::filesystem::path{} : directory_ / name, kTombstoneGraceMs,
        segment_bytes_);
    if (fsync_) state->log->set_fsync(fsync_);
    std::string reason;
    if (!state->log->open(&reason)) {
        std::fprintf(stderr, "zetta: cannot open object bucket %s: %s\n", name.c_str(),
                     reason.c_str());
        return nullptr;
    }
    Bucket* raw = state.get();
    buckets_[name] = std::move(state);
    return raw;
}

std::vector<std::string> ObjectStore::live_peers() const {
    std::vector<std::string> peers;
    if (zetta::cluster::Cluster* cluster = cluster_.load()) {
        for (const zetta::cluster::ClusterPeer& peer : cluster->peers()) {
            if (peer.node_id != node_id_) peers.push_back(peer.node_id);
        }
    }
    return peers;
}

namespace {

// Candidates for placement: this node and its live peers, permanent ones only
// when any is reachable (an ephemeral node may vanish at any moment — #809).
std::vector<std::string> placement_candidates(const zetta::cluster::ReplicationManager& replication,
                                              const std::string& self,
                                              std::vector<std::string> peers) {
    peers.push_back(self);
    const auto view = replication.shared_snapshot();
    const auto ephemeral = [&](const std::string& node) {
        const auto found = view->find(catalog::node_key(node));
        return found != view->end() && found->is_object() &&
               found->value("node_type", std::string()) == "ephemeral";
    };
    std::vector<std::string> permanent;
    std::ranges::copy_if(peers, std::back_inserter(permanent),
                         [&](const std::string& node) { return !ephemeral(node); });
    return permanent.empty() ? peers : permanent;
}

std::vector<std::string> ranked(std::vector<std::string> candidates, const std::string& bucket,
                                const std::string& key) {
    std::ranges::sort(candidates, [&](const std::string& left, const std::string& right) {
        const std::uint64_t a = rendezvous_weight(left, bucket, key);
        const std::uint64_t b = rendezvous_weight(right, bucket, key);
        return a != b ? a > b : left > right;
    });
    return candidates;
}

}  // namespace

std::uint32_t ObjectStore::replication_factor(const std::string& bucket) const {
    const std::optional<ObjectBucket> definition = find_bucket(bucket);
    return definition && definition->rf ? *definition->rf : 3u;
}

ObjectStore::Placement ObjectStore::placement(const std::string& bucket) const {
    Placement placement;
    placement.candidates = placement_candidates(replication_, node_id_, live_peers());
    placement.rf = replication_factor(bucket);
    return placement;
}

std::vector<std::string> ObjectStore::Placement::owners(const std::string& bucket,
                                                        const std::string& key) const {
    std::vector<std::string> owners = ranked(candidates, bucket, key);
    owners.resize(std::min<std::size_t>(owners.size(), rf));
    return owners;
}

bool ObjectStore::Placement::owns(const std::string& node, const std::string& bucket,
                                  const std::string& key) const {
    const std::vector<std::string> ranking = owners(bucket, key);
    return std::ranges::find(ranking, node) != ranking.end();
}

std::vector<std::string> ObjectStore::owners_of(const std::string& bucket,
                                                const std::string& key) const {
    return placement(bucket).owners(bucket, key);
}

// ---- buckets --------------------------------------------------------------------------------

std::optional<ObjectBucket> ObjectStore::find_bucket(const std::string& name) const {
    const auto view = replication_.shared_snapshot();
    const auto found = view->find(bucket_key(name));
    if (found == view->end()) return std::nullopt;
    return object_bucket_from_json(*found);
}

ObjectOutcome ObjectStore::create_bucket(const ObjectBucket& requested) {
    ObjectBucket bucket = requested;
    if (bucket.created_ms == 0) bucket.created_ms = now_ms();
    std::string reason;
    if (!object_bucket_from_json(to_json(bucket), &reason)) {
        const std::size_t colon = reason.find(':');
        std::string path = colon == std::string::npos ? "" : reason.substr(0, colon);
        if (path == "/name") path = "/bucket";
        return failure(ObjectStatus::InvalidInput, reason, path);
    }
    ObjectOutcome outcome;
    outcome.owner = node_id_;
    bool conflict = false;
    std::optional<ObjectBucket> existing;
    const bool created = replication_.update(
        bucket_key(bucket.name),
        [&](const nlohmann::json* current) -> std::optional<std::vector<ReplicationWrite>> {
            if (current) {
                existing = object_bucket_from_json(*current);
                conflict = !existing || existing->quota_bytes != bucket.quota_bytes ||
                           existing->rf != bucket.rf;
                return std::nullopt;
            }
            return std::vector<ReplicationWrite>{{bucket_key(bucket.name), to_json(bucket)}};
        });
    if (conflict) {
        outcome = failure(ObjectStatus::Conflict,
                          "bucket '" + bucket.name + "' already exists with a different definition",
                          "/bucket");
        outcome.bucket = existing;
        return outcome;
    }
    outcome.created = created;
    outcome.bucket = created ? bucket : existing;
    if (!bucket_state(bucket.name, /*create=*/true)) {
        return failure(ObjectStatus::Unavailable,
                       "the bucket's object log could not be opened on " + node_id_);
    }
    // Every live peer learns the definition before this answers.
    const std::vector<std::string> peers = live_peers();
    outcome.replicas_expected = peers.size();
    if (zetta::cluster::Cluster* cluster = cluster_.load(); cluster && !peers.empty()) {
        const auto responses = cluster->request_each(
            peers, std::string(kDefineMethod),
            encode_payload({{"definition", to_json(*outcome.bucket)}}), kReplicateTimeout);
        outcome.replicas = static_cast<std::size_t>(
            std::ranges::count_if(responses, [](const SignalResponse& r) { return r.ok; }));
    }
    return outcome;
}

// ---- leases ----------------------------------------------------------------------------------

std::optional<ObjectLease> ObjectStore::lease(const std::string& bucket) const {
    std::optional<ObjectLease> newest;
    {
        const auto view = replication_.shared_snapshot();
        if (const auto found = view->find(lease_key(bucket)); found != view->end()) {
            newest = object_lease_from_json(bucket, *found);
        }
    }
    const std::shared_lock buckets(buckets_mutex_);
    if (const auto found = buckets_.find(bucket); found != buckets_.end()) {
        const std::shared_lock lock(found->second->fence_mutex);
        const auto& held = found->second->lease;
        if (held && (!newest || held->version > newest->version)) newest = held;
    }
    return newest;
}

void ObjectStore::watch_lease(const std::string& bucket, std::function<void()> wake) {
    Bucket* state = bucket_state(bucket, /*create=*/true);
    if (!state) return;
    const std::lock_guard lock(state->watch_mutex);
    constexpr std::size_t kMaxWatchers = 4096;
    if (state->watchers.size() >= kMaxWatchers) {
        // Waiters whose wait already ended leave their wake behind; the oldest go.
        state->watchers.erase(state->watchers.begin(),
                              state->watchers.begin() + kMaxWatchers / 2);
    }
    state->watchers.push_back(std::move(wake));
}

void ObjectStore::install_lease(const ObjectLease& lease) {
    Bucket* state = bucket_state(lease.bucket, /*create=*/true);
    if (!state) return;
    {
        const std::unique_lock lock(state->fence_mutex);
        if (state->lease && state->lease->version >= lease.version) return;
        state->lease = lease;
    }
    std::vector<std::function<void()>> wake;
    {
        const std::lock_guard lock(state->watch_mutex);
        wake.swap(state->watchers);
    }
    for (const auto& watcher : wake) watcher();
}

ObjectOutcome ObjectStore::lease_decide(const nlohmann::json& request) {
    const std::lock_guard serialize(lease_mutex_);
    const std::string bucket = request.at("bucket").get<std::string>();
    const std::string op = request.at("op").get<std::string>();
    const std::string holder = request.value("holder", std::string());
    const std::uint64_t fence = request.value("fence", std::uint64_t{0});
    const std::int64_t ttl = request.value("ttl_ms", std::int64_t{0});
    if (!find_bucket(bucket)) {
        return failure(ObjectStatus::NotFound, "no bucket '" + bucket + "'", "/bucket");
    }
    const std::int64_t now = now_ms();
    const std::optional<ObjectLease> current = lease(bucket);
    const bool held = current && !current->holder.empty() && current->expires_ms > now;
    ObjectLease next;
    next.bucket = bucket;
    if (op == "acquire") {
        if (held && current->holder != holder) {
            ObjectOutcome outcome = failure(
                ObjectStatus::Held,
                "bucket '" + bucket + "' is held by '" + current->holder + "' until " +
                    std::to_string(current->expires_ms));
            outcome.lease = current;
            return outcome;
        }
        next.holder = holder;
        // The same holder re-acquiring a lease it still holds keeps its fence;
        // anyone else, or a holder whose lease lapsed, gets a new one.
        next.fence = held ? current->fence : (current ? current->fence : 0) + 1;
        next.expires_ms = now + ttl;
    } else {
        if (!current || current->fence != fence ||
            (!current->holder.empty() && current->holder != holder) ||
            (op == "renew" && current->holder.empty())) {
            ObjectOutcome outcome = failure(
                ObjectStatus::Fenced,
                current ? "fence " + std::to_string(fence) + " is not bucket '" + bucket +
                              "''s current lease (fence " + std::to_string(current->fence) +
                              (current->holder.empty() ? ", released"
                                                       : ", held by '" + current->holder + "'") +
                              ")"
                        : "bucket '" + bucket + "' has no lease",
                "/fence");
            outcome.fence = current ? current->fence : 0;
            outcome.lease = current;
            return outcome;
        }
        next = *current;
        if (op == "renew") {
            next.expires_ms = now + ttl;
        } else {
            if (current->holder.empty()) {  // released already: nothing changes
                ObjectOutcome outcome;
                outcome.lease = current;
                return outcome;
            }
            next.holder.clear();
            next.expires_ms = 0;
        }
    }
    next.version = std::max((current ? current->version : 0) + 1, now_micros());
    replication_.set(lease_key(bucket), to_json(next));
    install_lease(next);
    ObjectOutcome outcome;
    outcome.lease = next;
    outcome.owner = node_id_;
    return outcome;
}

ObjectOutcome ObjectStore::lease_call(const nlohmann::json& request) {
    const std::string bucket = request.at("bucket").get<std::string>();
    const std::string owner =
        ranked(placement_candidates(replication_, node_id_, live_peers()), bucket,
               std::string(kLeaseKey))
            .front();
    ObjectOutcome outcome;
    zetta::cluster::Cluster* cluster = cluster_.load();
    if (owner == node_id_) {
        outcome = lease_decide(request);
    } else {
        if (!cluster) return failure(ObjectStatus::Unavailable, "no cluster to reach " + owner);
        const SignalResponse response = cluster->request(
            owner, std::string(kLeaseMethod), encode_payload(request), kForwardTimeout);
        if (!response.ok) {
            return failure(ObjectStatus::Unavailable,
                           "the bucket's lease node " + owner + " did not answer: " +
                               response.error + "; retry");
        }
        nlohmann::json header;
        std::string_view bytes;
        std::optional<ObjectOutcome> parsed;
        if (decode_payload(response.payload, header, bytes, false, nullptr)) {
            parsed = outcome_from_json(header, bucket);
        }
        if (!parsed) {
            return failure(ObjectStatus::Unavailable,
                           "the bucket's lease node " + owner + " sent an unreadable answer; retry");
        }
        outcome = std::move(*parsed);
        outcome.owner = owner;
        if (outcome.ok() && outcome.lease) install_lease(*outcome.lease);
    }
    if (!outcome.ok() || !outcome.lease) return outcome;
    // Every live node installs the new lease before this answers, so no node
    // still orders writes under the old fence once a new holder has it.
    std::vector<std::string> peers = live_peers();
    std::erase(peers, owner);
    outcome.replicas_expected = peers.size();
    if (cluster && !peers.empty()) {
        const auto responses = cluster->request_each(
            peers, std::string(kLeaseApplyMethod),
            encode_payload({{"bucket", bucket}, {"lease", to_json(*outcome.lease)}}),
            kReplicateTimeout);
        outcome.replicas = static_cast<std::size_t>(
            std::ranges::count_if(responses, [](const SignalResponse& r) { return r.ok; }));
        if (outcome.replicas < peers.size()) {
            ObjectOutcome unavailable = failure(
                ObjectStatus::Unavailable,
                "the lease changed but " + std::to_string(peers.size() - outcome.replicas) +
                    " live node(s) did not confirm it; retry");
            unavailable.lease = outcome.lease;
            return unavailable;
        }
    }
    return outcome;
}

ObjectOutcome ObjectStore::acquire(const std::string& bucket, const std::string& holder,
                                   std::int64_t ttl_ms) {
    if (!valid_bucket_name(bucket)) return failure(ObjectStatus::InvalidInput, "want a bucket name", "/bucket");
    if (!valid_holder(holder)) {
        return failure(ObjectStatus::InvalidInput,
                       "want a holder of 1 to 256 bytes with no control characters", "/holder");
    }
    if (ttl_ms < 1 || ttl_ms > kMaxLeaseTtlMs) {
        return failure(ObjectStatus::InvalidInput, "want ttl_ms from 1 to 86400000", "/ttl_ms");
    }
    return lease_call({{"op", "acquire"}, {"bucket", bucket}, {"holder", holder}, {"ttl_ms", ttl_ms}});
}

ObjectOutcome ObjectStore::renew(const std::string& bucket, const std::string& holder,
                                 std::uint64_t fence, std::int64_t ttl_ms) {
    if (!valid_bucket_name(bucket)) return failure(ObjectStatus::InvalidInput, "want a bucket name", "/bucket");
    if (!valid_holder(holder)) {
        return failure(ObjectStatus::InvalidInput,
                       "want a holder of 1 to 256 bytes with no control characters", "/holder");
    }
    if (ttl_ms < 1 || ttl_ms > kMaxLeaseTtlMs) {
        return failure(ObjectStatus::InvalidInput, "want ttl_ms from 1 to 86400000", "/ttl_ms");
    }
    return lease_call({{"op", "renew"}, {"bucket", bucket}, {"holder", holder}, {"fence", fence},
                       {"ttl_ms", ttl_ms}});
}

ObjectOutcome ObjectStore::release(const std::string& bucket, const std::string& holder,
                                   std::uint64_t fence) {
    if (!valid_bucket_name(bucket)) return failure(ObjectStatus::InvalidInput, "want a bucket name", "/bucket");
    if (!valid_holder(holder)) {
        return failure(ObjectStatus::InvalidInput,
                       "want a holder of 1 to 256 bytes with no control characters", "/holder");
    }
    return lease_call({{"op", "release"}, {"bucket", bucket}, {"holder", holder}, {"fence", fence}});
}

// ---- mutations ---------------------------------------------------------------------------------

std::uint64_t ObjectStore::bucket_bytes(Bucket& state, const std::string& bucket) const {
    std::uint64_t held = state.log->object_bytes() + state.log->staged_bytes();
    const std::vector<std::string> peers = live_peers();
    if (peers.empty()) return held;
    {
        const std::lock_guard lock(state.peers_mutex);
        for (const std::string& peer : peers) {
            if (const auto found = state.peer_held.find(peer); found != state.peer_held.end()) {
                held += found->second;
            }
        }
    }
    // Each object is held by rf nodes: the bucket's bytes are what all of them
    // hold, divided by rf. Peers' figures are as of the last anti-entropy round.
    const std::size_t nodes =
        placement_candidates(replication_, node_id_, peers).size();
    const std::uint64_t rf = std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(replication_factor(bucket), nodes));
    return held / rf;
}

ObjectOutcome ObjectStore::decide(const Mutation& mutation, std::string_view bytes) {
    const std::optional<ObjectBucket> definition = find_bucket(mutation.bucket);
    if (!definition) {
        return failure(ObjectStatus::NotFound, "no bucket '" + mutation.bucket + "'", "/bucket");
    }
    Bucket* state = bucket_state(mutation.bucket, /*create=*/true);
    if (!state) {
        return failure(ObjectStatus::Unavailable,
                       "the bucket's object log could not be opened on " + node_id_);
    }
    if (state->log->failed()) return failed_log(mutation.bucket, node_id_);
    const std::shared_lock fenced(state->fence_mutex);
    // The fence: once the bucket has had a holder, every mutation carries the
    // current fence. Read through lease() so a restart sees the catalogue's.
    std::uint64_t current = state->lease ? state->lease->fence : 0;
    {
        const auto view = replication_.shared_snapshot();
        if (const auto found = view->find(lease_key(mutation.bucket)); found != view->end()) {
            if (const auto recorded = object_lease_from_json(mutation.bucket, *found)) {
                if (!state->lease || recorded->version > state->lease->version) {
                    current = recorded->fence;
                }
            }
        }
    }
    if (current != 0 || mutation.options.fence) {
        const std::uint64_t presented = mutation.options.fence.value_or(0);
        if (presented != current) {
            ObjectOutcome outcome = failure(
                ObjectStatus::Fenced,
                current == 0 ? "bucket '" + mutation.bucket + "' has never had a lease holder; "
                                   "send no fence"
                : !mutation.options.fence
                    ? "bucket '" + mutation.bucket + "' has a lease holder; send its fence (" +
                          std::to_string(current) + ") as Zetta-Fence"
                    : "fence " + std::to_string(presented) + " is not bucket '" +
                          mutation.bucket + "''s current fence " + std::to_string(current) +
                          ": a newer holder has the lease",
                "/fence");
            outcome.fence = current;
            return outcome;
        }
    }
    // The quota: refuse a write that would take the bucket past it.
    if (definition->quota_bytes && mutation.op != Mutation::Op::Remove) {
        const std::optional<ObjectVersion> held = state->log->stat(mutation.key);
        const std::uint64_t existing = held && !held->deleted ? held->size : 0;
        std::uint64_t grows = 0;
        switch (mutation.op) {
            case Mutation::Op::Append:
                grows = mutation.offset + bytes.size() > existing
                            ? mutation.offset + bytes.size() - existing
                            : 0;
                break;
            case Mutation::Op::Put: grows = bytes.size() > existing ? bytes.size() - existing : 0; break;
            case Mutation::Op::Commit:
                // The upload's parts already count as staged bytes.
                grows = 0;
                break;
            case Mutation::Op::Remove: break;
        }
        const std::uint64_t used = bucket_bytes(*state, mutation.bucket);
        if (grows > 0 && used + grows > *definition->quota_bytes) {
            return failure(ObjectStatus::QuotaExceeded,
                           "bucket '" + mutation.bucket + "' holds " + std::to_string(used) +
                               " bytes of its " + std::to_string(*definition->quota_bytes) +
                               "-byte quota; this write needs " + std::to_string(grows) + " more");
        }
    }
    const std::uint64_t fresh = now_micros();
    const bool sync = mutation.options.durable;
    ObjectLog::Written written;
    switch (mutation.op) {
        case Mutation::Op::Append:
            written = state->log->append(mutation.key, mutation.offset, bytes, fresh, sync);
            break;
        case Mutation::Op::Put:
            written = state->log->put(mutation.key, bytes, fresh, mutation.if_absent, sync);
            if (written.result == ObjectLog::Result::Conflict) {
                ObjectOutcome outcome = from_written(written, node_id_, mutation.key);
                outcome.message = "object '" + mutation.key + "' already exists";
                outcome.path = "/if_absent";
                return outcome;
            }
            break;
        case Mutation::Op::Commit:
            written = state->log->commit(mutation.key, mutation.upload, mutation.size, fresh,
                                         mutation.if_absent, sync);
            if (written.result == ObjectLog::Result::Conflict) {
                ObjectOutcome outcome = from_written(written, node_id_, mutation.key);
                outcome.message = "object '" + mutation.key + "' already exists";
                outcome.path = "/if_absent";
                return outcome;
            }
            if (written.result == ObjectLog::Result::NotFound) {
                return failure(ObjectStatus::Unavailable,
                               "owner " + node_id_ + " does not hold every part of the upload; "
                               "retry the put");
            }
            break;
        case Mutation::Op::Remove:
            written = state->log->remove(mutation.key, fresh, sync);
            break;
    }
    return from_written(written, node_id_, mutation.key);
}

ObjectOutcome ObjectStore::install(const Mutation& mutation, std::string_view bytes) {
    Bucket* state = bucket_state(mutation.bucket, /*create=*/true);
    if (!state) {
        return failure(ObjectStatus::Unavailable,
                       "the bucket's object log could not be opened on " + node_id_);
    }
    if (state->log->failed()) return failed_log(mutation.bucket, node_id_);
    const bool sync = mutation.options.durable;
    ObjectLog::Written written;
    switch (mutation.op) {
        case Mutation::Op::Append:
            written = state->log->apply_append(mutation.key, mutation.generation, mutation.offset,
                                               bytes, sync);
            break;
        case Mutation::Op::Put:
            written = state->log->apply_put(mutation.key, mutation.generation, bytes, sync);
            break;
        case Mutation::Op::Commit:
            written = state->log->apply_commit(mutation.key, mutation.upload, mutation.generation,
                                               mutation.size, sync);
            if (written.result == ObjectLog::Result::NotFound) {
                return failure(ObjectStatus::Unavailable,
                               "owner " + node_id_ + " does not hold every part of the upload");
            }
            break;
        case Mutation::Op::Remove:
            written = state->log->apply_remove(mutation.key, mutation.generation,
                                               mutation.updated_ms, sync);
            break;
    }
    return from_written(written, node_id_, mutation.key);
}

ObjectOutcome ObjectStore::mutate(const Mutation& mutation, std::string_view bytes) {
    const std::vector<std::string> owners = owners_of(mutation.bucket, mutation.key);
    const std::string& primary = owners.front();
    zetta::cluster::Cluster* cluster = cluster_.load();
    ObjectOutcome outcome;
    if (primary == node_id_) {
        outcome = decide(mutation, bytes);
    } else {
        if (!cluster) return failure(ObjectStatus::Unavailable, "no cluster to reach " + primary);
        const SignalResponse response =
            cluster->request(primary, std::string(kWriteMethod),
                             encode_payload(mutation.header(), bytes), kForwardTimeout);
        if (!response.ok) {
            return failure(ObjectStatus::Unavailable,
                           "the key's primary owner " + primary + " did not answer: " +
                               response.error + "; retry");
        }
        nlohmann::json header;
        std::string_view unused;
        std::optional<ObjectOutcome> parsed;
        if (decode_payload(response.payload, header, unused, false, nullptr)) {
            parsed = outcome_from_json(header, mutation.bucket);
        }
        if (!parsed || (parsed->ok() && !parsed->object)) {
            return failure(ObjectStatus::Unavailable,
                           "the key's primary owner " + primary + " sent an unreadable answer; retry");
        }
        outcome = std::move(*parsed);
    }
    outcome.owner = primary;
    outcome.replicas_expected = owners.size();
    if (!outcome.ok()) return outcome;
    outcome.replicas = 1;
    if (owners.size() == 1) return outcome;

    // Install the decided version on the other owners.
    Mutation decided = mutation;
    decided.generation = outcome.object->generation;
    decided.updated_ms = outcome.object->updated_ms;
    decided.options.fence.reset();
    std::vector<std::string> remote;
    for (std::size_t index = 1; index < owners.size(); ++index) {
        if (owners[index] == node_id_) {
            if (install(decided, bytes).ok()) ++outcome.replicas;
        } else {
            remote.push_back(owners[index]);
        }
    }
    if (!remote.empty() && cluster) {
        const auto responses = cluster->request_each(
            remote, std::string(kApplyMethod), encode_payload(decided.header(), bytes),
            kReplicateTimeout);
        for (const SignalResponse& response : responses) {
            if (!response.ok) continue;
            nlohmann::json header;
            std::string_view unused;
            if (decode_payload(response.payload, header, unused, false, nullptr) &&
                header.value("status", std::string()) == "ok") {
                ++outcome.replicas;
            }
        }
    }
    if (outcome.replicas < outcome.replicas_expected) {
        ObjectOutcome partial = failure(
            ObjectStatus::Unavailable,
            "the write is on " + std::to_string(outcome.replicas) + " of its " +
                std::to_string(outcome.replicas_expected) +
                " owners; retry (a retried append of the same bytes changes nothing)");
        partial.object = outcome.object;
        partial.owner = outcome.owner;
        partial.replicas = outcome.replicas;
        partial.replicas_expected = outcome.replicas_expected;
        return partial;
    }
    return outcome;
}

namespace {

ObjectOutcome check_target(const std::string& bucket, const std::string& key) {
    if (!valid_bucket_name(bucket)) {
        return failure(ObjectStatus::InvalidInput, "want a bucket name", "/bucket");
    }
    if (!valid_object_key(key)) {
        return failure(ObjectStatus::InvalidInput,
                       "want a key of 1 to 1024 bytes with no control characters", "/key");
    }
    return {};
}

}  // namespace

ObjectOutcome ObjectStore::put(const std::string& bucket, const std::string& key,
                               std::string_view bytes, const ObjectWrite& options,
                               bool if_absent) {
    if (ObjectOutcome checked = check_target(bucket, key); !checked.ok()) return checked;
    if (bytes.size() <= kSliceBytes) {
        Mutation mutation;
        mutation.op = Mutation::Op::Put;
        mutation.bucket = bucket;
        mutation.key = key;
        mutation.if_absent = if_absent;
        mutation.options = options;
        return mutate(mutation, bytes);
    }
    ObjectUpload upload;
    ObjectOutcome begun = begin_upload(bucket, key, options, bytes.size(), upload);
    if (!begun.ok()) return begun;
    for (std::size_t from = 0; from < bytes.size(); from += kSliceBytes) {
        ObjectOutcome part = upload_part(upload, bytes.substr(from, kSliceBytes));
        if (!part.ok()) {
            abort_upload(upload);
            return part;
        }
    }
    return commit_upload(upload, if_absent);
}

ObjectOutcome ObjectStore::put_range(const std::string& bucket, const std::string& key,
                                     std::uint64_t offset, std::string_view bytes,
                                     const ObjectWrite& options) {
    if (ObjectOutcome checked = check_target(bucket, key); !checked.ok()) return checked;
    ObjectOutcome outcome;
    std::size_t from = 0;
    bool unchanged = true;
    do {
        Mutation mutation;
        mutation.op = Mutation::Op::Append;
        mutation.bucket = bucket;
        mutation.key = key;
        mutation.offset = offset + from;
        mutation.options = options;
        const std::string_view slice = bytes.substr(from, kSliceBytes);
        outcome = mutate(mutation, slice);
        if (!outcome.ok()) return outcome;
        unchanged = unchanged && outcome.unchanged;
        from += slice.size();
    } while (from < bytes.size());
    outcome.unchanged = unchanged;
    return outcome;
}

ObjectOutcome ObjectStore::remove(const std::string& bucket, const std::string& key,
                                  const ObjectWrite& options) {
    if (ObjectOutcome checked = check_target(bucket, key); !checked.ok()) return checked;
    Mutation mutation;
    mutation.op = Mutation::Op::Remove;
    mutation.bucket = bucket;
    mutation.key = key;
    mutation.options = options;
    return mutate(mutation, {});
}

// ---- uploads ------------------------------------------------------------------------------------

ObjectOutcome ObjectStore::begin_upload(const std::string& bucket, const std::string& key,
                                        const ObjectWrite& options,
                                        std::optional<std::uint64_t> expected_size,
                                        ObjectUpload& upload) {
    if (ObjectOutcome checked = check_target(bucket, key); !checked.ok()) return checked;
    const std::optional<ObjectBucket> definition = find_bucket(bucket);
    if (!definition) return failure(ObjectStatus::NotFound, "no bucket '" + bucket + "'", "/bucket");
    Bucket* state = bucket_state(bucket, /*create=*/true);
    if (!state) {
        return failure(ObjectStatus::Unavailable,
                       "the bucket's object log could not be opened on " + node_id_);
    }
    if (state->log->failed()) return failed_log(bucket, node_id_);
    // Early refusals, before any byte moves: a stale fence and a body the quota
    // cannot take. The primary checks both again at the commit.
    if (const std::optional<ObjectLease> held = lease(bucket); held && held->fence != 0) {
        if (options.fence.value_or(0) != held->fence) {
            ObjectOutcome outcome = failure(
                ObjectStatus::Fenced,
                !options.fence ? "bucket '" + bucket + "' has a lease holder; send its fence (" +
                                     std::to_string(held->fence) + ") as Zetta-Fence"
                               : "fence " + std::to_string(*options.fence) + " is not bucket '" +
                                     bucket + "''s current fence " + std::to_string(held->fence),
                "/fence");
            outcome.fence = held->fence;
            return outcome;
        }
    }
    if (definition->quota_bytes && expected_size) {
        const std::optional<ObjectVersion> current = state->log->stat(key);
        const std::uint64_t replaced = current && !current->deleted ? current->size : 0;
        const std::uint64_t counted = bucket_bytes(*state, bucket);
        const std::uint64_t used = counted > replaced ? counted - replaced : 0;
        if (used + *expected_size > *definition->quota_bytes) {
            return failure(ObjectStatus::QuotaExceeded,
                           "bucket '" + bucket + "' holds " + std::to_string(used) +
                               " bytes of its " + std::to_string(*definition->quota_bytes) +
                               "-byte quota; this object is " + std::to_string(*expected_size));
        }
    }
    upload = ObjectUpload{};
    upload.id = random_id();
    upload.bucket = bucket;
    upload.key = key;
    upload.options = options;
    upload.owners = owners_of(bucket, key);
    return {};
}

ObjectOutcome ObjectStore::upload_part(ObjectUpload& upload, std::string_view bytes) {
    if (bytes.size() > kSliceBytes) {
        return failure(ObjectStatus::InvalidInput, "an upload part is at most 4 MiB");
    }
    const std::optional<ObjectBucket> definition = find_bucket(upload.bucket);
    if (!definition) {
        return failure(ObjectStatus::NotFound, "no bucket '" + upload.bucket + "'", "/bucket");
    }
    Bucket* state = bucket_state(upload.bucket, /*create=*/true);
    if (!state) {
        return failure(ObjectStatus::Unavailable,
                       "the bucket's object log could not be opened on " + node_id_);
    }
    if (definition->quota_bytes) {
        // A put replaces the key's current version, whose bytes it frees.
        const std::optional<ObjectVersion> current = state->log->stat(upload.key);
        const std::uint64_t replaced = current && !current->deleted ? current->size : 0;
        const std::uint64_t counted = bucket_bytes(*state, upload.bucket);
        const std::uint64_t used = counted > replaced ? counted - replaced : 0;
        if (used + bytes.size() > *definition->quota_bytes) {
            return failure(ObjectStatus::QuotaExceeded,
                           "bucket '" + upload.bucket + "' holds " + std::to_string(used) +
                               " bytes of its " + std::to_string(*definition->quota_bytes) +
                               "-byte quota; this object does not fit");
        }
    }
    // Every owner stages the part.
    std::vector<std::string> remote;
    std::size_t staged = 0;
    for (const std::string& owner : upload.owners) {
        if (owner != node_id_) {
            remote.push_back(owner);
            continue;
        }
        const ObjectLog::Written written = state->log->stage(
            upload.key, upload.id, upload.written, bytes, upload.options.durable);
        if (written.result == ObjectLog::Result::Ok || written.result == ObjectLog::Result::Unchanged) {
            ++staged;
        }
    }
    if (!remote.empty()) {
        zetta::cluster::Cluster* cluster = cluster_.load();
        if (!cluster) return failure(ObjectStatus::Unavailable, "no cluster to reach the owners");
        nlohmann::json header{{"bucket", upload.bucket},
                              {"key", upload.key},
                              {"upload", upload.id},
                              {"offset", upload.written}};
        if (upload.options.durable) header["durable"] = true;
        const auto responses = cluster->request_each(remote, std::string(kStageMethod),
                                                     encode_payload(header, bytes),
                                                     kReplicateTimeout);
        staged += static_cast<std::size_t>(
            std::ranges::count_if(responses, [](const SignalResponse& r) { return r.ok; }));
    }
    if (staged < upload.owners.size()) {
        return failure(ObjectStatus::Unavailable,
                       "an upload part reached " + std::to_string(staged) + " of " +
                           std::to_string(upload.owners.size()) + " owners; retry the put");
    }
    upload.written += bytes.size();
    return {};
}

ObjectOutcome ObjectStore::commit_upload(ObjectUpload& upload, bool if_absent) {
    // The placement the parts went to must still be the key's placement.
    if (owners_of(upload.bucket, upload.key) != upload.owners) {
        abort_upload(upload);
        return failure(ObjectStatus::Unavailable,
                       "the key's owners changed during the upload; retry the put");
    }
    Mutation mutation;
    mutation.op = Mutation::Op::Commit;
    mutation.bucket = upload.bucket;
    mutation.key = upload.key;
    mutation.upload = upload.id;
    mutation.size = upload.written;
    mutation.if_absent = if_absent;
    mutation.options = upload.options;
    ObjectOutcome outcome = mutate(mutation, {});
    if (!outcome.ok()) abort_upload(upload);
    return outcome;
}

void ObjectStore::abort_upload(ObjectUpload& upload) {
    if (upload.id == 0) return;
    std::vector<std::string> remote;
    for (const std::string& owner : upload.owners) {
        if (owner == node_id_) {
            if (Bucket* state = bucket_state(upload.bucket, /*create=*/false)) {
                state->log->abort(upload.id);
            }
        } else {
            remote.push_back(owner);
        }
    }
    if (zetta::cluster::Cluster* cluster = cluster_.load(); cluster && !remote.empty()) {
        cluster->request_each(remote, std::string(kAbortMethod),
                              encode_payload({{"bucket", upload.bucket}, {"upload", upload.id}}),
                              kReplicateTimeout);
    }
    upload.id = 0;
}

// ---- reads -----------------------------------------------------------------------------------------

ObjectOutcome ObjectStore::open_read(const std::string& bucket, const std::string& key,
                                     std::uint64_t offset, std::optional<std::uint64_t> length,
                                     std::shared_ptr<ObjectReader>& reader, bool* needs_peer) {
    if (ObjectOutcome checked = check_target(bucket, key); !checked.ok()) return checked;
    if (!find_bucket(bucket)) {
        return failure(ObjectStatus::NotFound, "no bucket '" + bucket + "'", "/bucket");
    }
    const auto out_of_range = [&](std::uint64_t size) {
        ObjectOutcome outcome = failure(
            ObjectStatus::OutOfRange,
            "the range is outside object '" + key + "' of " + std::to_string(size) + " bytes",
            "/range");
        outcome.object = ObjectInfo{key, size, 0, 0};
        return outcome;
    };
    const std::vector<std::string> owners = owners_of(bucket, key);
    const bool owner = std::ranges::find(owners, node_id_) != owners.end();
    Bucket* local = bucket_state(bucket, /*create=*/false);
    if (local && owner && local->log->failed()) {
        // This copy cannot be trusted; another owner's can.
        if (owners.size() == 1) return failed_log(bucket, node_id_);
        local = nullptr;
    }
    if (Bucket* state = local; state && owner) {
        ObjectLog::ReadSnapshot snapshot;
        switch (state->log->snapshot(key, offset, length, snapshot)) {
            case ObjectLog::ReadStatus::Ok: {
                ObjectOutcome outcome;
                outcome.object = info_of(snapshot.version);
                outcome.owner = node_id_;
                reader = std::make_shared<LocalReader>(std::move(snapshot));
                return outcome;
            }
            case ObjectLog::ReadStatus::OutOfRange: return out_of_range(snapshot.version.size);
            case ObjectLog::ReadStatus::NotFound:
                if (owners.size() == 1) {
                    return failure(ObjectStatus::NotFound,
                                   "no object '" + key + "' in '" + bucket + "'", "/key");
                }
                break;
        }
    }
    if (needs_peer) {
        *needs_peer = true;
        return failure(ObjectStatus::Unavailable, "the read needs a peer");
    }
    // Another owner's copy.
    zetta::cluster::Cluster* cluster = cluster_.load();
    bool unreachable = false;
    for (const std::string& peer : owners) {
        if (peer == node_id_) continue;
        if (!cluster) {
            unreachable = true;
            break;
        }
        nlohmann::json request{{"bucket", bucket}, {"key", key}, {"offset", offset}, {"probe", true}};
        if (length) request["length"] = *length;
        const SignalResponse response = cluster->request(
            peer, std::string(kReadMethod), encode_payload(request), kForwardTimeout);
        nlohmann::json header;
        std::string_view unused;
        if (!response.ok || !decode_payload(response.payload, header, unused, false, nullptr) ||
            !header.is_object()) {
            unreachable = true;
            continue;
        }
        const std::string status = header.value("status", std::string());
        const auto info = header.contains("object") ? info_from_json(header["object"]) : std::nullopt;
        if (status == "range_not_satisfiable" && info) return out_of_range(info->size);
        if (status != "ok" || !info) continue;
        const std::uint64_t resolved = length ? *length : info->size - offset;
        ObjectOutcome outcome;
        outcome.object = *info;
        outcome.owner = peer;
        reader = std::make_shared<RemoteReader>(*cluster, peer, bucket, *info, offset, resolved);
        return outcome;
    }
    if (unreachable) {
        return failure(ObjectStatus::Unavailable,
                       "an owner of '" + key + "' did not answer; retry");
    }
    return failure(ObjectStatus::NotFound, "no object '" + key + "' in '" + bucket + "'", "/key");
}

ObjectPage ObjectStore::list(const std::string& bucket, const std::string& prefix,
                             const std::string& after, std::size_t limit) {
    ObjectPage page;
    const auto fail = [&](ObjectStatus status, std::string message, std::string path) {
        page.status = status;
        page.message = std::move(message);
        page.path = std::move(path);
        return page;
    };
    if (!valid_bucket_name(bucket)) return fail(ObjectStatus::InvalidInput, "want a bucket name", "/bucket");
    if (!find_bucket(bucket)) return fail(ObjectStatus::NotFound, "no bucket '" + bucket + "'", "/bucket");
    limit = std::clamp<std::size_t>(limit, 1, kMaxListLimit);
    // Every node answers its first limit + 1 versions after `after`, tombstones
    // included, and the highest generation of a key wins: an owner's removal
    // outranks a copy a node has not dropped yet. A node that had more than it
    // sent bounds what the merge knows for certain (the horizon); the page stops
    // there and `next_after` continues from it.
    struct Version {
        ObjectInfo info;
        bool deleted = false;
    };
    std::map<std::string, Version> merged;
    std::optional<std::string> horizon;
    const auto take = [&](ObjectInfo info, bool deleted) {
        auto [slot, inserted] = merged.try_emplace(info.key, Version{info, deleted});
        if (!inserted && info.generation > slot->second.info.generation) {
            slot->second = Version{std::move(info), deleted};
        }
    };
    const auto bound = [&](const std::string& last) {
        if (!horizon || last < *horizon) horizon = last;
    };
    if (Bucket* state = bucket_state(bucket, /*create=*/false)) {
        if (state->log->failed()) {
            const ObjectOutcome failed = failed_log(bucket, node_id_);
            return fail(failed.status, failed.message, "");
        }
        std::size_t count = 0;
        std::string last;
        state->log->list(
            prefix, after,
            [&](const ObjectVersion& version) {
                take(info_of(version), version.deleted);
                last = version.key;
                return ++count <= limit;
            },
            /*tombstones=*/true);
        if (count > limit) bound(last);
    }
    const std::vector<std::string> peers = live_peers();
    if (zetta::cluster::Cluster* cluster = cluster_.load(); cluster && !peers.empty()) {
        const nlohmann::json request{{"bucket", bucket}, {"prefix", prefix}, {"after", after},
                                     {"limit", limit + 1}};
        const auto responses = cluster->request_each(peers, std::string(kListMethod),
                                                     encode_payload(request), kForwardTimeout);
        for (std::size_t index = 0; index < responses.size(); ++index) {
            nlohmann::json header;
            std::string_view unused;
            if (!responses[index].ok ||
                !decode_payload(responses[index].payload, header, unused, false, nullptr) ||
                !header.is_array()) {
                return fail(ObjectStatus::Unavailable,
                            "node " + peers[index] + " did not answer the listing; retry", "");
            }
            std::string last;
            for (const nlohmann::json& entry : header) {
                auto info = info_from_json(entry);
                if (!info || !info->key.starts_with(prefix) || info->key <= after) continue;
                last = std::max(last, info->key);
                take(std::move(*info), entry.value("d", false));
            }
            if (header.size() > limit) bound(last);
        }
    }
    for (auto& [key, version] : merged) {
        if (horizon && key > *horizon) break;
        if (version.deleted) continue;
        if (page.objects.size() == limit) {
            page.next_after = page.objects.back().key;
            return page;
        }
        page.objects.push_back(std::move(version.info));
    }
    if (horizon) page.next_after = page.objects.empty() || page.objects.back().key < *horizon
                                       ? *horizon
                                       : page.objects.back().key;
    return page;
}

ObjectOutcome ObjectStore::barrier(const std::string& bucket) {
    if (!valid_bucket_name(bucket)) return failure(ObjectStatus::InvalidInput, "want a bucket name", "/bucket");
    if (!find_bucket(bucket)) return failure(ObjectStatus::NotFound, "no bucket '" + bucket + "'", "/bucket");
    ObjectOutcome outcome;
    if (Bucket* state = bucket_state(bucket, /*create=*/false); state && !state->log->sync()) {
        return failure(ObjectStatus::Unavailable, "the bucket could not be synced on " + node_id_);
    }
    outcome.nodes = 1;
    const std::vector<std::string> peers = live_peers();
    if (zetta::cluster::Cluster* cluster = cluster_.load(); cluster && !peers.empty()) {
        const auto responses = cluster->request_each(peers, std::string(kBarrierMethod),
                                                     encode_payload({{"bucket", bucket}}),
                                                     kReplicateTimeout);
        for (const SignalResponse& response : responses) {
            if (response.ok) ++outcome.nodes;
        }
        if (outcome.nodes < peers.size() + 1) {
            ObjectOutcome partial = failure(
                ObjectStatus::Unavailable,
                std::to_string(peers.size() + 1 - outcome.nodes) +
                    " node(s) did not confirm the barrier; retry");
            partial.nodes = outcome.nodes;
            return partial;
        }
    }
    return outcome;
}

ObjectUsage ObjectStore::usage(const std::string& bucket) {
    ObjectUsage usage;
    if (!valid_bucket_name(bucket)) {
        usage.status = ObjectStatus::InvalidInput;
        usage.message = "want a bucket name";
        return usage;
    }
    const std::optional<ObjectBucket> definition = find_bucket(bucket);
    if (!definition) {
        usage.status = ObjectStatus::NotFound;
        usage.message = "no bucket '" + bucket + "'";
        return usage;
    }
    usage.quota_bytes = definition->quota_bytes;
    const std::vector<std::string> peers = live_peers();
    Bucket* state = bucket_state(bucket, /*create=*/false);
    if (state && state->log->failed()) {
        const ObjectOutcome failed = failed_log(bucket, node_id_);
        usage.status = failed.status;
        usage.message = failed.message;
        return usage;
    }
    if (peers.empty()) {
        if (state) {
            usage.objects = state->log->object_count();
            usage.bytes = state->log->object_bytes();
        }
        return usage;
    }
    // Each node counts the objects it is the primary owner of.
    const Placement view = placement(bucket);
    const auto count_primary = [&](std::uint64_t& objects, std::uint64_t& bytes) {
        if (!state) return;
        state->log->list("", "", [&](const ObjectVersion& version) {
            if (view.owners(bucket, version.key).front() == node_id_) {
                ++objects;
                bytes += version.size;
            }
            return true;
        });
    };
    count_primary(usage.objects, usage.bytes);
    if (zetta::cluster::Cluster* cluster = cluster_.load()) {
        const auto responses = cluster->request_each(peers, std::string(kUsageMethod),
                                                     encode_payload({{"bucket", bucket}}),
                                                     kForwardTimeout);
        for (std::size_t index = 0; index < responses.size(); ++index) {
            nlohmann::json header;
            std::string_view unused;
            if (!responses[index].ok ||
                !decode_payload(responses[index].payload, header, unused, false, nullptr) ||
                !header.is_object() || !header.value("objects", nlohmann::json()).is_number_unsigned() ||
                !header.value("bytes", nlohmann::json()).is_number_unsigned()) {
                usage.status = ObjectStatus::Unavailable;
                usage.message = "node " + peers[index] + " did not answer; retry";
                return usage;
            }
            usage.objects += header["objects"].get<std::uint64_t>();
            usage.bytes += header["bytes"].get<std::uint64_t>();
        }
    }
    return usage;
}

// ---- upkeep --------------------------------------------------------------------------------------------

void ObjectStore::maintain(std::int64_t now) {
    std::vector<Bucket*> states;
    {
        const std::shared_lock lock(buckets_mutex_);
        for (const auto& [name, state] : buckets_) states.push_back(state.get());
    }
    for (Bucket* state : states) {
        state->log->expire_uploads(now - kUploadIdleMs);
        state->log->maintain(now);
    }
}

std::vector<ObjectLogFailure> ObjectStore::failed_logs() const {
    std::vector<ObjectLogFailure> failures;
    const std::shared_lock lock(buckets_mutex_);
    for (const auto& [name, state] : buckets_) {
        const ObjectLog::Health health = state->log->health();
        if (health.failed) failures.push_back({name, health.stopped, health.reopen_attempts});
    }
    return failures;
}

void ObjectStore::set_fsync(Fsync fsync) {
    fsync_ = std::move(fsync);
    const std::shared_lock lock(buckets_mutex_);
    for (const auto& [name, state] : buckets_) state->log->set_fsync(fsync_);
}

ObjectStorageStats ObjectStore::storage_stats() const {
    ObjectStorageStats stats;
    const std::shared_lock lock(buckets_mutex_);
    stats.buckets = buckets_.size();
    for (const auto& [name, state] : buckets_) {
        const ObjectLog::Health health = state->log->health();
        if (health.failed) ++stats.failed_logs;
        if (health.stopped) ++stats.stopped_logs;
        const ObjectLog::Stats log = state->log->stats();
        stats.objects += log.objects;
        stats.object_bytes += state->log->object_bytes();
        stats.tombstones += log.tombstones;
        stats.segments += log.segments;
        stats.disk_bytes += log.disk_bytes;
        stats.live_bytes += log.live_bytes;
        stats.memory_bytes += log.memory_bytes;
    }
    return stats;
}

std::size_t ObjectStore::pull_from(const std::string& peer) {
    zetta::cluster::Cluster* cluster = cluster_.load();
    if (!cluster) return 0;
    const auto ask = [&](std::string_view method, const nlohmann::json& request,
                         std::string* bytes = nullptr) -> std::optional<nlohmann::json> {
        const SignalResponse response = cluster->request(peer, std::string(method),
                                                         encode_payload(request), kAntiEntropyTimeout);
        if (!response.ok) return std::nullopt;
        nlohmann::json header;
        std::string_view body;
        if (!decode_payload(response.payload, header, body, false, nullptr)) return std::nullopt;
        if (bytes) bytes->assign(body);
        return header;
    };
    const auto theirs = ask(kDigestMethod, {{"for", node_id_}});
    if (!theirs || !theirs->is_object()) return 0;
    const std::int64_t now = now_ms();
    std::size_t pulled = 0;
    for (const auto& [bucket, summary] : theirs->items()) {
        if (!valid_bucket_name(bucket) || !summary.is_object()) continue;
        Bucket* state = bucket_state(bucket, /*create=*/true);
        if (!state || state->log->failed()) continue;
        if (const auto held = summary.find("held"); held != summary.end() && held->is_number_unsigned()) {
            const std::lock_guard lock(state->peers_mutex);
            state->peer_held[peer] = held->get<std::uint64_t>();
        }
        const auto digests = summary.find("d");
        if (digests == summary.end() || !digests->is_array() ||
            digests->size() != ObjectLog::kBuckets) {
            continue;
        }
        const Placement view = placement(bucket);
        const auto mine_only = [&](const std::string& key) {
            return view.owns(node_id_, bucket, key);
        };
        const auto mine = state->log->digests(mine_only);
        std::vector<std::size_t> differing;
        for (std::size_t index = 0; index < ObjectLog::kBuckets; ++index) {
            if ((*digests)[index].is_number_unsigned() &&
                (*digests)[index].get<std::uint64_t>() != mine[index]) {
                differing.push_back(index);
            }
        }
        for (std::size_t start = 0; start < differing.size(); start += kBucketsPerRequest) {
            const std::vector<std::size_t> slots(
                differing.begin() + static_cast<std::ptrdiff_t>(start),
                differing.begin() + static_cast<std::ptrdiff_t>(
                                        std::min(differing.size(), start + kBucketsPerRequest)));
            const auto versions =
                ask(kKeysMethod, {{"bucket", bucket}, {"buckets", slots}, {"for", node_id_}});
            if (!versions || !versions->is_array()) continue;
            for (const nlohmann::json& entry : *versions) {
                const auto info = info_from_json(entry);
                if (!info || !valid_object_key(info->key) || !mine_only(info->key)) continue;
                const bool deleted = entry.value("d", false);
                const std::optional<ObjectVersion> local = state->log->stat(info->key);
                if (local && local->generation > info->generation) continue;
                if (local && local->generation == info->generation &&
                    (local->deleted || local->size >= info->size)) {
                    continue;
                }
                if (deleted) {
                    if (now - info->updated_ms >= kTombstoneGraceMs) continue;
                    if (state->log->apply_remove(info->key, info->generation, info->updated_ms, false)
                            .result == ObjectLog::Result::Ok) {
                        ++pulled;
                    }
                    continue;
                }
                // Pull the bytes: the missing tail of the same version, or the
                // whole of a newer one staged and then committed.
                const bool tail = local && !local->deleted && local->generation == info->generation;
                std::uint64_t from = tail ? local->size : 0;
                const std::uint64_t upload = tail ? 0 : random_id();
                bool whole = true;
                std::string bytes;
                while (from < info->size) {
                    const std::uint64_t take =
                        std::min<std::uint64_t>(info->size - from, kSliceBytes);
                    const auto read = ask(kReadMethod,
                                          {{"bucket", bucket},
                                           {"key", info->key},
                                           {"generation", info->generation},
                                           {"offset", from},
                                           {"length", take}},
                                          &bytes);
                    if (!read || read->value("status", std::string()) != "ok" ||
                        bytes.size() != take) {
                        whole = false;
                        break;
                    }
                    const ObjectLog::Written written =
                        tail ? state->log->apply_append(info->key, info->generation, from, bytes,
                                                        false)
                             : state->log->stage(info->key, upload, from, bytes, false);
                    if (written.result != ObjectLog::Result::Ok &&
                        written.result != ObjectLog::Result::Unchanged) {
                        whole = false;
                        break;
                    }
                    from += take;
                }
                if (tail) {
                    if (whole) ++pulled;
                    continue;
                }
                if (!whole) {
                    state->log->abort(upload);
                    continue;
                }
                if (info->size == 0) {
                    if (state->log->apply_put(info->key, info->generation, {}, false).result ==
                        ObjectLog::Result::Ok) {
                        ++pulled;
                    }
                    state->log->abort(upload);
                    continue;
                }
                if (state->log->apply_commit(info->key, upload, info->generation, info->size, false)
                        .result == ObjectLog::Result::Ok) {
                    ++pulled;
                } else {
                    state->log->abort(upload);
                }
            }
        }
    }
    return pulled;
}

std::size_t ObjectStore::evict_unowned() {
    zetta::cluster::Cluster* cluster = cluster_.load();
    if (!cluster || live_peers().empty()) return 0;
    std::vector<std::pair<std::string, Bucket*>> states;
    {
        const std::shared_lock lock(buckets_mutex_);
        for (const auto& [name, state] : buckets_) states.emplace_back(name, state.get());
    }
    std::size_t evicted = 0;
    for (const auto& [bucket, state] : states) {
        if (state->log->failed()) continue;
        // The copies this node holds but no longer owns, grouped by owner set.
        std::map<std::vector<std::string>, std::vector<ObjectVersion>> unowned;
        const Placement view = placement(bucket);
        state->log->each([&](const ObjectVersion& version) {
            std::vector<std::string> owners = view.owners(bucket, version.key);
            if (std::ranges::find(owners, node_id_) == owners.end()) {
                unowned[std::move(owners)].push_back(version);
            }
        });
        for (const auto& [owners, versions] : unowned) {
            for (std::size_t start = 0; start < versions.size(); start += kKeysPerRequest) {
                const auto end = std::min(versions.size(), start + kKeysPerRequest);
                std::vector<std::string> keys;
                for (std::size_t index = start; index < end; ++index) keys.push_back(versions[index].key);
                const auto responses = cluster->request_each(
                    owners, std::string(kStatMethod),
                    encode_payload({{"bucket", bucket}, {"keys", keys}}), kAntiEntropyTimeout);
                // Each owner's versions, by key.
                std::vector<std::map<std::string, std::pair<ObjectInfo, bool>>> held(owners.size());
                bool every_owner_answered = true;
                for (std::size_t index = 0; index < responses.size(); ++index) {
                    nlohmann::json header;
                    std::string_view unused;
                    if (!responses[index].ok ||
                        !decode_payload(responses[index].payload, header, unused, false, nullptr) ||
                        !header.is_array()) {
                        every_owner_answered = false;
                        break;
                    }
                    for (const nlohmann::json& entry : header) {
                        if (auto info = info_from_json(entry)) {
                            held[index][info->key] = {*info, entry.value("d", false)};
                        }
                    }
                }
                if (!every_owner_answered) continue;
                for (std::size_t index = start; index < end; ++index) {
                    const ObjectVersion& mine = versions[index];
                    const bool everywhere = std::ranges::all_of(held, [&](const auto& owner) {
                        const auto found = owner.find(mine.key);
                        if (found == owner.end()) return false;
                        const auto& [info, deleted] = found->second;
                        return info.generation > mine.generation ||
                               (info.generation == mine.generation &&
                                (deleted || info.size >= mine.size));
                    });
                    if (everywhere && state->log->purge(mine.key, mine.generation)) ++evicted;
                }
            }
        }
    }
    return evicted;
}

std::size_t ObjectStore::anti_entropy() {
    std::size_t moved = 0;
    for (const std::string& peer : live_peers()) moved += pull_from(peer);
    moved += evict_unowned();
    return moved;
}

// ---- the signal plane ----------------------------------------------------------------------------------

zetta::cluster::SignalResponse ObjectStore::on_request(const zetta::cluster::Signal& request) {
    static const std::vector<std::string_view> kMethods = {
        kWriteMethod, kApplyMethod, kStageMethod,  kAbortMethod, kBarrierMethod,
        kReadMethod,  kStatMethod,  kListMethod,   kUsageMethod, kDigestMethod,
        kKeysMethod,  kLeaseMethod, kLeaseApplyMethod, kDefineMethod};
    if (std::ranges::find(kMethods, std::string_view(request.method)) == kMethods.end()) {
        if (auto* delegate = delegate_.load()) return delegate->on_request(request);
        return {.ok = false, .payload = {}, .error = "no handler"};
    }
    nlohmann::json header;
    std::string_view bytes;
    std::string reason;
    if (!decode_payload(request.payload, header, bytes, /*validate=*/true, &reason)) {
        return {.ok = false, .payload = {}, .error = std::string(request.method) + ": " + reason};
    }
    const auto refuse = [&](std::string why) {
        return SignalResponse{.ok = false, .payload = {}, .error = std::string(request.method) + ": " + why};
    };
    const auto answer = [](const nlohmann::json& body, std::string_view data = {}) {
        return SignalResponse{.ok = true, .payload = encode_payload(body, data), .error = {}};
    };
    const std::string bucket = header.value("bucket", std::string());
    const std::string method = request.method;

    if (method == kDefineMethod) {
        std::string why;
        const auto definition =
            object_bucket_from_json(header.value("definition", nlohmann::json()), &why);
        if (!definition) return refuse(why);
        replication_.update(bucket_key(definition->name),
                            [&](const nlohmann::json* current)
                                -> std::optional<std::vector<ReplicationWrite>> {
                                if (current) return std::nullopt;  // the first definition stands
                                return std::vector<ReplicationWrite>{
                                    {bucket_key(definition->name), to_json(*definition)}};
                            });
        bucket_state(definition->name, /*create=*/true);
        return answer({{"status", "ok"}});
    }
    if (method == kDigestMethod) {
        const std::string requester = header.value("for", std::string());
        nlohmann::json result = nlohmann::json::object();
        std::vector<std::pair<std::string, Bucket*>> states;
        {
            const std::shared_lock lock(buckets_mutex_);
            for (const auto& [name, state] : buckets_) states.emplace_back(name, state.get());
        }
        for (const auto& [name, state] : states) {
            if (state->log->failed()) continue;  // it serves nothing until reopened
            const Placement view = placement(name);
            const auto digests = state->log->digests([&](const std::string& key) {
                return view.owns(requester, name, key);
            });
            result[name] = {{"d", digests}, {"held", state->log->object_bytes()}};
        }
        return answer(result);
    }
    if (method == kLeaseApplyMethod) {
        const auto lease = object_lease_from_json(bucket, header.value("lease", nlohmann::json()));
        if (!lease || bucket.empty()) return refuse("want a bucket and a lease");
        install_lease(*lease);
        return answer({{"status", "ok"}});
    }
    if (method == kLeaseMethod) {
        const std::string op = header.value("op", std::string());
        if (bucket.empty() || (op != "acquire" && op != "renew" && op != "release")) {
            return refuse("want a bucket and a lease op");
        }
        return answer(outcome_json(lease_decide(header)));
    }
    if (bucket.empty()) return refuse("want a bucket");
    Bucket* state = bucket_state(bucket, /*create=*/method == kApplyMethod || method == kStageMethod ||
                                             method == kWriteMethod);
    if (method == kWriteMethod || method == kApplyMethod) {
        const auto mutation = Mutation::from_header(header);
        if (!mutation || !valid_object_key(mutation->key)) return refuse("want a mutation");
        if (method == kApplyMethod) {
            if (mutation->generation == 0) return refuse("want a generation");
            return answer(outcome_json(install(*mutation, bytes)));
        }
        ObjectOutcome outcome;
        if (const auto owners = owners_of(mutation->bucket, mutation->key);
            owners.front() != node_id_) {
            // Two nodes disagree about membership right now; the caller retries.
            outcome = failure(ObjectStatus::Unavailable,
                              "ownership of this key is moving (this node sees " + owners.front() +
                                  " as its primary owner); retry");
        } else {
            outcome = decide(*mutation, bytes);
        }
        return answer(outcome_json(outcome));
    }
    if (!state) {
        if (method == kStatMethod || method == kListMethod) return answer(nlohmann::json::array());
        if (method == kUsageMethod) return answer({{"objects", 0}, {"bytes", 0}});
        if (method == kBarrierMethod || method == kAbortMethod) return answer({{"status", "ok"}});
        if (method == kReadMethod) return answer({{"status", "not_found"}});
        return refuse("no bucket '" + bucket + "' on this node");
    }
    if (state->log->failed() && method != kAbortMethod) {
        return refuse("the object log of bucket '" + bucket + "' failed an fsync; retry");
    }
    if (method == kStageMethod) {
        const std::string key = header.value("key", std::string());
        if (!valid_object_key(key) || !header.contains("upload")) return refuse("want a key and an upload");
        const ObjectLog::Written written =
            state->log->stage(key, header["upload"].get<std::uint64_t>(),
                              header.value("offset", std::uint64_t{0}), bytes,
                              header.value("durable", false));
        if (written.result != ObjectLog::Result::Ok && written.result != ObjectLog::Result::Unchanged) {
            return refuse("the part could not be staged");
        }
        return answer({{"status", "ok"}});
    }
    if (method == kAbortMethod) {
        state->log->abort(header.value("upload", std::uint64_t{0}));
        return answer({{"status", "ok"}});
    }
    if (method == kBarrierMethod) {
        if (!state->log->sync()) return refuse("the bucket could not be synced");
        return answer({{"status", "ok"}});
    }
    if (method == kReadMethod) {
        const std::string key = header.value("key", std::string());
        if (!valid_object_key(key)) return refuse("want a key");
        const std::uint64_t offset = header.value("offset", std::uint64_t{0});
        ObjectLog::ReadSnapshot snapshot;
        ObjectLog::ReadStatus status;
        if (header.contains("generation")) {
            status = state->log->snapshot_generation(key, header["generation"].get<std::uint64_t>(),
                                                     offset, header.value("length", std::uint64_t{0}),
                                                     snapshot);
        } else {
            std::optional<std::uint64_t> length;
            if (header.contains("length")) length = header["length"].get<std::uint64_t>();
            status = state->log->snapshot(key, offset, length, snapshot);
        }
        if (status == ObjectLog::ReadStatus::NotFound) return answer({{"status", "not_found"}});
        if (status == ObjectLog::ReadStatus::OutOfRange) {
            return answer({{"status", "range_not_satisfiable"},
                           {"object", info_json(info_of(snapshot.version))}});
        }
        const nlohmann::json result{{"status", "ok"}, {"object", info_json(info_of(snapshot.version))}};
        if (header.value("probe", false)) return answer(result);
        if (snapshot.length > kSliceBytes) return refuse("a read is at most 4 MiB");
        std::string data;
        if (!snapshot.read_into(0, snapshot.length, data)) return refuse("the bytes could not be read");
        return answer(result, data);
    }
    if (method == kStatMethod) {
        nlohmann::json versions = nlohmann::json::array();
        for (const auto& key : header.value("keys", std::vector<std::string>{})) {
            if (const auto version = state->log->stat(key)) {
                nlohmann::json entry = info_json(info_of(*version));
                if (version->deleted) entry["d"] = true;
                versions.push_back(std::move(entry));
            }
        }
        return answer(versions);
    }
    if (method == kListMethod) {
        nlohmann::json objects = nlohmann::json::array();
        const std::size_t limit = header.value("limit", std::size_t{100});
        state->log->list(
            header.value("prefix", std::string()), header.value("after", std::string()),
            [&](const ObjectVersion& version) {
                nlohmann::json entry = info_json(info_of(version));
                if (version.deleted) entry["d"] = true;
                objects.push_back(std::move(entry));
                return objects.size() < limit;
            },
            /*tombstones=*/true);
        return answer(objects);
    }
    if (method == kUsageMethod) {
        std::uint64_t objects = 0;
        std::uint64_t total = 0;
        const Placement view = placement(bucket);
        state->log->list("", "", [&](const ObjectVersion& version) {
            if (view.owners(bucket, version.key).front() == node_id_) {
                ++objects;
                total += version.size;
            }
            return true;
        });
        return answer({{"objects", objects}, {"bytes", total}});
    }
    if (method == kKeysMethod) {
        const std::string requester = header.value("for", std::string());
        nlohmann::json versions = nlohmann::json::array();
        const Placement view = placement(bucket);
        for (const ObjectVersion& version :
             state->log->versions(header.value("buckets", std::vector<std::size_t>{}),
                                  [&](const std::string& key) {
                                      return view.owns(requester, bucket, key);
                                  })) {
            nlohmann::json entry = info_json(info_of(version));
            if (version.deleted) entry["d"] = true;
            versions.push_back(std::move(entry));
        }
        return answer(versions);
    }
    return refuse("unhandled");
}

void ObjectStore::on_event(const zetta::cluster::Signal& event) {
    if (auto* delegate = delegate_.load()) delegate->on_event(event);
}

void ObjectStore::on_task(const zetta::cluster::Signal& task) {
    if (auto* delegate = delegate_.load()) delegate->on_task(task);
}

}  // namespace zetta::data
