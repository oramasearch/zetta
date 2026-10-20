#include "zetta/data/table.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/storage/jsonl_store.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zetta::data {

Table::Table(std::string id, std::string name, std::filesystem::path dir,
             zetta::cluster::ReplicationManager* replication,
             ReplicationStrategy replication_strategy, bool partition_only,
             std::optional<DatasetManifestValue> partition_manifest,
             std::string source_format)
    : id_(std::move(id)),
      name_(std::move(name)),
      dir_(std::move(dir)),
      store_(std::make_unique<zetta::storage::JsonlStore>(dir_)),
      replication_(replication),
      replication_strategy_(std::move(replication_strategy)),
      partition_only_(partition_only),
      partition_manifest_(std::move(partition_manifest)),
      source_format_(std::move(source_format)) {}

std::size_t Table::add_row(nlohmann::json doc) {
    const std::string row_id = zetta::util::new_uuid();
    // CRDT first (the source of truth), then the local store projection. A
    // partition-only table (#833) skips the capture: its rows belong to the
    // partition bodies its owners hold, and replicating them would put the same
    // data on the wire a second time.
    if (replication_ && !partition_only_) {
        replication_->set(catalog::row_key(row_id),
                          nlohmann::json{{"tbl", id_}, {"data", doc}});
    }
    store_->add(row_id, std::move(doc));
    return store_->size();
}

void Table::update_row(const std::string& row_id, nlohmann::json doc) {
    if (!store_->contains(row_id)) return;
    // CRDT first (the source of truth), then the local store projection.
    if (replication_ && !partition_only_) {
        replication_->set(catalog::row_key(row_id),
                          nlohmann::json{{"tbl", id_}, {"data", doc}});
    }
    store_->update(row_id, std::move(doc));
}

void Table::delete_row(const std::string& row_id) {
    if (!store_->contains(row_id)) return;
    // A TOMBSTONE value (not a key removal) so the delete replicates as a normal
    // value change; a reconciling peer sees `deleted` and drops its local row.
    // A partition-only row was never captured, so there is nothing to tombstone.
    if (replication_ && !partition_only_) {
        replication_->set(catalog::row_key(row_id),
                          nlohmann::json{{"tbl", id_}, {"deleted", true}});
    }
    store_->remove(row_id);
}

bool Table::materialize_row(std::string row_id, nlohmann::json doc) {
    if (store_->contains(row_id)) {
        store_->update(row_id, std::move(doc));  // upsert — apply a peer's edit
        return false;
    }
    store_->add(std::move(row_id), std::move(doc));
    return true;
}

void Table::materialize_row_delete(const std::string& row_id) {
    store_->remove(row_id);
}

std::size_t Table::row_count() const { return store_->size(); }

void Table::release_local_rows() {
    if (!partition_only_) return;
    // Free the rows first, then remove the files they were appended to, then
    // open an empty store over the directory. A JsonlStore keeps only `*.jsonl`
    // chunk files there; table.json is the table's own metadata and stays.
    store_.reset();
    std::error_code error;
    std::vector<std::filesystem::path> chunks;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, error)) {
        if (entry.path().extension() == ".jsonl") chunks.push_back(entry.path());
    }
    for (const std::filesystem::path& chunk : chunks) std::filesystem::remove(chunk, error);
    store_ = std::make_unique<zetta::storage::JsonlStore>(dir_);
}

void Table::set_partition_manifest(DatasetManifestValue value) {
    value.table = id_;  // the key names the table; the value cannot disagree
    // CRDT first (the source of truth), then the on-disk projection — the same
    // order as DatasetPartition::set_owner_nodes. The manifest lives under its
    // OWN key rather than inside `tbl:`, because a co-launched ingest has every
    // participant capture `tbl:<id>` when it adopts the table, and a value with
    // several concurrent writers can lose a merge.
    if (replication_) {
        replication_->set(catalog::manifest_key(id_), to_json(value));
    }
    partition_manifest_ = std::move(value);
    persist_meta();
}

void Table::apply_partition_manifest(std::optional<DatasetManifestValue> value) {
    if (partition_manifest_ == value) return;
    partition_manifest_ = std::move(value);
    persist_meta();  // projection only — no capture (no feedback loop)
}

namespace {

// Drop repeated names (the first place wins) and empty ones.
std::vector<std::string> distinct_columns(std::vector<std::string> columns) {
    std::vector<std::string> kept;
    kept.reserve(columns.size());
    for (std::string& column : columns) {
        if (column.empty()) continue;
        if (std::find(kept.begin(), kept.end(), column) != kept.end()) continue;
        kept.push_back(std::move(column));
    }
    return kept;
}

}  // namespace

void Table::set_column_order(std::vector<std::string> columns) {
    columns = distinct_columns(std::move(columns));
    if (columns.empty()) return;
    // CRDT first (the source of truth), then the projection, as the manifest.
    if (replication_) {
        replication_->set(catalog::column_order_key(id_),
                          nlohmann::json{{"columns", columns}});
    }
    column_order_ = std::move(columns);
    persist_meta();
}

void Table::apply_column_order(std::vector<std::string> columns) {
    columns = distinct_columns(std::move(columns));
    if (column_order_ == columns) return;
    column_order_ = std::move(columns);
    persist_meta();  // projection only — no capture (no feedback loop)
}

void Table::restore_column_order(std::vector<std::string> columns) {
    column_order_ = distinct_columns(std::move(columns));
}

std::vector<std::string> order_columns(const std::vector<std::string>& recorded,
                                       const std::vector<std::string>& seen) {
    if (recorded.empty()) return seen;
    std::vector<std::string> ordered = recorded;
    std::unordered_set<std::string> named(recorded.begin(), recorded.end());
    for (const std::string& column : seen) {
        if (named.insert(column).second) ordered.push_back(column);
    }
    return ordered;
}

void Table::set_partitioner(std::shared_ptr<Partitioner> partitioner) {
    partitioner_ = std::move(partitioner);
}

void Table::persist_meta() const {
    nlohmann::json meta{{"id", id_}, {"name", name_}};
    // Default (ReplicateToAll) is omitted, so table.json is byte-identical to
    // before a table carried a strategy — the default preserves current behavior.
    if (!replication_strategy_.is_default()) {
        meta["replication"] = replication_strategy_.to_json();
    }
    // Likewise omitted when false, so a normal table's table.json is unchanged.
    if (partition_only_) meta["partition_only"] = true;
    // Omitted when not recorded (#1221), so other tables' table.json is unchanged.
    if (!source_format_.empty()) meta["source_format"] = source_format_;
    // The partition manifest (#837) is persisted beside the rest so a restarted
    // node knows how many partitions its dataset has before the first gossip
    // round — and so a node with no ReplicationManager has one at all.
    if (partition_manifest_) {
        meta["manifest"] = to_json(*partition_manifest_);
    }
    // The source's column order (#1021); omitted when none was recorded.
    if (!column_order_.empty()) meta["column_order"] = column_order_;
    std::ofstream out(dir_ / "table.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::data