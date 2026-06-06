#include "zetta/database.hpp"

#include <algorithm>
#include <utility>

namespace zetta {

Database::Database(std::string name) : name_(std::move(name)) {}

std::shared_ptr<Table> Database::create_table(std::string name) {
    if (name.empty() || find_table(name)) return nullptr;
    auto table = std::make_shared<Table>(std::move(name));
    tables_.push_back(table);
    return table;
}

std::shared_ptr<Table> Database::find_table(const std::string& name) const {
    for (const auto& t : tables_) {
        if (t->name() == name) return t;
    }
    return nullptr;
}

bool Database::drop_table(const std::string& name) {
    const auto it = std::find_if(
        tables_.begin(), tables_.end(),
        [&](const std::shared_ptr<Table>& t) { return t->name() == name; });
    if (it == tables_.end()) return false;
    tables_.erase(it);
    return true;
}

std::vector<std::string> Database::table_names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& t : tables_) names.push_back(t->name());
    return names;
}

}  // namespace zetta
