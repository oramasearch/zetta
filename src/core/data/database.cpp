#include "zetta/data/database.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta::data {
namespace fs = std::filesystem;

Database::Database(std::string id, std::string name, fs::path dir,
                   zetta::cluster::ReplicationManager* replication,
                   ReplicationStrategy replication_strategy)
    : id_(std::move(id)),
      name_(std::move(name)),
      dir_(std::move(dir)),
      large_objects_(dir_ / "large_objects", id_, replication),
      replication_(replication),
      replication_strategy_(std::move(replication_strategy)) {}

void Database::set_name(std::string name) {
    name_ = std::move(name);
    persist_meta();
}

void Database::persist_meta() const {
    nlohmann::json meta{{"id", id_}, {"name", name_}};
    // Default (ReplicateToAll) is omitted, so database.json is byte-identical to
    // before a database carried a strategy — the default preserves behavior.
    if (!replication_strategy_.is_default()) {
        meta["replication"] = replication_strategy_.to_json();
    }
    std::ofstream out(dir_ / "database.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

void Database::load() {
    tables_.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "table.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        if (meta.is_discarded() || !meta.contains("name")) continue;
        // The partition manifest (#837) comes back with the table, so a restarted
        // node knows how many partitions its dataset has before the first gossip
        // round. A manifest that fails the schema is dropped, not trusted: a
        // corrupt count would fail every read of a healthy dataset.
        std::optional<DatasetManifestValue> manifest;
        if (const auto it = meta.find("manifest"); it != meta.end()) {
            manifest = parse_dataset_manifest_value(*it);
        }
        tables_.push_back(std::make_shared<Table>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(), entry.path(), replication_,
            replication_strategy_from_meta(meta),
            meta.value("partition_only", false), std::move(manifest),
            meta.value("source_format", std::string())));
    }
    // Stable order for the UI (directory iteration order is unspecified).
    std::sort(tables_.begin(), tables_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<Table> Database::create_table(
    std::string name, std::optional<ReplicationStrategy> replication_strategy,
    bool partition_only, std::string source_format) {
    if (name.empty() || find_table(name)) return nullptr;
    // No explicit strategy → inherit this database's (ENT-35).
    const ReplicationStrategy strategy =
        replication_strategy.value_or(replication_strategy_);
    const std::string id = zetta::util::new_uuid();
    if (replication_) {
        // The default (ReplicateToAll) is omitted, so the tbl: value is unchanged
        // from before tables carried a strategy. `partition_only` is likewise
        // omitted when false (#833).
        nlohmann::json value{{"db", id_}, {"name", name}};
        if (!strategy.is_default()) value["replication"] = strategy.to_json();
        if (partition_only) value["partition_only"] = true;
        // What the rows were loaded from (#1221); omitted when not recorded.
        if (!source_format.empty()) value["source_format"] = source_format;
        replication_->set(catalog::table_key(id), value);
    }
    return materialize_table(id, std::move(name), strategy, partition_only,
                             std::move(source_format));
}

std::shared_ptr<Table> Database::materialize_table(
    std::string id, std::string name, ReplicationStrategy strategy,
    bool partition_only, std::string source_format) {
    if (auto existing = find_table_by_id(id)) return existing;
    const fs::path table_dir = dir_ / id;
    std::error_code ec;
    fs::create_directories(table_dir, ec);
    if (ec) return nullptr;
    auto table = std::make_shared<Table>(std::move(id), std::move(name),
                                         table_dir, replication_,
                                         std::move(strategy), partition_only,
                                         std::nullopt, std::move(source_format));
    table->persist_meta();
    tables_.push_back(table);
    return table;
}

std::shared_ptr<Table> Database::find_table(const std::string& name) const {
    for (const auto& t : tables_) {
        if (t->name() == name) return t;
    }
    return nullptr;
}

std::shared_ptr<Table> Database::find_table_by_id(const std::string& id) const {
    for (const auto& t : tables_) {
        if (t->id() == id) return t;
    }
    return nullptr;
}

bool Database::drop_table(const std::string& name) {
    const auto it = std::find_if(
        tables_.begin(), tables_.end(),
        [&](const std::shared_ptr<Table>& t) { return t->name() == name; });
    if (it == tables_.end()) return false;
    if (replication_) {
        replication_->remove(catalog::table_key((*it)->id()));
        // The manifest is keyed by the table, so it goes with it (#837) —
        // otherwise a re-created table id would inherit a stale expected count.
        replication_->remove(catalog::manifest_key((*it)->id()));
    }
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    tables_.erase(it);
    return true;
}

bool Database::materialize_drop_table(const std::string& id) {
    const auto it = std::find_if(
        tables_.begin(), tables_.end(),
        [&](const std::shared_ptr<Table>& t) { return t->id() == id; });
    if (it == tables_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    tables_.erase(it);
    return true;
}

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& t : tables_) names.push_back(t->name());
    return names;
}

}  // namespace zetta::data