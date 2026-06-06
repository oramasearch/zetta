#include "zetta/table.hpp"

#include "zetta/storage/jsonl_store.hpp"

#include <fstream>
#include <utility>

namespace zetta {

Table::Table(std::string id, std::string name, std::filesystem::path dir)
    : id_(std::move(id)),
      name_(std::move(name)),
      dir_(std::move(dir)),
      store_(std::make_unique<JsonlStore>(dir_)) {}

std::size_t Table::add_row(nlohmann::json doc) {
    store_->add(std::move(doc));
    return store_->size();
}

std::size_t Table::row_count() const { return store_->size(); }

void Table::persist_meta() const {
    const nlohmann::json meta{{"id", id_}, {"name", name_}};
    std::ofstream out(dir_ / "table.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta
