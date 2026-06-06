#include "zetta/table.hpp"

#include "zetta/storage/json_store.hpp"

#include <utility>

namespace zetta {

Table::Table(std::string name)
    : name_(std::move(name)), store_(std::make_unique<JsonStore>()) {}

std::size_t Table::add_row(nlohmann::json doc) {
    store_->add(std::move(doc));
    return store_->size();
}

std::size_t Table::row_count() const { return store_->size(); }

}  // namespace zetta
