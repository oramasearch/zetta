#include "zetta/data/backup_snapshot.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/blob_store.hpp"
#include "zetta/data/entity_key_table.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

// Backup snapshots and their restore (zetta#1518). The design, the archive
// format and what is and is not in a snapshot: docs/data/backup-snapshot.md.
namespace zetta::data {
namespace fs = std::filesystem;
using nlohmann::json;
using zetta::cluster::ReplicationWrite;

namespace {

constexpr std::size_t kRecordsPerFrame = 1000;
constexpr std::size_t kMaxHeaderBytes = 1u << 20;
// A frame held whole while it is applied: the catalogue, a page of records, a
// body (the BlobStore takes a body whole), the manifest.
constexpr std::uint64_t kMaxBufferedFrame = 4ull << 30;
constexpr std::size_t kCatalogueBatch = 512;
// The record store's collection of a metric's buckets (metric_store.cpp).
constexpr std::string_view kMetricCollectionPrefix = "_metric.";

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The families a snapshot leaves out: the source cluster's membership, the
// object leases (a holder's runtime state, and a restored lease would fence the
// restored buckets), retired audit, and the identity records, which are sealed
// under the source cluster's identity root and stay with it.
constexpr std::array<std::string_view, 10> kExcludedPrefixes = {
    catalog::kNodePrefix,      catalog::kObjectLeasePrefix, catalog::kAuditPrefix,
    catalog::kUserPrefix,      catalog::kRolePrefix,        catalog::kTokenPrefix,
    catalog::kUserRolePrefix,  catalog::kPolicyPrefix,      catalog::kPrincipalPrefix,
    catalog::kAccessKeyPrefix,
};

// Families counted by what they hold rather than by their definitions.
bool counted_by_contents(std::string_view family) {
    return family == "record_collection" || family == "metric" || family == "object_bucket";
}

std::string family_of(std::string_view key) {
    return std::string(key.substr(0, key.find(':')));
}

std::string string_field(const json& value, std::string_view name) {
    if (!value.is_object()) return {};
    const auto found = value.find(std::string(name));
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::uint64_t unsigned_field(const json& value, std::string_view name) {
    if (!value.is_object()) return 0;
    const auto found = value.find(std::string(name));
    if (found == value.end()) return 0;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer() && found->get<std::int64_t>() > 0) {
        return static_cast<std::uint64_t>(found->get<std::int64_t>());
    }
    return 0;
}

bool bool_field(const json& value, std::string_view name) {
    if (!value.is_object()) return false;
    const auto found = value.find(std::string(name));
    return found != value.end() && found->is_boolean() && found->get<bool>();
}

std::string without_prefix(std::string id, std::string_view prefix) {
    if (std::string_view(id).starts_with(prefix)) id.erase(0, prefix.size());
    return id;
}

// The large-object and model-weights bodies a catalogue names, by id.
std::vector<std::string> body_ids(const json& catalogue) {
    std::vector<std::string> ids;
    catalog::for_each_with_prefix(
        catalogue, catalog::kLargeObjectPrefix, [&](const std::string& key, const json& value) {
            if (value.is_object() && value.contains("db")) {
                ids.push_back(catalog::id_after(key, catalog::kLargeObjectPrefix));
            }
        });
    catalog::for_each_with_prefix(
        catalogue, catalog::kModelPrefix, [&](const std::string& key, const json& value) {
            if (value.is_object() && value.contains("weights") && value["weights"].is_object()) {
                ids.push_back(catalog::id_after(key, catalog::kModelPrefix));
            }
        });
    return ids;
}

// Every record collection a catalogue implies: the declared ones, the
// entity-key rows and each metric's buckets.
std::vector<std::string> collection_names(const json& catalogue) {
    std::vector<std::string> names;
    catalog::for_each_with_prefix(catalogue, catalog::kRecordCollectionPrefix,
                                  [&](const std::string& key, const json&) {
                                      names.push_back(
                                          catalog::id_after(key, catalog::kRecordCollectionPrefix));
                                  });
    names.emplace_back(kEntityKeyCollection);
    catalog::for_each_with_prefix(catalogue, catalog::kMetricPrefix,
                                  [&](const std::string& key, const json&) {
                                      names.push_back(std::string(kMetricCollectionPrefix) +
                                                      catalog::id_after(key, catalog::kMetricPrefix));
                                  });
    return names;
}

std::vector<std::string> bucket_names(const json& catalogue) {
    std::vector<std::string> names;
    catalog::for_each_with_prefix(catalogue, catalog::kObjectBucketPrefix,
                                  [&](const std::string& key, const json&) {
                                      names.push_back(
                                          catalog::id_after(key, catalog::kObjectBucketPrefix));
                                  });
    return names;
}

// The size and SHA-256 the catalogue records for a body (a large object's, or a
// model's weights).
std::pair<std::uint64_t, std::string> body_descriptor(const json& catalogue,
                                                      const std::string& id) {
    if (const auto found = catalogue.find(std::string(catalog::kLargeObjectPrefix) + id);
        found != catalogue.end()) {
        return {unsigned_field(*found, "size"), string_field(*found, "sha256")};
    }
    if (const auto found = catalogue.find(std::string(catalog::kModelPrefix) + id);
        found != catalogue.end() && found->is_object() && found->contains("weights")) {
        const json& weights = (*found)["weights"];
        return {unsigned_field(weights, "size"), string_field(weights, "sha256")};
    }
    return {0, {}};
}

// Which nodes own each partition body: blob id -> owner_nodes.
std::unordered_map<std::string, std::vector<std::string>> partition_owners(const json& catalogue) {
    std::unordered_map<std::string, std::vector<std::string>> owners;
    catalog::for_each_with_prefix(
        catalogue, catalog::kPartitionPrefix, [&](const std::string&, const json& value) {
            const std::string blob =
                without_prefix(string_field(value, "blob_ref"), catalog::kLargeObjectPrefix);
            if (blob.empty() || !value.is_object()) return;
            const auto nodes = value.find("owner_nodes");
            if (nodes == value.end() || !nodes->is_array()) return;
            for (const json& node : *nodes) {
                if (node.is_string()) owners[blob].push_back(node.get<std::string>());
            }
        });
    return owners;
}

json record_json(const StoredRecord& record) {
    json line{{"key", record.key},
              {"value", record.value},
              {"revision", record.revision},
              {"updated_ms", record.updated_ms},
              {"index", record.index},
              {"order", record.order}};
    if (record.expires_ms) line["expires_ms"] = *record.expires_ms;
    return line;
}

std::optional<StoredRecord> record_from_json(const json& line, const std::string& collection) {
    if (!line.is_object() || !line.contains("key") || !line["key"].is_string() ||
        !line.contains("value") || !line["value"].is_string() || !line.contains("revision") ||
        !line["revision"].is_number_unsigned()) {
        return std::nullopt;
    }
    StoredRecord record;
    record.collection = collection;
    record.key = line["key"].get<std::string>();
    record.value = line["value"].get<std::string>();
    record.revision = line["revision"].get<std::uint64_t>();
    if (const auto updated = line.find("updated_ms");
        updated != line.end() && updated->is_number_integer()) {
        record.updated_ms = updated->get<std::int64_t>();
    }
    if (const auto index = line.find("index"); index != line.end()) {
        if (!index->is_object()) return std::nullopt;
        for (const auto& item : index->items()) {
            if (!item.value().is_string()) return std::nullopt;
            record.index[item.key()] = item.value().get<std::string>();
        }
    }
    record.order = string_field(line, "order");
    if (const auto expires = line.find("expires_ms");
        expires != line.end() && expires->is_number_integer()) {
        record.expires_ms = expires->get<std::int64_t>();
    }
    return record;
}

bool write_json_file(const fs::path& path, const json& value) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << value.dump();
        if (!out.good()) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

std::optional<json> read_json_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json value = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded() || !value.is_object()) return std::nullopt;
    return value;
}

