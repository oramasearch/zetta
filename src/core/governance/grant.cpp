#include "zetta/governance/grant.hpp"

#include <algorithm>

namespace zetta::governance {

void to_json(nlohmann::json& document, const ScopedGrant& grant) {
    document = nlohmann::json{
        {"operation",
         {{"domain", to_string(grant.operation.domain)},
          {"action", grant.operation.action}}},
        {"object", grant.object}};
}

std::optional<ScopedGrant> scoped_grant_from_json(const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    if (!document.contains("operation") || !document["operation"].is_object()) {
        return std::nullopt;
    }
    if (!document.contains("object")) return std::nullopt;
    const auto& op = document["operation"];
    if (!op.contains("domain") || !op["domain"].is_string()) return std::nullopt;
    if (!op.contains("action") || !op["action"].is_string()) return std::nullopt;
    CapabilityDomain domain{};
    if (!capability_domain_from_string(op["domain"].get<std::string>(), domain)) {
        return std::nullopt;
    }
    const auto object = resource_from_json(document["object"]);
    if (!object) return std::nullopt;
    return ScopedGrant{Operation{domain, op["action"].get<std::string>()},
                       *object};
}

bool ColumnAccess::allows(const std::string& column) const {
    if (!table_allowed) return false;
    if (all_columns) return true;
    return std::find(columns.begin(), columns.end(), column) != columns.end();
}

ColumnAccess column_access_for(bool global,
                               const std::vector<ScopedGrant>& grants,
                               const Operation& operation,
                               const std::string& database,
                               const std::string& table) {
    ColumnAccess access;
    if (global) {
        access.table_allowed = true;
        access.all_columns = true;
        return access;
    }
    for (const ScopedGrant& grant : grants) {
        if (!(grant.operation == operation)) continue;
        const Resource& g = grant.object;
        // A database or whole-table grant over this table opens every column.
        if ((g.kind == ResourceKind::Database && g.name == database) ||
            (g.kind == ResourceKind::Table && g.database == database &&
             g.name == table)) {
            access.table_allowed = true;
            access.all_columns = true;
            return access;
        }
        // A column-set grant on this table contributes its columns and makes the
        // table reachable (but keeps the column restriction).
        if (g.kind == ResourceKind::Column && g.database == database &&
            g.name == table) {
            access.table_allowed = true;
            for (const std::string& col : g.columns) {
                if (std::find(access.columns.begin(), access.columns.end(), col) ==
                    access.columns.end()) {
                    access.columns.push_back(col);
                }
            }
        }
    }
    return access;
}

bool object_permitted(bool global, const std::vector<ScopedGrant>& grants,
                      const Operation& operation, const Resource& object) {
    if (global) return true;
    for (const ScopedGrant& grant : grants) {
        if (!(grant.operation == operation)) continue;
        if (object_covers(grant.object, object)) return true;
    }
    return false;
}

}  // namespace zetta::governance
