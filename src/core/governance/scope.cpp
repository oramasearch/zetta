#include "zetta/governance/scope.hpp"

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

std::optional<Resource> resource_from_json(const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    if (!document.contains("kind") || !document["kind"].is_string()) {
        return std::nullopt;
    }
    if (!document.contains("name") || !document["name"].is_string()) {
        return std::nullopt;
    }
    ResourceKind kind{};
    if (!resource_kind_from_string(document["kind"].get<std::string>(), kind)) {
        return std::nullopt;
    }
    return Resource{kind, document["name"].get<std::string>()};
}

std::optional<Scope> scope_from_json(const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    if (!document.contains("unrestricted") ||
        !document["unrestricted"].is_boolean()) {
        return std::nullopt;
    }
    if (!document.contains("grants") || !document["grants"].is_array()) {
        return std::nullopt;
    }
    Scope parsed;
    parsed.unrestricted = document["unrestricted"].get<bool>();
    parsed.grants.reserve(document["grants"].size());
    for (const auto& entry : document["grants"]) {
        const auto resource = resource_from_json(entry);
        if (!resource) return std::nullopt;
        parsed.grants.push_back(*resource);
    }
    return parsed;
}

}  // namespace zetta