BackupOutcome refusal(BackupStatus status, std::string message) {
    BackupOutcome outcome;
    outcome.status = status;
    outcome.message = std::move(message);
    return outcome;
}

// Write catalogue keys in batches, each one replicated change.
void write_catalogue(zetta::cluster::ReplicationManager& replication,
                     std::vector<ReplicationWrite>& writes) {
    for (std::size_t from = 0; from < writes.size(); from += kCatalogueBatch) {
        const std::size_t to = std::min(writes.size(), from + kCatalogueBatch);
        std::vector<ReplicationWrite> batch(std::make_move_iterator(writes.begin() + from),
                                            std::make_move_iterator(writes.begin() + to));
        const std::string anchor = batch.front().key;
        replication.update(anchor, [&batch](const json*)
                                       -> std::optional<std::vector<ReplicationWrite>> {
            return std::move(batch);
        });
    }
    writes.clear();
}

}  // namespace

std::string_view to_string(BackupStatus status) {
    switch (status) {
        case BackupStatus::Ok: return "ok";
        case BackupStatus::InvalidInput: return "invalid_input";
        case BackupStatus::NotFound: return "not_found";
        case BackupStatus::Conflict: return "conflict";
        case BackupStatus::Busy: return "busy";
        case BackupStatus::VerificationFailed: return "verification_failed";
        case BackupStatus::Failed: return "engine_error";
    }
    return "engine_error";
}

bool backed_up_key(std::string_view key) {
    if (key.find(':') == std::string_view::npos) return false;
    return std::ranges::none_of(kExcludedPrefixes,
                                [&](std::string_view prefix) { return key.starts_with(prefix); });
}

json summary_to_json(const BackupSummary& summary) {
    json value = json::object();
    for (const auto& [name, count] : summary) value[name] = count;
    return value;
}

std::optional<BackupSummary> summary_from_json(const json& value) {
    if (!value.is_object()) return std::nullopt;
    BackupSummary summary;
    for (const auto& item : value.items()) {
        if (!item.value().is_number_integer()) return std::nullopt;
        summary[item.key()] = item.value().get<std::int64_t>();
    }
    return summary;
}

BackupSummary summarize(const json& catalogue,
                        const std::function<bool(const std::string&)>& body_present,
                        const std::map<std::string, std::int64_t>& objects_per_bucket,
                        const std::map<std::string, std::int64_t>& records_per_collection) {
    BackupSummary summary;
    if (!catalogue.is_object()) return summary;
    for (const auto& item : catalogue.items()) {
        if (!backed_up_key(item.key())) continue;
        const std::string family = family_of(item.key());
        if (!counted_by_contents(family)) ++summary["catalogue:" + family];
    }

    // Rows per table, named <database>.<table> so the name is the same on every node.
    std::map<std::string, std::string> databases;
    catalog::for_each_with_prefix(catalogue, catalog::kDatabasePrefix,
                                  [&](const std::string& key, const json& value) {
                                      databases[catalog::id_after(key, catalog::kDatabasePrefix)] =
                                          string_field(value, "name");
                                  });
    struct TableCount {
        std::string label;
        std::int64_t rows = 0;
    };
    std::map<std::string, TableCount> tables;
    catalog::for_each_with_prefix(
        catalogue, catalog::kTablePrefix, [&](const std::string& key, const json& value) {
            const std::string database = string_field(value, "db");
            const auto named = databases.find(database);
            tables[catalog::id_after(key, catalog::kTablePrefix)].label =
                (named == databases.end() ? database : named->second) + "." +
                string_field(value, "name");
        });
    catalog::for_each_with_prefix(
        catalogue, catalog::kRowPrefix, [&](const std::string&, const json& value) {
            const auto table =
                tables.find(without_prefix(string_field(value, "tbl"), catalog::kTablePrefix));
            if (table != tables.end()) ++table->second.rows;
        });
    catalog::for_each_with_prefix(
        catalogue, catalog::kPartitionPrefix, [&](const std::string&, const json& value) {
            const auto table =
                tables.find(without_prefix(string_field(value, "table"), catalog::kTablePrefix));
            const std::string blob =
                without_prefix(string_field(value, "blob_ref"), catalog::kLargeObjectPrefix);
            if (table == tables.end() || blob.empty() || !body_present(blob)) return;
            const auto range = value.find("row_range");
            if (range == value.end()) return;
            const std::uint64_t start = unsigned_field(*range, "start");
            const std::uint64_t end = unsigned_field(*range, "end");
            if (end > start) table->second.rows += static_cast<std::int64_t>(end - start);
        });
    for (const auto& [id, table] : tables) summary["table:" + table.label] += table.rows;

    std::int64_t present = 0;
    std::int64_t missing = 0;
    for (const std::string& id : body_ids(catalogue)) ++(body_present(id) ? present : missing);
    summary["bodies"] = present;
    if (missing > 0) summary["bodies:missing"] = missing;

    for (const std::string& bucket : bucket_names(catalogue)) {
        const auto counted = objects_per_bucket.find(bucket);
        summary["bucket:" + bucket] = counted == objects_per_bucket.end() ? 0 : counted->second;
    }
    for (const auto& [collection, count] : records_per_collection) {
        if (count > 0) summary["collection:" + collection] = count;
    }
    return summary;
}

BackupSummary summarize_node(BackupStores& stores) {
    const std::shared_ptr<const json> catalogue = stores.replication.shared_snapshot();
    std::map<std::string, std::int64_t> objects;
    for (const std::string& bucket : bucket_names(*catalogue)) {
        std::int64_t count = 0;
        std::string after;
        do {
            const ObjectPage page =
                stores.objects.list(bucket, "", after, ObjectStore::kMaxListLimit);
            if (!page.ok()) break;
            count += static_cast<std::int64_t>(page.objects.size());
            after = page.next_after;
        } while (!after.empty());
        objects[bucket] = count;
    }
    std::map<std::string, std::int64_t> records;
    for (const std::string& collection : collection_names(*catalogue)) {
        std::int64_t count = 0;
        stores.records.export_records(collection, [&](const StoredRecord&) {
            ++count;
            return true;
        });
        records[collection] = count;
    }
    const std::vector<std::string> missing_list = stores.bodies.missing_blobs();
    const std::unordered_set<std::string> missing(missing_list.begin(), missing_list.end());
    return summarize(
        *catalogue, [&](const std::string& id) { return !missing.contains(id); }, objects,
        records);
}

bool holds_user_data(const BackupSummary& summary) {
    return std::ranges::any_of(summary, [](const auto& entry) { return entry.second > 0; });
}

json summary_differences(const BackupSummary& expected, const BackupSummary& observed) {
    json differences = json::object();
    std::set<std::string> names;
    for (const auto& [name, count] : expected) names.insert(name);
    for (const auto& [name, count] : observed) names.insert(name);
    for (const std::string& name : names) {
        const auto want = expected.find(name);
        const auto got = observed.find(name);
        const std::int64_t a = want == expected.end() ? 0 : want->second;
        const std::int64_t b = got == observed.end() ? 0 : got->second;
        if (a != b) differences[name] = {{"expected", a}, {"observed", b}};
    }
    return differences;
}

