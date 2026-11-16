// SPDX-License-Identifier: MIT

#include "zetta/governance/role.hpp"

#include <fstream>
#include <utility>

#include "zetta/governance/permission.hpp"

namespace zetta::governance {
namespace fs = std::filesystem;

std::optional<RolePermissions> builtin_role_permissions(std::string_view name) {
    // The built-ins are defined purely in terms of the permission vocabulary
    // (#746): each is a named set of permission atoms, expanded to a
    // CapabilitySet here. Built-ins reach every resource (unrestricted scope) and
    // differ only in rights — scope granularity is a later slice (epic #743 #4).
    const auto permission_names = builtin_role_permission_names(name);
    if (!permission_names) return std::nullopt;
    RolePermissions permissions;
    permissions.scope = unrestricted_scope();
    permissions.capabilities = capabilities_for_permissions(*permission_names);
    return permissions;
}

const std::vector<std::string>& builtin_role_names() {
    static const std::vector<std::string> names = {"admin", "writer", "reader"};
    return names;
}

Role::Role(std::string id, std::string name, RolePermissions permissions,
           std::string created, fs::path dir)
    : id_(std::move(id)),
      name_(std::move(name)),
      permissions_(std::move(permissions)),
      created_(std::move(created)),
      dir_(std::move(dir)) {}

void Role::set_metadata(std::string name, RolePermissions permissions,
                        std::string created) {
    name_ = std::move(name);
    permissions_ = std::move(permissions);
    created_ = std::move(created);
    persist_meta();
}

nlohmann::json Role::metadata() const {
    nlohmann::json meta{{"name", name_},
                        {"scope", permissions_.scope},
                        {"capabilities", permissions_.capabilities},
                        {"created", created_}};
    // Emit the object-scoped grants (#747) only when the role has any, so a role
    // without fine-grained grants (every built-in) serializes exactly as before.
    if (!permissions_.scoped_grants.empty()) {
        meta["scoped_grants"] = permissions_.scoped_grants;
    }
    return meta;
}

void Role::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "role.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

std::optional<RolePermissions> role_permissions_from_json(
    const nlohmann::json& document) {
    if (!document.is_object() || !document.contains("scope")) return std::nullopt;
    const auto scope = scope_from_json(document["scope"]);
    if (!scope) return std::nullopt;
    // Capabilities are optional (a role may grant scope-reach with no rights, or
    // an older shape omits them); a present-yet-malformed set fails fast.
    CapabilitySet capabilities;
    if (document.contains("capabilities")) {
        const auto parsed = capability_set_from_json(document["capabilities"]);
        if (!parsed) return std::nullopt;
        capabilities = *parsed;
    }
    // Object-scoped grants (#747) are optional (an older shape / a role with only
    // flat rights omits them); a present-yet-malformed entry fails fast.
    std::vector<ScopedGrant> scoped_grants;
    if (document.contains("scoped_grants")) {
        if (!document["scoped_grants"].is_array()) return std::nullopt;
        for (const auto& entry : document["scoped_grants"]) {
            const auto grant = scoped_grant_from_json(entry);
            if (!grant) return std::nullopt;
            scoped_grants.push_back(*grant);
        }
    }
    return RolePermissions{*scope, std::move(capabilities),
                           std::move(scoped_grants)};
}

const nlohmann::json& role_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/role.schema.json",
  "title": "Role",
  "description": "A named permission set (FEAT-9 / #744): a Scope (which resources) + a CapabilitySet (which rights), granted to a user via UserRole. The on-disk role.json projection of the replicated `role:<uuid>` CRDT value. Mirrored by zetta::governance::Role in include/zetta/governance/role.hpp and enforced by role_permissions_from_json() + Node::load_roles()/reconcile_roles() — keep them in sync. The scope/capability sub-shapes mirror access_key.schema.json's $defs.",
  "type": "object",
  "required": ["name", "scope"],
  "additionalProperties": false,
  "properties": {
    "id": {
      "description": "The role's UUID — its CRDT key and on-disk folder name. Present in role.json and omitted from the CRDT value (the key carries it).",
      "type": "string"
    },
    "name": {
      "description": "The role's name (unique among roles), e.g. a built-in 'admin'/'writer'/'reader'.",
      "type": "string"
    },
    "scope": { "$ref": "#/$defs/scope" },
    "capabilities": {
      "description": "The deny-by-default FLAT (unscoped) rights this role grants (ENT-22) — granted on every object. Optional; absent means no flat rights.",
      "type": "array",
      "items": { "$ref": "#/$defs/capability" }
    },
    "scoped_grants": {
      "description": "Object-scoped grants (#747): rights bound to a specific object (table / column-set / blob prefix / MCP tool / database). Optional; absent means no object-scoped rights. Each narrows a permission to exactly the objects it covers.",
      "type": "array",
      "items": { "$ref": "#/$defs/scoped_grant" }
    },
    "created": {
      "description": "ISO-8601 UTC creation stamp (millisecond precision, trailing 'Z').",
      "type": "string"
    }
  },
  "$defs": {
    "scoped_grant": {
      "description": "A specific right (operation) bound to a specific object (#747).",
      "type": "object",
      "required": ["operation", "object"],
      "additionalProperties": false,
      "properties": {
        "operation": {
          "description": "The right this grant confers on the object.",
          "type": "object",
          "required": ["domain", "action"],
          "additionalProperties": false,
          "properties": {
            "domain": { "type": "string", "enum": ["api", "data", "orchestration"] },
            "action": { "type": "string" }
          }
        },
        "object": { "$ref": "#/$defs/resource" }
      }
    },
    "capability": {
      "description": "A deny-by-default grant of a specific API/data/orchestration right (ENT-22), optionally TTL'd.",
      "type": "object",
      "required": ["domain", "action"],
      "additionalProperties": false,
      "properties": {
        "domain": {
          "description": "The governance domain of the right.",
          "type": "string",
          "enum": ["api", "data", "orchestration"]
        },
        "action": {
          "description": "The specific right within the domain (e.g. 'query', 'read', 'deploy', 'start_run', 'manage_users').",
          "type": "string"
        },
        "expires_at": {
          "description": "Optional TTL: the ISO-8601 UTC instant at/after which the capability is expired. Omitted means it never expires.",
          "type": "string"
        }
      }
    },
    "scope": {
      "description": "What the role permits — deny-by-default. A resource is in scope iff `unrestricted` is true or it appears in `grants`.",
      "type": "object",
      "required": ["unrestricted", "grants"],
      "additionalProperties": false,
      "properties": {
        "unrestricted": {
          "description": "The '*' grant: when true the role reaches every resource.",
          "type": "boolean"
        },
        "grants": {
          "description": "The explicit in-scope resources. Order is preserved.",
          "type": "array",
          "items": { "$ref": "#/$defs/resource" }
        }
      }
    },
    "resource": {
      "description": "A single node-owned object a grant names. The coarse tiers are database/project; the fine-grained object tiers (#747) are table/column/blob/tool, which add the optional `database` (table/column) and `columns` (column) fields.",
      "type": "object",
      "required": ["kind", "name"],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "description": "The kind of object.",
          "type": "string",
          "enum": ["database", "project", "table", "column", "blob", "tool", "topic", "collection", "bucket"]
        },
        "name": {
          "description": "The object's handle: database/project name, table name (table/column), blob id-or-prefix, or MCP tool name.",
          "type": "string"
        },
        "database": {
          "description": "The owning database, for the table/column kinds.",
          "type": "string"
        },
        "columns": {
          "description": "The granted column set, for the column kind.",
          "type": "array",
          "items": { "type": "string" }
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::governance
