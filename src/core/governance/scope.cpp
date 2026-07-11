#include "zetta/governance/scope.hpp"

#include <utility>

namespace zetta::governance {

std::string_view to_string(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::Database: return "database";
        case ResourceKind::Project:  return "project";
        case ResourceKind::Table:    return "table";
        case ResourceKind::Column:   return "column";
        case ResourceKind::Blob:     return "blob";
        case ResourceKind::Tool:     return "tool";
    }
    return "unknown";
}

bool resource_kind_from_string(std::string_view text, ResourceKind& out) {
    if (text == "database") { out = ResourceKind::Database; return true; }
    if (text == "project")  { out = ResourceKind::Project;  return true; }
    if (text == "table")    { out = ResourceKind::Table;    return true; }
    if (text == "column")   { out = ResourceKind::Column;   return true; }
    if (text == "blob")     { out = ResourceKind::Blob;     return true; }
    if (text == "tool")     { out = ResourceKind::Tool;     return true; }
    return false;
}

Resource Resource::table(std::string database, std::string table) {
    Resource r;
    r.kind = ResourceKind::Table;
    r.database = std::move(database);
    r.name = std::move(table);
    return r;
}

Resource Resource::column(std::string database, std::string table,
                          std::vector<std::string> columns) {
    Resource r;
    r.kind = ResourceKind::Column;
    r.database = std::move(database);
    r.name = std::move(table);
    r.columns = std::move(columns);
    return r;
}

Resource Resource::blob(std::string id_or_prefix) {
    Resource r;
    r.kind = ResourceKind::Blob;
    r.name = std::move(id_or_prefix);
    return r;
}

Resource Resource::tool(std::string name) {
    Resource r;
    r.kind = ResourceKind::Tool;
    r.name = std::move(name);
    return r;
}

namespace {
bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}
bool contains_column(const std::vector<std::string>& set, const std::string& col) {
    for (const std::string& c : set) {
        if (c == col) return true;
    }
    return false;
}
}  // namespace

bool object_covers(const Resource& granted, const Resource& target) {
    switch (target.kind) {
        case ResourceKind::Blob:
            // A blob grant is a PREFIX: it covers every id that starts with it.
            return granted.kind == ResourceKind::Blob &&
                   starts_with(target.name, granted.name);
        case ResourceKind::Tool:
            return granted.kind == ResourceKind::Tool &&
                   granted.name == target.name;
        case ResourceKind::Table:
            // A database grant covers all its tables; a table/column grant on the
            // same table makes the table reachable.
            if (granted.kind == ResourceKind::Database)
                return granted.name == target.database;
            if (granted.kind == ResourceKind::Table ||
                granted.kind == ResourceKind::Column)
                return granted.database == target.database &&
                       granted.name == target.name;
            return false;
        case ResourceKind::Column:
            // Covered by a db grant, a whole-table grant, or a column grant whose
            // set contains the requested column (target carries one column).
            if (granted.kind == ResourceKind::Database)
                return granted.name == target.database;
            if (granted.kind == ResourceKind::Table)
                return granted.database == target.database &&
                       granted.name == target.name;
            if (granted.kind == ResourceKind::Column)
                return granted.database == target.database &&
                       granted.name == target.name && !target.columns.empty() &&
                       contains_column(granted.columns, target.columns.front());
            return false;
        case ResourceKind::Database:
            return granted.kind == ResourceKind::Database &&
                   granted.name == target.name;
        case ResourceKind::Project:
            return granted.kind == ResourceKind::Project &&
                   granted.name == target.name;
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
    // Emit the object-tier fields only when they carry something, so a coarse
    // database/project resource serializes exactly as it did before #747.
    if (!resource.database.empty()) document["database"] = resource.database;
    if (!resource.columns.empty()) document["columns"] = resource.columns;
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
    Resource parsed;
    parsed.kind = kind;
    parsed.name = document["name"].get<std::string>();
    // Optional object-tier fields (#747). Present-yet-malformed rejects the whole
    // resource so a corrupt grant fails fast rather than silently widening reach.
    if (document.contains("database")) {
        if (!document["database"].is_string()) return std::nullopt;
        parsed.database = document["database"].get<std::string>();
    }
    if (document.contains("columns")) {
        if (!document["columns"].is_array()) return std::nullopt;
        for (const auto& entry : document["columns"]) {
            if (!entry.is_string()) return std::nullopt;
            parsed.columns.push_back(entry.get<std::string>());
        }
    }
    return parsed;
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

}  // namespace zetta::governance