// ---- the freeze ---------------------------------------------------------------

namespace {

constexpr std::chrono::milliseconds kGateTimeout{5000};
constexpr std::chrono::milliseconds kBodyTimeout{30000};

std::chrono::milliseconds lease_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(BackupFreeze::kLease);
}

// Freeze both gates for `owner`, or neither.
bool freeze_both(RecordStore& records, ObjectStore& objects, std::string_view owner,
                 std::chrono::milliseconds lease) {
    const auto until = WriteGate::Clock::now() + lease;
    if (!records.write_gate().freeze(owner, until)) return false;
    if (!objects.write_gate().freeze(owner, until)) {
        records.write_gate().thaw(owner);
        return false;
    }
    return true;
}

void thaw_both(RecordStore& records, ObjectStore& objects, std::string_view owner) {
    objects.write_gate().thaw(owner);
    records.write_gate().thaw(owner);
}

std::string gate_payload(std::string_view op, std::string_view owner) {
    return json{{"op", std::string(op)},
                {"owner", std::string(owner)},
                {"lease_ms", lease_ms().count()}}
        .dump();
}

}  // namespace

BackupFreeze::BackupFreeze(RecordStore& records, ObjectStore& objects, std::string owner)
    : records_(&records), objects_(&objects), owner_(std::move(owner)) {}

BackupFreeze::BackupFreeze(BackupFreeze&& other) noexcept
    : records_(std::exchange(other.records_, nullptr)),
      objects_(std::exchange(other.objects_, nullptr)),
      cluster_(std::exchange(other.cluster_, nullptr)),
      owner_(std::move(other.owner_)),
      members_(std::move(other.members_)) {}

BackupFreeze::~BackupFreeze() {
    if (records_ && objects_) thaw_both(*records_, *objects_, owner_);
    // Fire and forget: never wait on a member here. A thaw that is lost lapses
    // with its lease.
    if (cluster_) {
        const std::string payload = gate_payload("thaw", owner_);
        for (const std::string& member : members_) {
            cluster_->send_event(member, std::string(BackupPeer::kGateMethod), payload);
        }
    }
}

std::optional<BackupFreeze> BackupFreeze::begin(RecordStore& records, ObjectStore& objects) {
    std::string owner = "local:" + zetta::util::new_uuid();
    if (!freeze_both(records, objects, owner, lease_ms())) return std::nullopt;
    return BackupFreeze(records, objects, std::move(owner));
}

BackupOutcome BackupFreeze::begin(RecordStore& records, ObjectStore& objects,
                                  zetta::cluster::Cluster* cluster, std::string owner,
                                  std::optional<BackupFreeze>& out) {
    out.reset();
    if (!freeze_both(records, objects, owner, lease_ms())) {
        return refusal(BackupStatus::Busy,
                       "another backup snapshot is being captured on this node; retry");
    }
    BackupFreeze freeze(records, objects, std::move(owner));
    if (cluster) {
        std::vector<std::string> members;
        for (const zetta::cluster::ClusterPeer& peer : cluster->peers()) {
            if (peer.node_id != cluster->node_id()) members.push_back(peer.node_id);
        }
        freeze.cluster_ = cluster;
        if (!members.empty()) {
            const std::vector<zetta::cluster::SignalResponse> answers = cluster->request_each(
                members, std::string(BackupPeer::kGateMethod), gate_payload("freeze", freeze.owner_),
                kGateTimeout);
            for (std::size_t index = 0; index < members.size(); ++index) {
                // Thawed on every path: the members that froze are in members_.
                if (index < answers.size() && answers[index].ok) {
                    freeze.members_.push_back(members[index]);
                }
            }
            for (std::size_t index = 0; index < members.size(); ++index) {
                if (index < answers.size() && answers[index].ok) continue;
                BackupOutcome refused = refusal(
                    BackupStatus::Busy,
                    "member " + members[index] + " did not pause its writes for the snapshot (" +
                        (index < answers.size() ? answers[index].error : std::string("no answer")) +
                        "); every member must run an engine with backup.gate and hold no other "
                        "snapshot; retry");
                refused.details = {{"member", members[index]}};
                return refused;  // `freeze` thaws itself and every member that froze
            }
        }
    }
    out.emplace(std::move(freeze));
    return {};
}

bool BackupFreeze::drained() const {
    return records_ && objects_ && records_->write_gate().drained() &&
           objects_->write_gate().drained();
}

BackupOutcome BackupFreeze::ask_members(std::string_view op, bool& all_drained, bool& all_held) {
    all_drained = true;
    all_held = true;
    if (!cluster_ || members_.empty()) return {};
    const std::vector<zetta::cluster::SignalResponse> answers = cluster_->request_each(
        members_, std::string(BackupPeer::kGateMethod), gate_payload(op, owner_), kGateTimeout);
    for (std::size_t index = 0; index < members_.size(); ++index) {
        if (index >= answers.size() || !answers[index].ok) {
            BackupOutcome lost = refusal(
                BackupStatus::Busy, "member " + members_[index] +
                                        " stopped answering during the snapshot; retry");
            lost.details = {{"member", members_[index]}};
            return lost;
        }
        const json answer = json::parse(answers[index].payload, nullptr, false);
        all_drained = all_drained && bool_field(answer, "drained");
        all_held = all_held && bool_field(answer, "held");
    }
    return {};
}

BackupOutcome BackupFreeze::members_drained(bool& all) {
    bool held_all = true;
    BackupOutcome asked = ask_members("status", all, held_all);
    if (!asked.ok()) return asked;
    if (!held_all) {
        all = false;
        return refusal(BackupStatus::Busy,
                       "a member's pause lapsed before its writes drained; retry");
    }
    return {};
}

BackupOutcome BackupFreeze::held() {
    if (!records_ || !objects_ || !records_->write_gate().held(owner_) ||
        !objects_->write_gate().held(owner_)) {
        return refusal(BackupStatus::Busy,
                       "this node's pause lapsed during the capture (" +
                           std::to_string(kLease.count()) +
                           " s): the snapshot would not be one cut; retry");
    }
    bool drained_all = true;
    bool held_all = true;
    BackupOutcome asked = ask_members("status", drained_all, held_all);
    if (!asked.ok()) return asked;
    if (!held_all) {
        return refusal(BackupStatus::Busy,
                       "a member's pause lapsed during the capture: the snapshot would not be "
                       "one cut; retry");
    }
    return {};
}

// ---- the peer side of the signal plane ---------------------------------------------

BackupPeer::BackupPeer(RecordStore& records, ObjectStore& objects,
                       zetta::cluster::ReplicationManager& replication, fs::path data_dir)
    : records_(records), objects_(objects), bodies_(std::move(data_dir), replication) {}

BackupPeer::~BackupPeer() { detach(); }

void BackupPeer::attach(zetta::cluster::Cluster& cluster) {
    detach();
    delegate_.store(cluster.signal_handler());
    cluster_.store(&cluster);
    cluster.set_signal_handler(this);
}

void BackupPeer::detach() {
    zetta::cluster::Cluster* cluster = cluster_.exchange(nullptr);
    if (!cluster) return;
    if (cluster->signal_handler() == this) cluster->set_signal_handler(delegate_.load());
    delegate_.store(nullptr);
}

zetta::cluster::SignalResponse BackupPeer::gate(const json& request) {
    const std::string op = string_field(request, "op");
    const std::string owner = string_field(request, "owner");
    if (owner.empty()) return {.ok = false, .payload = {}, .error = "backup.gate: no owner"};
    if (op == "thaw") {
        thaw_both(records_, objects_, owner);
    } else if (op == "freeze" || op == "status") {
        if (op == "freeze") {
            // Bounded by the coordinator's lease, never longer than ours.
            const std::uint64_t asked = unsigned_field(request, "lease_ms");
            const auto lease = std::chrono::milliseconds(
                std::min<std::uint64_t>(asked == 0 ? lease_ms().count() : asked,
                                        static_cast<std::uint64_t>(lease_ms().count())));
            if (!freeze_both(records_, objects_, owner, lease)) {
                return {.ok = false,
                        .payload = {},
                        .error = "backup.gate: another snapshot holds this node's writes"};
            }
        }
    } else {
        return {.ok = false, .payload = {}, .error = "backup.gate: unknown op '" + op + "'"};
    }
    const bool held =
        records_.write_gate().held(owner) && objects_.write_gate().held(owner);
    const bool drained = records_.write_gate().drained() && objects_.write_gate().drained();
    return {.ok = true, .payload = json{{"drained", drained}, {"held", held}}.dump(), .error = {}};
}

zetta::cluster::SignalResponse BackupPeer::body(const json& request) {
    const std::string id = string_field(request, "id");
    const std::uint64_t offset = unsigned_field(request, "offset");
    const std::uint64_t length = std::min(unsigned_field(request, "length"), kMaxSliceBytes);
    std::string bytes;
    std::uint64_t size = 0;
    if (id.empty() || !bodies_.read_blob_range(id, offset, length, bytes, size)) {
        return {.ok = false, .payload = {}, .error = "backup.body: this node does not hold " + id};
    }
    std::string payload = json{{"size", size}}.dump();
    payload.push_back('\n');
    payload.append(bytes);
    return {.ok = true, .payload = std::move(payload), .error = {}};
}

zetta::cluster::SignalResponse BackupPeer::on_request(const zetta::cluster::Signal& request) {
    if (request.method != kGateMethod && request.method != kBodyMethod) {
        if (auto* delegate = delegate_.load()) return delegate->on_request(request);
        return {.ok = false, .payload = {}, .error = "no handler"};
    }
    const json payload = json::parse(request.payload, nullptr, /*allow_exceptions=*/false);
    if (payload.is_discarded() || !payload.is_object()) {
        return {.ok = false, .payload = {}, .error = std::string(request.method) + ": not JSON"};
    }
    return request.method == kGateMethod ? gate(payload) : body(payload);
}

void BackupPeer::on_event(const zetta::cluster::Signal& event) {
    if (event.method == kGateMethod) {
        const json payload = json::parse(event.payload, nullptr, /*allow_exceptions=*/false);
        if (payload.is_object() && string_field(payload, "op") == "thaw") gate(payload);
        return;
    }
    if (auto* delegate = delegate_.load()) delegate->on_event(event);
}

void BackupPeer::on_task(const zetta::cluster::Signal& task) {
    if (auto* delegate = delegate_.load()) delegate->on_task(task);
}

std::optional<std::string> fetch_body(zetta::cluster::Cluster& cluster,
                                      const std::vector<std::string>& owners,
                                      const std::string& id, std::uint64_t size,
                                      const std::string& sha256) {
    for (const std::string& owner : owners) {
        if (owner == cluster.node_id()) continue;
        std::string body;
        body.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(size, 64u << 20)));
        bool whole = true;
        do {
            const json request{{"id", id},
                               {"offset", static_cast<std::uint64_t>(body.size())},
                               {"length", BackupPeer::kMaxSliceBytes}};
            const zetta::cluster::SignalResponse answer = cluster.request(
                owner, std::string(BackupPeer::kBodyMethod), request.dump(), kBodyTimeout);
            const std::size_t newline = answer.ok ? answer.payload.find('\n') : std::string::npos;
            if (newline == std::string::npos) {
                whole = false;
                break;
            }
            const json header = json::parse(answer.payload.substr(0, newline), nullptr, false);
            if (unsigned_field(header, "size") != size) {
                whole = false;
                break;
            }
            const std::string_view slice = std::string_view(answer.payload).substr(newline + 1);
            if (slice.empty() && body.size() < size) {
                whole = false;
                break;
            }
            body.append(slice);
        } while (body.size() < size);
        if (!whole || body.size() != size) continue;
        if (!sha256.empty() && zetta::util::sha256_hex(body) != sha256) continue;
        return body;
    }
    return std::nullopt;
}

// ---- writing a snapshot --------------------------------------------------------

SnapshotWriter::SnapshotWriter(BackupStores& stores, fs::path directory, std::string id)
    : stores_(stores), folder_(std::move(directory) / id), id_(std::move(id)) {}

SnapshotWriter::~SnapshotWriter() {
    if (finished_) return;
    objects_.clear();
    if (out_.is_open()) out_.close();
    std::error_code ec;
    fs::remove_all(folder_, ec);
}

bool SnapshotWriter::write_bytes(std::string_view bytes) {
    out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    hash_.update(bytes);
    written_ += bytes.size();
    return out_.good();
}

bool SnapshotWriter::write_header(json header, std::uint64_t bytes) {
    header["bytes"] = bytes;
    return write_bytes(header.dump() + "\n");
}

bool SnapshotWriter::write_frame(json header, std::string_view payload) {
    return write_header(std::move(header), payload.size()) && write_bytes(payload);
}

BackupOutcome SnapshotWriter::failed(std::string message) {
    return refusal(BackupStatus::Failed, std::move(message));
}

BackupOutcome SnapshotWriter::capture(std::shared_ptr<const json> catalogue) {
    taken_ms_ = now_ms();
    std::error_code ec;
    fs::create_directories(folder_, ec);
    if (ec) return failed("could not create " + folder_.string() + ": " + ec.message());
    out_.open(folder_ / "archive", std::ios::binary | std::ios::trunc);
    if (!out_) return failed("could not create " + (folder_ / "archive").string());

    // A partition-only table whose load has not finished has no manifest yet:
    // leave it out whole, its partitions and their bodies with it, so no table
    // is ever restored torn.
    static const json kNothing = json::object();
    const json& source = catalogue ? *catalogue : kNothing;
    std::unordered_set<std::string> unfinished;
    catalog::for_each_with_prefix(
        source, catalog::kTablePrefix, [&](const std::string& key, const json& value) {
            const std::string id = catalog::id_after(key, catalog::kTablePrefix);
            if (bool_field(value, "partition_only") &&
                !source.contains(catalog::manifest_key(id))) {
                unfinished.insert(id);
                skipped_tables_.push_back(string_field(value, "name"));
            }
        });
    std::unordered_set<std::string> skipped_bodies;
    std::unordered_set<std::string> skipped_partitions;
    catalog::for_each_with_prefix(
        source, catalog::kPartitionPrefix, [&](const std::string& key, const json& value) {
            if (unfinished.contains(
                    without_prefix(string_field(value, "table"), catalog::kTablePrefix))) {
                skipped_partitions.insert(key);
                skipped_bodies.insert(
                    without_prefix(string_field(value, "blob_ref"), catalog::kLargeObjectPrefix));
            }
        });
    const auto skipped = [&](const std::string& key) {
        if (skipped_partitions.contains(key)) return true;
        for (std::string_view prefix :
             {catalog::kTablePrefix, catalog::kColumnOrderPrefix, catalog::kManifestPrefix}) {
            if (std::string_view(key).starts_with(prefix) &&
                unfinished.contains(catalog::id_after(key, prefix))) {
                return true;
            }
        }
        return std::string_view(key).starts_with(catalog::kLargeObjectPrefix) &&
               skipped_bodies.contains(catalog::id_after(key, catalog::kLargeObjectPrefix));
    };
    if (source.is_object()) {
        for (const auto& item : source.items()) {
            if (backed_up_key(item.key()) && !skipped(item.key())) {
                catalogue_[item.key()] = item.value();
            }
        }
    }

    if (!write_frame({{"kind", "header"},
                      {"format", std::string(kBackupFormat)},
                      {"snapshot", id_},
                      {"node", stores_.node_id},
                      {"taken_ms", taken_ms_}},
                     {}) ||
        !write_frame({{"kind", "catalogue"}}, catalogue_.dump())) {
        return failed("could not write the archive under " + folder_.string());
    }

    // Every record, while no record write can land.
    for (const std::string& collection : collection_names(catalogue_)) {
        std::string lines;
        std::size_t in_frame = 0;
        std::int64_t total = 0;
        bool written = true;
        const auto flush = [&] {
            if (in_frame == 0) return true;
            const bool ok = write_frame(
                {{"kind", "records"}, {"collection", collection}, {"count", in_frame}}, lines);
            lines.clear();
            in_frame = 0;
            return ok;
        };
        stores_.records.export_records(collection, [&](const StoredRecord& record) {
            lines += record_json(record).dump();
            lines += '\n';
            ++in_frame;
            ++total;
            if (in_frame == kRecordsPerFrame && !flush()) {
                written = false;
                return false;
            }
            return true;
        });
        if (!written || !flush()) return failed("could not write the archive's records");
        records_per_collection_[collection] = total;
    }

    // A reader on every object version, while no object write can land: each
    // reader holds what it reads, so the bytes copied after the thaw are these.
    for (const std::string& bucket : bucket_names(catalogue_)) {
        objects_per_bucket_[bucket] = 0;
        std::string after;
        do {
            const ObjectPage page =
                stores_.objects.list(bucket, "", after, ObjectStore::kMaxListLimit);
            if (!page.ok()) {
                return failed("could not list bucket '" + bucket + "': " + page.message);
            }
            for (const ObjectInfo& info : page.objects) {
                std::shared_ptr<ObjectReader> reader;
                const ObjectOutcome opened =
                    stores_.objects.open_read(bucket, info.key, 0, std::nullopt, reader);
                if (opened.status == ObjectStatus::NotFound) continue;
                if (!opened.ok() || !reader) {
                    return failed("could not read object '" + info.key + "' of bucket '" +
                                  bucket + "': " + opened.message);
                }
                objects_.push_back({bucket, info.key, reader->info().generation,
                                    reader->info().updated_ms, std::move(reader)});
                ++objects_per_bucket_[bucket];
            }
            after = page.next_after;
        } while (!after.empty());
    }
    out_.flush();
    if (!out_.good()) return failed("could not write the archive under " + folder_.string());
    BackupOutcome outcome;
    outcome.snapshot = id_;
    return outcome;
}

BackupOutcome SnapshotWriter::finish() {
    constexpr std::uint64_t kChunk = ObjectStore::kSliceBytes;
    std::string chunk;
    for (PinnedObject& pinned : objects_) {
        const std::uint64_t length = pinned.reader->length();
        if (!write_header({{"kind", "object"},
                           {"bucket", pinned.bucket},
                           {"key", pinned.key},
                           {"generation", pinned.generation},
                           {"updated_ms", pinned.updated_ms}},
                          length)) {
            return failed("could not write the archive under " + folder_.string());
        }
        for (std::uint64_t from = 0; from < length; from += kChunk) {
            const std::uint64_t take = std::min(kChunk, length - from);
            chunk.clear();
            if (!pinned.reader->read(from, take, chunk) || chunk.size() != take) {
                return failed("object '" + pinned.key + "' of bucket '" + pinned.bucket +
                              "' could not be read; take the snapshot again");
            }
            if (!write_bytes(chunk)) {
                return failed("could not write the archive under " + folder_.string());
            }
        }
        pinned.reader.reset();
    }
    objects_.clear();

    // The bodies. One this node lacks and another member owns makes the snapshot
    // incomplete: refuse it rather than write a short archive. One no node owns
    // (lost, or never arrived) is named in the manifest and counted missing.
    const auto owners = partition_owners(catalogue_);
    std::unordered_set<std::string> present;
    std::vector<std::string> missing;
    for (const std::string& id : body_ids(catalogue_)) {
        std::optional<std::string> body = stores_.bodies.read_blob(id);
        const auto owned = owners.find(id);
        const bool owned_elsewhere =
            owned != owners.end() && !owned->second.empty() &&
            std::ranges::find(owned->second, stores_.node_id) == owned->second.end();
        if (!body && stores_.cluster) {
            // A body this node lacks: fetched from its owners (a partition's), or
            // from any member (a body every node should hold, not here yet), a
            // slice at a time and verified against its catalogue entry (zetta#1520).
            std::vector<std::string> sources;
            if (owned != owners.end() && !owned->second.empty()) {
                sources = owned->second;
            } else {
                for (const zetta::cluster::ClusterPeer& peer : stores_.cluster->peers()) {
                    sources.push_back(peer.node_id);
                }
            }
            const auto [size, sha256] = body_descriptor(catalogue_, id);
            body = fetch_body(*stores_.cluster, sources, id, size, sha256);
        }
        if (!body) {
            if (owned_elsewhere) {
                BackupOutcome outcome = refusal(
                    BackupStatus::Conflict,
                    "the body of partition blob " + id +
                        " is held only by other members of the cluster, and none of them gave "
                        "it whole; retry once they are reachable");
                outcome.details = {{"body", id}, {"owners", owned->second}};
                return outcome;
            }
            missing.push_back(id);
            continue;
        }
        if (!write_frame({{"kind", "body"}, {"id", id}, {"sha256", zetta::util::sha256_hex(*body)}},
                         *body)) {
            return failed("could not write the archive under " + folder_.string());
        }
        present.insert(id);
    }

    const BackupSummary summary = summarize(
        catalogue_, [&](const std::string& id) { return present.contains(id); },
        objects_per_bucket_, records_per_collection_);
    json excluded = json::array();
    for (std::string_view prefix : kExcludedPrefixes) excluded.push_back(std::string(prefix));
    const json manifest{{"format", std::string(kBackupFormat)},
                        {"snapshot", id_},
                        {"node", stores_.node_id},
                        {"taken_ms", taken_ms_},
                        {"summary", summary_to_json(summary)},
                        {"missing_bodies", missing},
                        {"skipped_tables", skipped_tables_},
                        {"excluded", excluded}};
    if (!write_frame({{"kind", "manifest"}}, manifest.dump())) {
        return failed("could not write the archive under " + folder_.string());
    }
    zetta::util::Sha256 digest = hash_;
    const std::string trailer = json{{"kind", "end"}, {"sha256", digest.hex()}}.dump() + "\n";
    out_.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
    written_ += trailer.size();
    out_.close();
    if (out_.fail()) return failed("could not write the archive under " + folder_.string());
    if (!write_json_file(folder_ / "snapshot.json", {{"snapshot", id_},
                                                     {"taken_ms", taken_ms_},
                                                     {"size_bytes", written_},
                                                     {"summary", summary_to_json(summary)}})) {
        return failed("could not record the snapshot under " + folder_.string());
    }
    finished_ = true;
    BackupOutcome outcome;
    outcome.snapshot = id_;
    outcome.summary = summary;
    outcome.size_bytes = written_;
    outcome.archive = folder_ / "archive";
    outcome.details = {{"missing_bodies", missing}, {"skipped_tables", skipped_tables_}};
    return outcome;
}

// ---- the snapshots a node holds -------------------------------------------------

BackupSnapshots::BackupSnapshots(fs::path directory) : directory_(std::move(directory)) {}

bool BackupSnapshots::valid_id(std::string_view id) {
    return id.size() == 36 && std::ranges::all_of(id, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
           });
}

BackupOutcome BackupSnapshots::find(const std::string& id) const {
    const auto gone = [&] {
        return refusal(BackupStatus::NotFound,
                       "no backup snapshot '" + id + "' on this node: released or expired");
    };
    if (!valid_id(id)) return gone();
    const fs::path folder = directory_ / id;
    const std::optional<json> record = read_json_file(folder / "snapshot.json");
    if (!record) return gone();
    const std::int64_t taken =
        record->contains("taken_ms") && (*record)["taken_ms"].is_number_integer()
            ? (*record)["taken_ms"].get<std::int64_t>()
            : 0;
    if (now_ms() - taken >
        std::chrono::duration_cast<std::chrono::milliseconds>(kRetention).count()) {
        std::error_code ec;
        fs::remove_all(folder, ec);
        return gone();
    }
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(folder / "archive", ec);
    if (ec) return gone();
    BackupOutcome outcome;
    outcome.snapshot = id;
    outcome.archive = folder / "archive";
    outcome.size_bytes = size;
    if (const auto summary = summary_from_json(record->value("summary", json::object()))) {
        outcome.summary = *summary;
    }
    return outcome;
}

BackupOutcome BackupSnapshots::release(const std::string& id) {
    BackupOutcome found = find(id);
    if (!found.ok()) return found;
    std::error_code ec;
    fs::remove_all(directory_ / id, ec);
    if (ec) return refusal(BackupStatus::Failed, "could not remove snapshot '" + id + "': " + ec.message());
    BackupOutcome outcome;
    outcome.snapshot = id;
    return outcome;
}

std::size_t BackupSnapshots::expire(std::chrono::system_clock::time_point now) {
    std::error_code ec;
    if (!fs::is_directory(directory_, ec)) return 0;
    const std::int64_t now_epoch_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::int64_t retention_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(kRetention).count();
    std::vector<fs::path> stale;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory_, ec)) {
        std::error_code entry_error;
        if (!entry.is_directory(entry_error)) continue;
        const std::string name = entry.path().filename().string();
        if (!valid_id(name)) continue;
        if (const std::optional<json> record = read_json_file(entry.path() / "snapshot.json")) {
            const std::int64_t taken = record->contains("taken_ms") &&
                                               (*record)["taken_ms"].is_number_integer()
                                           ? (*record)["taken_ms"].get<std::int64_t>()
                                           : 0;
            if (now_epoch_ms - taken > retention_ms) stale.push_back(entry.path());
            continue;
        }
        // Half written (its writer stopped): by the folder's age.
        const auto written = fs::last_write_time(entry.path(), entry_error);
        if (!entry_error && fs::file_time_type::clock::now() - written > kRetention) {
            stale.push_back(entry.path());
        }
    }
    for (const fs::path& path : stale) fs::remove_all(path, ec);
    return stale.size();
}

fs::path restore_record_path(const fs::path& data_dir) {
    return data_dir / "backups" / "restore.json";
}

// ---- restoring a snapshot -------------------------------------------------------

SnapshotRestore::SnapshotRestore(BackupStores& stores, fs::path record_path)
    : stores_(stores), record_path_(std::move(record_path)) {}

bool SnapshotRestore::write_record(const json& state) {
    json record = state;
    record["node"] = stores_.node_id;
    record["started_ms"] = started_ms_;
    if (!snapshot_.empty()) record["snapshot"] = snapshot_;
    if (!source_node_.empty()) record["source_node"] = source_node_;
    return write_json_file(record_path_, record);
}

BackupOutcome SnapshotRestore::failed(BackupStatus status, std::string message) {
    failed_ = true;
    if (wrote_) {
        message +=
            ". This node now holds part of the archive: empty it with POST "
            "/v1/restore_snapshot:discard before restoring again";
        write_record({{"state", "failed"}, {"reason", message}});
    }
    return refusal(status, std::move(message));
}

BackupOutcome SnapshotRestore::begin() {
    started_ms_ = now_ms();
    const BackupSummary present = summarize_node(stores_);
    if (holds_user_data(present)) {
        BackupOutcome outcome = refusal(
            BackupStatus::Conflict,
            "this Zetta holds user data: a snapshot is restored only into a node that holds none");
        outcome.summary = present;
        outcome.details = {{"summary", summary_to_json(present)}};
        failed_ = true;
        return outcome;
    }
    if (!write_record({{"state", "restoring"}})) {
        failed_ = true;
        return refusal(BackupStatus::Failed,
                       "could not write the restore record " + record_path_.string());
    }
    return {};
}

BackupOutcome SnapshotRestore::feed(std::string_view bytes) {
    if (failed_) return refusal(BackupStatus::InvalidInput, "the restore has already failed");
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (stage_ == Stage::Done) {
            return failed(BackupStatus::InvalidInput, "the archive has bytes after its trailer");
        }
        if (stage_ == Stage::Header) {
            const std::size_t newline = bytes.find('\n', at);
            const std::size_t end = newline == std::string_view::npos ? bytes.size() : newline;
            pending_.append(bytes.substr(at, end - at));
            if (pending_.size() > kMaxHeaderBytes) {
                return failed(BackupStatus::InvalidInput,
                              "a frame header is longer than 1 MiB: this is not a Zetta backup "
                              "archive");
            }
            at = end;
            if (newline == std::string_view::npos) break;
            ++at;
            const std::string line = std::move(pending_);
            pending_.clear();
            if (BackupOutcome outcome = header_line(line); !outcome.ok()) return outcome;
            continue;
        }
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, bytes.size() - at));
        const std::string_view chunk = bytes.substr(at, take);
        hash_.update(chunk);
        at += take;
        remaining_ -= take;
        if (BackupOutcome outcome = payload(chunk); !outcome.ok()) return outcome;
        if (remaining_ == 0) {
            if (BackupOutcome outcome = complete_frame(); !outcome.ok()) return outcome;
            stage_ = Stage::Header;
        }
    }
    return {};
}

