#include "zetta/project.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

Project::Project(std::string id, std::string name, std::string created,
                 std::string updated, std::string owner, fs::path dir)
    : id_(std::move(id)),
      name_(std::move(name)),
      created_(std::move(created)),
      updated_(std::move(updated)),
      owner_(std::move(owner)),
      dir_(std::move(dir)) {}

void Project::set_metadata(std::string name, std::string created,
                           std::string updated, std::string owner) {
    name_ = std::move(name);
    created_ = std::move(created);
    updated_ = std::move(updated);
    owner_ = std::move(owner);
    persist_meta();
}

nlohmann::json Project::metadata() const {
    return nlohmann::json{{"name", name_},
                          {"created", created_},
                          {"updated", updated_},
                          {"owner", owner_}};
}

void Project::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "project.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta
