#include "zetta/scope.hpp"

#include <utility>

namespace zetta {

std::string_view to_string(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::Database: return "database";
        case ResourceKind::Project:  return "project";
    }
    return "unknown";
}

bool resource_kind_from_string(std::string_view text, ResourceKind& out) {
    if (text == "database") {
        out = ResourceKind::Database;
        return true;
    }
    if (text == "project") {
        out = ResourceKind::Project;
        return true;
    }
    return false;
}

bool Scope::permits(const Resource& resource) const {
    if (unrestricted) return true;
    for (const auto& granted : grants) {
        if (granted == resource) return true;
    }
    return false;
}

Scope& Scope::grant(ResourceKind kind, std::string name) {
    grants.push_back(Resource{kind, std::move(name)});
    return *this;
}

Scope unrestricted_scope() {
    Scope scope;
    scope.unrestricted = true;
    return scope;
}

void to_json(nlohmann::json& document, const Resource& resource) {
    document = nlohmann::json{{"kind", to_string(resource.kind)},
                              {"name", resource.name}};
}

void to_json(nlohmann::json& document, const Scope& scope) {
    document = nlohmann::json{{"unrestricted", scope.unrestricted},
                              {"grants", scope.grants}};
}

bool resource_from_json(const nlohmann::json& document, Resource& out) {
    if (!document.is_object()) return false;
    if (!document.contains("kind") || !document["kind"].is_string()) return false;
    if (!document.contains("name") || !document["name"].is_string()) return false;
    ResourceKind kind{};
    if (!resource_kind_from_string(document["kind"].get<std::string>(), kind)) {
        return false;
    }
    out = Resource{kind, document["name"].get<std::string>()};
    return true;
}

bool scope_from_json(const nlohmann::json& document, Scope& out) {
    if (!document.is_object()) return false;
    if (!document.contains("unrestricted") ||
        !document["unrestricted"].is_boolean()) {
        return false;
    }
    if (!document.contains("grants") || !document["grants"].is_array()) {
        return false;
    }
    Scope parsed;
    parsed.unrestricted = document["unrestricted"].get<bool>();
    parsed.grants.reserve(document["grants"].size());
    for (const auto& entry : document["grants"]) {
        Resource resource;
        if (!resource_from_json(entry, resource)) return false;
        parsed.grants.push_back(std::move(resource));
    }
    out = std::move(parsed);
    return true;
}

}  // namespace zetta
