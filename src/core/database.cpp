#include "zetta/database.hpp"

#include "zetta/uuid.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

Database::Database(std::string id, std::string name, fs::path dir)
    : id_(std::move(id)),
      name_(std::move(name)),
      dir_(std::move(dir)),
      large_objects_(dir_ / "large_objects") {}

void Database::set_name(std::string name) {
    name_ = std::move(name);
    persist_meta();
}

void Database::persist_meta() const {
    const nlohmann::json meta{{"id", id_}, {"name", name_}};
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
        tables_.push_back(std::make_shared<Table>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(), entry.path()));
    }
    // Stable order for the UI (directory iteration order is unspecified).
    std::sort(tables_.begin(), tables_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<Table> Database::create_table(std::string name) {
    if (name.empty() || find_table(name)) return nullptr;
    const std::string id = new_uuid();
    const fs::path table_dir = dir_ / id;
    std::error_code ec;
    fs::create_directories(table_dir, ec);
    if (ec) return nullptr;
    auto table = std::make_shared<Table>(id, std::move(name), table_dir);
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

bool Database::drop_table(const std::string& name) {
    const auto it = std::find_if(
        tables_.begin(), tables_.end(),
        [&](const std::shared_ptr<Table>& t) { return t->name() == name; });
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

}  // namespace zetta
