#include "zetta/governance/access_key.hpp"

#include <fstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta::governance {
namespace fs = std::filesystem;

AccessKey::AccessKey(std::string id, std::string principal_id, Scope scope,
                     CapabilitySet capabilities, std::string created, fs::path dir)
    : id_(std::move(id)),
      principal_id_(std::move(principal_id)),
      scope_(std::move(scope)),
      capabilities_(std::move(capabilities)),
      created_(std::move(created)),
      dir_(std::move(dir)) {}

nlohmann::json AccessKey::metadata() const {
    return nlohmann::json{{"principal", principal_id_},
                          {"scope", scope_},
                          {"capabilities", capabilities_},
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

AccessDecision authorize(const AccessKey& key, const Operation& operation,
                         std::string_view now) {
    const std::string label = std::string(to_string(operation.domain)) +
                              " operation '" + operation.action + "'";
    if (key.permits(operation, now)) {
        return AccessDecision{true, label + " is permitted by a capability"};
    }
    // Distinguish an expired grant from one that never existed, for the audit
    // reason — both are denied (deny-by-default).
    if (key.capabilities().has_expired_match(operation, now)) {
        return AccessDecision{false,
                              label + "'s capability has expired (TTL elapsed)"};
    }
    return AccessDecision{false, label + " has no matching capability"};
}

}  // namespace zetta::governance