BackupOutcome SnapshotRestore::header_line(std::string_view line) {
    const json header = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (header.is_discarded() || !header.is_object() || !header.contains("kind") ||
        !header["kind"].is_string()) {
        return failed(BackupStatus::InvalidInput,
                      "a frame header is not JSON: this is not a Zetta backup archive");
    }
    const std::string kind = header["kind"].get<std::string>();
    if (!saw_header_ && kind != "header") {
        return failed(BackupStatus::InvalidInput,
                      "the archive does not start with its header frame: this is not a Zetta "
                      "backup archive");
    }
    if (kind == "end") {
        zetta::util::Sha256 digest = hash_;
        if (string_field(header, "sha256") != digest.hex()) {
            return failed(BackupStatus::InvalidInput,
                          "the archive is damaged: its checksum is not the one its trailer "
                          "records");
        }
        if (!saw_catalogue_ || !saw_manifest_) {
            return failed(BackupStatus::InvalidInput, "the archive has no catalogue or no manifest");
        }
        stage_ = Stage::Done;
        return {};
    }
    hash_.update(line);
    hash_.update("\n");
    if (!header.contains("bytes") || !header["bytes"].is_number_unsigned()) {
        return failed(BackupStatus::InvalidInput, "frame '" + kind + "' has no byte count");
    }
    remaining_ = header["bytes"].get<std::uint64_t>();
    kind_ = kind;
    header_ = header;
    pending_.clear();
    if (kind == "object") {
        if (!saw_catalogue_) {
            return failed(BackupStatus::InvalidInput, "an object frame comes before the catalogue");
        }
        object_bucket_ = string_field(header, "bucket");
        object_key_ = string_field(header, "key");
        object_size_ = remaining_;
        object_slice_.clear();
        upload_.reset();
        if (!valid_bucket_name(object_bucket_) || !valid_object_key(object_key_)) {
            return failed(BackupStatus::InvalidInput, "an object frame names no valid bucket and key");
        }
        if (object_size_ > ObjectStore::kSliceBytes) {
            ObjectUpload upload;
            const ObjectOutcome begun = stores_.objects.begin_upload(
                object_bucket_, object_key_, ObjectWrite{}, object_size_, upload);
            if (!begun.ok()) {
                return failed(BackupStatus::Failed, "object '" + object_key_ + "' of bucket '" +
                                                        object_bucket_ +
                                                        "' could not be written: " + begun.message);
            }
            upload_ = std::move(upload);
        }
    } else if (kind == "header" || kind == "catalogue" || kind == "records" || kind == "body" ||
               kind == "manifest") {
        if (remaining_ > kMaxBufferedFrame) {
            return failed(BackupStatus::InvalidInput,
                          "frame '" + kind + "' is larger than this engine restores");
        }
        pending_.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(remaining_, 64u << 20)));
    } else {
        return failed(BackupStatus::InvalidInput,
                      "the archive has a frame of kind '" + kind +
                          "', which this engine does not know; restore it with the engine that "
                          "took it or a newer one");
    }
    stage_ = Stage::Payload;
    if (remaining_ == 0) {
        BackupOutcome outcome = complete_frame();
        stage_ = Stage::Header;
        return outcome;
    }
    return {};
}

BackupOutcome SnapshotRestore::payload(std::string_view bytes) {
    if (kind_ == "object") return object_bytes(bytes);
    pending_.append(bytes);
    return {};
}

BackupOutcome SnapshotRestore::object_bytes(std::string_view bytes) {
    object_slice_.append(bytes);
    while (upload_ && object_slice_.size() >= ObjectStore::kSliceBytes) {
        const ObjectOutcome part = stores_.objects.upload_part(
            *upload_, std::string_view(object_slice_).substr(0, ObjectStore::kSliceBytes));
        if (!part.ok()) {
            stores_.objects.abort_upload(*upload_);
            upload_.reset();
            return failed(BackupStatus::Failed, "object '" + object_key_ + "' of bucket '" +
                                                    object_bucket_ +
                                                    "' could not be written: " + part.message);
        }
        object_slice_.erase(0, ObjectStore::kSliceBytes);
        wrote_ = true;
    }
    return {};
}

BackupOutcome SnapshotRestore::complete_frame() {
    BackupOutcome outcome;
    if (kind_ == "header") {
        if (string_field(header_, "format") != kBackupFormat) {
            return failed(BackupStatus::InvalidInput,
                          "the archive's format is '" + string_field(header_, "format") +
                              "'; this engine restores " + std::string(kBackupFormat));
        }
        snapshot_ = string_field(header_, "snapshot");
        source_node_ = string_field(header_, "node");
        saw_header_ = true;
        write_record({{"state", "restoring"}});
    } else if (kind_ == "catalogue") {
        if (saw_catalogue_) return failed(BackupStatus::InvalidInput, "the archive has two catalogues");
        outcome = apply_catalogue();
        saw_catalogue_ = true;
    } else if (kind_ == "records") {
        if (!saw_catalogue_) {
            return failed(BackupStatus::InvalidInput, "a records frame comes before the catalogue");
        }
        outcome = apply_records();
    } else if (kind_ == "object") {
        ObjectOutcome written;
        if (upload_) {
            if (!object_slice_.empty()) written = stores_.objects.upload_part(*upload_, object_slice_);
            if (written.ok()) written = stores_.objects.commit_upload(*upload_, /*if_absent=*/false);
            if (!written.ok()) stores_.objects.abort_upload(*upload_);
            upload_.reset();
        } else {
            written = stores_.objects.put(object_bucket_, object_key_, object_slice_, ObjectWrite{});
        }
        object_slice_.clear();
        if (!written.ok()) {
            return failed(BackupStatus::Failed, "object '" + object_key_ + "' of bucket '" +
                                                    object_bucket_ +
                                                    "' could not be written: " + written.message);
        }
        wrote_ = true;
        restored_buckets_.insert(object_bucket_);
    } else if (kind_ == "body") {
        if (!saw_catalogue_) {
            return failed(BackupStatus::InvalidInput, "a body frame comes before the catalogue");
        }
        const std::string id = string_field(header_, "id");
        if (!stores_.bodies.store_blob(id, pending_)) {
            return failed(BackupStatus::Failed,
                          "the body of " + id +
                              " was refused: its size or SHA-256 is not the one its catalogue "
                              "entry records");
        }
        wrote_ = true;
    } else if (kind_ == "manifest") {
        const json manifest = json::parse(pending_, nullptr, /*allow_exceptions=*/false);
        const std::optional<BackupSummary> summary =
            manifest.is_object() ? summary_from_json(manifest.value("summary", json()))
                                 : std::nullopt;
        if (!summary) return failed(BackupStatus::InvalidInput, "the archive's manifest is malformed");
        expected_ = *summary;
        saw_manifest_ = true;
    }
    pending_.clear();
    pending_.shrink_to_fit();
    return outcome;
}

BackupOutcome SnapshotRestore::apply_catalogue() {
    const json catalogue = json::parse(pending_, nullptr, /*allow_exceptions=*/false);
    if (catalogue.is_discarded() || !catalogue.is_object()) {
        return failed(BackupStatus::InvalidInput, "the archive's catalogue is not a JSON object");
    }
    std::vector<ReplicationWrite> writes;
    writes.reserve(catalogue.size());
    for (const auto& item : catalogue.items()) {
        if (!backed_up_key(item.key())) continue;
        json value = item.value();
        // A partition's body lives on this node now.
        if (std::string_view(item.key()).starts_with(catalog::kPartitionPrefix) &&
            value.is_object()) {
            value["owner_nodes"] = json::array({stores_.node_id});
        }
        writes.push_back({item.key(), std::move(value)});
    }
    if (!writes.empty()) wrote_ = true;
    write_catalogue(stores_.replication, writes);
    return {};
}

