#include "zetta/table.hpp"

#include "zetta/replication.hpp"
#include "zetta/storage/jsonl_store.hpp"
#include "zetta/uuid.hpp"

#include "catalog.hpp"

#include <fstream>
#include <utility>

namespace zetta {

Table::Table(std::string id, std::string name, std::filesystem::path dir,
             ReplicationManager* replication,
             ReplicationStrategy replication_strategy)
    : id_(std::move(id)),
      name_(std::move(name)),
      dir_(std::move(dir)),
      store_(std::make_unique<JsonlStore>(dir_)),
      replication_(replication),
      replication_strategy_(std::move(replication_strategy)) {}

std::size_t Table::add_row(nlohmann::json doc) {
    const std::string row_id = new_uuid();
    // CRDT first (the source of truth), then the local store projection.
    if (replication_) {
        replication_->set(catalog::row_key(row_id),
                          nlohmann::json{{"tbl", id_}, {"data", doc}});
    }
    store_->add(row_id, std::move(doc));
    return store_->size();
}

bool Table::materialize_row(std::string row_id, nlohmann::json doc) {
    if (store_->contains(row_id)) return false;
    store_->add(std::move(row_id), std::move(doc));
    return true;
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

}  // namespace zetta
