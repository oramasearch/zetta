// SPDX-License-Identifier: MIT

#include "zetta/governance/policy.hpp"

#include <array>
#include <fstream>
#include <utility>

namespace zetta::governance {
namespace fs = std::filesystem;

namespace {

// The command classes a policy governs, canonical lowercase (Postgres FOR clause).
constexpr std::array<std::string_view, 4> kPolicyCommands = {"select", "insert",
                                                             "update", "all"};

}  // namespace

bool is_policy_command(std::string_view command) {
    for (const std::string_view known : kPolicyCommands) {
        if (command == known) return true;
    }
    return false;
}

nlohmann::json policy_value_to_json(const PolicyValue& value) {
    nlohmann::json meta{{"name", value.name},
                        {"database", value.database},
                        {"table", value.table},
                        {"command", value.command},
                        {"created", value.created}};
    // The predicates + roles are emitted only when present, so a minimal policy
    // (e.g. FOR SELECT USING(...) to every role) serializes compactly and a
    // predicate-less side stays absent rather than a JSON null on disk.
    if (!value.using_predicate.is_null()) {
        meta["using"] = value.using_predicate;
    }
    if (!value.check_predicate.is_null()) {
        meta["check"] = value.check_predicate;
    }
    if (!value.roles.empty()) {
        meta["roles"] = value.roles;
    }
    return meta;
}

std::optional<PolicyValue> policy_value_from_json(const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    for (const char* field : {"name", "database", "table", "command"}) {
        if (!document.contains(field) || !document[field].is_string()) {
            return std::nullopt;
        }
    }
    PolicyValue value;
    value.name = document["name"].get<std::string>();
    value.database = document["database"].get<std::string>();
    value.table = document["table"].get<std::string>();
    value.command = document["command"].get<std::string>();
    if (!is_policy_command(value.command)) return std::nullopt;
    // The predicate expressions are opaque to governance (the query layer owns
    // their grammar), but their envelope must be an object when present — a
    // non-object predicate is a corrupt record, not an empty one.
    if (document.contains("using")) {
        if (!document["using"].is_object()) return std::nullopt;
        value.using_predicate = document["using"];
    }
    if (document.contains("check")) {
        if (!document["check"].is_object()) return std::nullopt;
        value.check_predicate = document["check"];
    }
    if (document.contains("roles")) {
        if (!document["roles"].is_array()) return std::nullopt;
        for (const auto& role : document["roles"]) {
            if (!role.is_string()) return std::nullopt;
            value.roles.push_back(role.get<std::string>());
        }
    }
    value.created = document.value("created", std::string{});
    return value;
}

Policy::Policy(std::string id, PolicyValue value, fs::path dir)
    : id_(std::move(id)), value_(std::move(value)), dir_(std::move(dir)) {}

void Policy::set_value(PolicyValue value) {
    value_ = std::move(value);
    persist_meta();
}

nlohmann::json Policy::metadata() const { return policy_value_to_json(value_); }

void Policy::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "policy.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

const nlohmann::json& policy_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/policy.schema.json",
  "title": "Policy",
  "description": "A row-level security policy (FEAT-9 / #748, epic #743 item 5): Postgres-style RLS over one table for one command class, carrying a USING read filter and/or a WITH CHECK write filter that the SQL WHERE engine evaluates against the acting identity (current_user()/current_roles()). The on-disk policy.json projection of the replicated `policy:<uuid>` CRDT value. Mirrored by zetta::governance::Policy in include/zetta/governance/policy.hpp and enforced by policy_value_from_json() + Node::load_policies()/reconcile_policies() — keep them in sync. The predicate expressions are opaque objects here; zetta::query::PolicyPredicate owns their grammar + evaluation.",
  "type": "object",
  "required": ["name", "database", "table", "command"],
  "additionalProperties": false,
  "properties": {
    "id": {
      "description": "The policy's UUID — its CRDT key and on-disk folder name. Present in policy.json and omitted from the CRDT value (the key carries it).",
      "type": "string"
    },
    "name": {
      "description": "The policy's name (unique among policies on the node) — the DROP POLICY handle.",
      "type": "string"
    },
    "database": {
      "description": "The database owning the protected table.",
      "type": "string"
    },
    "table": {
      "description": "The table the policy protects.",
      "type": "string"
    },
    "command": {
      "description": "The command class the policy governs. SELECT filters reads (USING); INSERT/UPDATE filter writes (WITH CHECK); ALL covers both.",
      "type": "string",
      "enum": ["select", "insert", "update", "all"]
    },
    "using": {
      "description": "The read filter (USING predicate): a row is visible iff it satisfies it. Opaque to governance — zetta::query::PolicyPredicate owns its grammar + evaluation. Absent when the policy has no read filter; a SELECT/ALL policy without one admits no row (#765), never every row.",
      "type": "object"
    },
    "check": {
      "description": "The write filter (WITH CHECK predicate): a written row is admitted iff it satisfies it. Opaque to governance. Absent when the policy has no write filter; an INSERT/UPDATE policy with neither check nor using admits no written row (#765).",
      "type": "object"
    },
    "roles": {
      "description": "The role names the policy applies to. Absent/empty means it applies to every role.",
      "type": "array",
      "items": { "type": "string" }
    },
    "created": {
      "description": "ISO-8601 UTC creation stamp (millisecond precision, trailing 'Z').",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::governance
