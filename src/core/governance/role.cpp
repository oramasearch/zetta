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
    return nlohmann::json{{"name", name_},
                          {"scope", permissions_.scope},
                          {"capabilities", permissions_.capabilities},
                          {"created", created_}};
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
    return RolePermissions{*scope, std::move(capabilities)};
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
      "description": "The deny-by-default rights this role grants (ENT-22). Optional; absent means no rights.",
      "type": "array",
      "items": { "$ref": "#/$defs/capability" }
    },
    "created": {
      "description": "ISO-8601 UTC creation stamp (millisecond precision, trailing 'Z').",
      "type": "string"
    }
  },
  "$defs": {
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
      "description": "A single node-owned resource a grant names (database or project).",
      "type": "object",
      "required": ["kind", "name"],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "description": "The kind of resource.",
          "type": "string",
          "enum": ["database", "project"]
        },
        "name": {
          "description": "The resource's name (its human-facing handle).",
          "type": "string"
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::governance