BackupOutcome SnapshotRestore::apply_records() {
    const std::string collection = string_field(header_, "collection");
    const std::int64_t now = now_ms();
    std::size_t from = 0;
    while (from < pending_.size()) {
        std::size_t end = pending_.find('\n', from);
        if (end == std::string::npos) end = pending_.size();
        const std::string_view text = std::string_view(pending_).substr(from, end - from);
        from = end + 1;
        if (text.empty()) continue;
        const std::optional<StoredRecord> record =
            record_from_json(json::parse(text, nullptr, /*allow_exceptions=*/false), collection);
        if (!record) {
            return failed(BackupStatus::InvalidInput,
                          "a record of '" + collection + "' in the archive is malformed");
        }
        // Past its retention now: a restore never writes an expired record.
        if (record->expires_ms && *record->expires_ms <= now) {
            ++expired_[collection];
            continue;
        }
        if (!stores_.records.import_record(*record)) {
            return failed(BackupStatus::Failed, "record '" + record->key + "' of '" + collection +
                                                    "' could not be written");
        }
        wrote_ = true;
    }
    return {};
}

BackupOutcome SnapshotRestore::finish() {
    if (failed_) return refusal(BackupStatus::InvalidInput, "the restore has already failed");
    if (stage_ != Stage::Done) {
        return failed(BackupStatus::InvalidInput,
                      "the archive ended before its trailer: it is truncated");
    }
    stores_.records.maintain();  // the imported records reach the disk
    for (const std::string& bucket : restored_buckets_) {
        const ObjectOutcome synced = stores_.objects.barrier(bucket);
        if (!synced.ok()) {
            return failed(BackupStatus::Failed,
                          "bucket '" + bucket + "' could not be synced: " + synced.message);
        }
    }
    // What the manifest counted, less the records that expired since the
    // snapshot (a restore does not write them).
    BackupSummary expected = expected_;
    json expired = json::object();
    for (const auto& [collection, count] : expired_) {
        const std::string name = "collection:" + collection;
        expected[name] -= count;
        if (expected[name] <= 0) expected.erase(name);
        expired[collection] = count;
    }
    const BackupSummary observed = summarize_node(stores_);
    const json differences = summary_differences(expected, observed);
    write_record({{"state", differences.empty() ? "restored" : "unverified"},
                  {"finished_ms", now_ms()},
                  {"summary", summary_to_json(observed)}});
    BackupOutcome outcome;
    outcome.snapshot = snapshot_;
    outcome.summary = observed;
    // The node the restore went into: what a discard names.
    outcome.details = {{"node_id", stores_.node_id}, {"expired_records", expired}};
    if (!differences.empty()) {
        outcome.status = BackupStatus::VerificationFailed;
        outcome.message =
            "the restored node does not hold what the archive's manifest counted; it is left as "
            "restored for inspection, and POST /v1/restore_snapshot:discard empties it";
        outcome.details = {{"expected", summary_to_json(expected)},
                           {"observed", summary_to_json(observed)},
                           {"differences", differences},
                           {"node_id", stores_.node_id},
                           {"expired_records", expired}};
    }
    return outcome;
}

// ---- emptying a scratch target ----------------------------------------------------

BackupOutcome discard_restored(BackupStores& stores, const fs::path& record_path,
                               bool restore_target, const DiscardRequest& request) {
    if (!restore_target) {
        return refusal(BackupStatus::Conflict,
                       "this node was not started as a restore target (--restore-target or "
                       "ZETTA_RESTORE_TARGET=1): discard empties a node, and is refused on any "
                       "other");
    }
    const std::optional<json> record = read_json_file(record_path);
    if (!record) {
        return refusal(BackupStatus::Conflict,
                       "this node holds no restore record: its data did not come from a restore, "
                       "and discard is refused");
    }
    if (request.node_id != stores.node_id) {
        return refusal(BackupStatus::InvalidInput,
                       "node_id must name this node, " + stores.node_id);
    }
    const std::string snapshot = string_field(*record, "snapshot");
    if (snapshot.empty() || request.snapshot != snapshot) {
        return refusal(BackupStatus::InvalidInput,
                       "snapshot must name the snapshot this node was restored from");
    }
    if (record->contains("summary")) {
        const std::optional<BackupSummary> restored = summary_from_json((*record)["summary"]);
        const BackupSummary now = summarize_node(stores);
        const json differences =
            restored ? summary_differences(*restored, now) : json{{"summary", "unreadable"}};
        if (!differences.empty()) {
            BackupOutcome outcome = refusal(
                BackupStatus::Conflict,
                "this node changed since its restore: discard is refused, so nothing written "
                "since is ever wiped");
            outcome.details = {{"differences", differences}};
            return outcome;
        }
    }

    const std::shared_ptr<const json> catalogue = stores.replication.shared_snapshot();
    std::int64_t objects = 0;
    for (const std::string& bucket : bucket_names(*catalogue)) {
        ObjectWrite options;
        if (const std::optional<ObjectLease> lease = stores.objects.lease(bucket);
            lease && lease->fence > 0) {
            options.fence = lease->fence;
        }
        std::vector<std::string> keys;
        std::string after;
        do {
            const ObjectPage page =
                stores.objects.list(bucket, "", after, ObjectStore::kMaxListLimit);
            if (!page.ok()) {
                return refusal(BackupStatus::Failed,
                               "could not list bucket '" + bucket + "': " + page.message);
            }
            for (const ObjectInfo& info : page.objects) keys.push_back(info.key);
            after = page.next_after;
        } while (!after.empty());
        for (const std::string& key : keys) {
            const ObjectOutcome removed = stores.objects.remove(bucket, key, options);
            if (!removed.ok() && removed.status != ObjectStatus::NotFound) {
                return refusal(BackupStatus::Failed, "could not remove object '" + key +
                                                         "' of bucket '" + bucket +
                                                         "': " + removed.message);
            }
            ++objects;
        }
    }
    std::int64_t records = 0;
    for (const std::string& collection : collection_names(*catalogue)) {
        std::vector<std::string> keys;
        stores.records.export_records(collection, [&](const StoredRecord& stored) {
            keys.push_back(stored.key);
            return true;
        });
        for (const std::string& key : keys) {
            bool removed = true;
            if (collection.starts_with('_')) {
                removed = stores.records.remove_internal(collection, key);
            } else {
                RecordWrite write;
                write.collection = collection;
                write.key = key;
                write.remove = true;
                const RecordOutcome outcome = stores.records.write(write);
                removed = outcome.ok() || outcome.status == RecordStatus::NotFound;
            }
            if (!removed) {
                return refusal(BackupStatus::Failed, "could not remove record '" + key + "' of '" +
                                                         collection + "'");
            }
            ++records;
        }
    }
    // Every catalogue entry a restore writes, and the buckets' leases; the record
    // collection and metric definitions stay (the engine declares some itself).
    std::vector<ReplicationWrite> removals;
    for (const auto& item : catalogue->items()) {
        const std::string& key = item.key();
        const std::string family = family_of(key);
        if ((backed_up_key(key) && family != "record_collection" && family != "metric") ||
            std::string_view(key).starts_with(catalog::kObjectLeasePrefix)) {
            removals.push_back({key, std::nullopt});
        }
    }
    const std::int64_t entries = static_cast<std::int64_t>(removals.size());
    write_catalogue(stores.replication, removals);
    std::error_code ec;
    fs::remove(record_path, ec);

    BackupOutcome outcome;
    outcome.snapshot = snapshot;
    outcome.summary = summarize_node(stores);
    outcome.message = "emptied";
    outcome.details = {{"catalogue_entries", entries}, {"objects", objects}, {"records", records}};
    return outcome;
}

}  // namespace zetta::data
