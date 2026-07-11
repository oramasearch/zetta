#include "zetta/data/table.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/storage/jsonl_store.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

#include <fstream>
#include <utility>

namespace zetta::data {

Table::Table(std::string id, std::string name, std::filesystem::path dir,
             zetta::cluster::ReplicationManager* replication,
             ReplicationStrategy replication_strategy)
    : id_(std::move(id)),
      name_(std::move(name)),
      dir_(std::move(dir)),
      store_(std::make_unique<zetta::storage::JsonlStore>(dir_)),
      replication_(replication),
      replication_strategy_(std::move(replication_strategy)) {}

std::size_t Table::add_row(nlohmann::json doc) {
    const std::string row_id = zetta::util::new_uuid();
    // CRDT first (the source of truth), then the local store projection.
    if (replication_) {
        replication_->set(catalog::row_key(row_id),
                          nlohmann::json{{"tbl", id_}, {"data", doc}});
    }
    store_->add(row_id, std::move(doc));
    return store_->size();
}

void Table::update_row(const std::string& row_id, nlohmann::json doc) {
    if (!store_->contains(row_id)) return;
    // CRDT first (the source of truth), then the local store projection.
    if (replication_) {
        replication_->set(catalog::row_key(row_id),
                          nlohmann::json{{"tbl", id_}, {"data", doc}});
    }
    store_->update(row_id, std::move(doc));
}

void Table::delete_row(const std::string& row_id) {
    if (!store_->contains(row_id)) return;
    // A TOMBSTONE value (not a key removal) so the delete replicates as a normal
    // value change; a reconciling peer sees `deleted` and drops its local row.
    if (replication_) {
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
    std::ofstream out(dir_ / "table.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::data