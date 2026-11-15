// SPDX-License-Identifier: MIT

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

const nlohmann::json& access_key_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/access_key.schema.json",
  "title": "AccessKey",
  "description": "An access key (ENT-21): the login credential whose scope is the V1 key-scoped access model (RULE-3 / PERM-1 / ADR-6) and whose capabilities are the deny-by-default rights model (ENT-22). The on-disk key.json projection of the replicated `key:<uuid>` CRDT value, owned by a principal. Mirrored by zetta::AccessKey in include/zetta/access_key.hpp, zetta::Scope in include/zetta/scope.hpp, and zetta::CapabilitySet in include/zetta/capability.hpp; enforced by scope_from_json()/capability_set_from_json() and Principal::load() in src/core/principal.cpp — keep them in sync. A malformed scope or capability is rejected at the boundary so access never silently widens or narrows.",
  "type": "object",
  "required": ["principal", "scope"],
  "additionalProperties": false,
  "properties": {
    "id": {
      "description": "The key's UUID — its CRDT key and on-disk folder name. Present in key.json (where the folder name is the source of truth) and omitted from the CRDT value (the key carries it).",
      "type": "string"
    },
    "principal": {
      "description": "The id of the Principal this key authenticates as.",
      "type": "string"
    },
    "scope": { "$ref": "#/$defs/scope" },
    "capabilities": {
      "description": "The deny-by-default rights this key may exercise (ENT-22) — the verb/right axis, orthogonal to scope's resource axis. Optional; absent means no rights granted. An operation is permitted only by a matching, unexpired capability.",
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
      "description": "A deny-by-default grant of a specific API/data/orchestration right (ENT-22), optionally TTL'd. An operation is covered iff its (domain, action) match exactly and the capability has not expired.",
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
          "description": "The specific right within the domain (e.g. 'query', 'read', 'deploy', 'start_run'). The action taxonomy is open in V1.",
          "type": "string"
        },
        "expires_at": {
          "description": "Optional TTL: the ISO-8601 UTC instant (same fixed-width format as `created`) at/after which the capability is expired. Omitted means it never expires.",
          "type": "string"
        }
      }
    },
    "scope": {
      "description": "What the key permits — deny-by-default. A resource is in scope iff `unrestricted` is true or it appears in `grants`.",
      "type": "object",
      "required": ["unrestricted", "grants"],
      "additionalProperties": false,
      "properties": {
        "unrestricted": {
          "description": "The '*' grant: when true the key reaches every resource (an admin key) and `grants` need not enumerate them.",
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
      "description": "A single node-owned object a grant names. The coarse tiers are databases/projects; the fine-grained object tiers (#747) are table/column/blob/tool, which add the optional `database` (table/column) and `columns` (column) fields.",
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