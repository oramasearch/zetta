#include "zetta/principal.hpp"

#include "zetta/replication.hpp"
#include "zetta/timestamp.hpp"
#include "zetta/uuid.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

std::string_view to_string(PrincipalKind kind) {
    switch (kind) {
        case PrincipalKind::User:    return "user";
        case PrincipalKind::Service: return "service";
        case PrincipalKind::Node:    return "node";
    }
    return "unknown";
}

std::optional<PrincipalKind> principal_kind_from(std::string_view name) {
    if (name == "user") return PrincipalKind::User;
    if (name == "service") return PrincipalKind::Service;
    if (name == "node") return PrincipalKind::Node;
    return std::nullopt;
}

Principal::Principal(std::string id, PrincipalKind kind, std::string name,
                     std::string created, fs::path dir,
                     ReplicationManager* replication)
    : id_(std::move(id)),
      kind_(kind),
      name_(std::move(name)),
      created_(std::move(created)),
      dir_(std::move(dir)),
      replication_(replication) {}

void Principal::set_metadata(PrincipalKind kind, std::string name,
                             std::string created) {
    kind_ = kind;
    name_ = std::move(name);
    created_ = std::move(created);
    persist_meta();
}

nlohmann::json Principal::metadata() const {
    return nlohmann::json{{"kind", to_string(kind_)},
                          {"name", name_},
                          {"created", created_}};
}

void Principal::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "principal.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

// ----- keys (owned by the principal; replicated as key:<uuid>) ---------------

void Principal::load() {
    keys_.clear();
    const fs::path root = keys_dir();
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "key.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: require an object carrying a well-formed scope;
        // skip anything malformed rather than load a key with broken access.
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("scope")) {
            continue;
        }
        Scope scope;
        if (!scope_from_json(meta["scope"], scope)) continue;
        // Capabilities are optional (an older key may lack them => no rights);
        // but a present-yet-malformed set fails fast rather than granting wrong
        // rights, so the whole key is skipped.
        CapabilitySet capabilities;
        if (meta.contains("capabilities") &&
            !capability_set_from_json(meta["capabilities"], capabilities)) {
            continue;
        }
        keys_.push_back(std::make_shared<AccessKey>(
            entry.path().filename().string(), id_, std::move(scope),
            std::move(capabilities), meta.value("created", std::string{}),
            entry.path()));
    }
    // Stable order: oldest key first (created is a fixed-width stamp).
    std::sort(keys_.begin(), keys_.end(), [](const auto& a, const auto& b) {
        return a->created() < b->created();
    });
}

std::shared_ptr<AccessKey> Principal::create_key(Scope scope,
                                                 CapabilitySet capabilities) {
    const std::string id = new_uuid();
    const std::string created = now_iso8601();
    if (replication_) {
        replication_->set(catalog::access_key_key(id),
                          nlohmann::json{{"principal", id_},
                                         {"scope", scope},
                                         {"capabilities", capabilities},
                                         {"created", created}});
    }
    return materialize_key(id, std::move(scope), std::move(capabilities),
                           created);
}

std::shared_ptr<AccessKey> Principal::materialize_key(std::string id,
                                                      Scope scope,
                                                      CapabilitySet capabilities,
                                                      std::string created) {
    if (auto existing = find_key(id)) return existing;
    const fs::path key_dir = keys_dir() / id;
    std::error_code ec;
    fs::create_directories(key_dir, ec);
    if (ec) return nullptr;
    auto key = std::make_shared<AccessKey>(std::move(id), id_, std::move(scope),
                                           std::move(capabilities),
                                           std::move(created), key_dir);
    key->persist_meta();
    keys_.push_back(key);
    return key;
}

std::shared_ptr<AccessKey> Principal::find_key(const std::string& id) const {
    for (const auto& key : keys_) {
        if (key->id() == id) return key;
    }
    return nullptr;
}

bool Principal::drop_key(const std::string& id) {
    auto key = find_key(id);
    if (!key) return false;
    if (replication_) replication_->remove(catalog::access_key_key(id));
    return materialize_drop_key(id);
}

bool Principal::materialize_drop_key(const std::string& id) {
    const auto it = std::find_if(
        keys_.begin(), keys_.end(),
        [&](const std::shared_ptr<AccessKey>& k) { return k->id() == id; });
    if (it == keys_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    keys_.erase(it);
    return true;
}

}  // namespace zetta
