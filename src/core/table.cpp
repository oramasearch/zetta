#include "zetta/table.hpp"

#include "zetta/replication.hpp"
#include "zetta/storage/jsonl_store.hpp"
#include "zetta/uuid.hpp"

#include "catalog.hpp"

#include <fstream>
#include <utility>

namespace zetta {

Table::Table(std::string id, std::string name, std::filesystem::path dir,
             ReplicationManager* replication)
    : id_(std::move(id)),
      name_(std::move(name)),
      dir_(std::move(dir)),
      store_(std::make_unique<JsonlStore>(dir_)),
      replication_(replication) {}

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

void Table::persist_meta() const {
    const nlohmann::json meta{{"id", id_}, {"name", name_}};
    std::ofstream out(dir_ / "table.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta
