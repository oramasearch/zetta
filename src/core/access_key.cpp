#include "zetta/access_key.hpp"

#include <fstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

AccessKey::AccessKey(std::string id, std::string principal_id, Scope scope,
                     std::string created, fs::path dir)
    : id_(std::move(id)),
      principal_id_(std::move(principal_id)),
      scope_(std::move(scope)),
      created_(std::move(created)),
      dir_(std::move(dir)) {}

nlohmann::json AccessKey::metadata() const {
    return nlohmann::json{{"principal", principal_id_},
                          {"scope", scope_},
                          {"created", created_}};
}

void AccessKey::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "key.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

AccessDecision authorize(const AccessKey& key, const Resource& resource) {
    const std::string label =
        std::string(to_string(resource.kind)) + " '" + resource.name + "'";
    if (key.permits(resource)) {
        return AccessDecision{true, label + " is in scope"};
    }
    return AccessDecision{false, label + " is outside the key's scope"};
}

}  // namespace zetta
