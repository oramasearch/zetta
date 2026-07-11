#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::governance {

// A row-level security policy (FEAT-9 / #748, epic #743 item 5) — Postgres-style
// RLS. A policy protects one table for one command class and carries a USING
// read filter and/or a WITH CHECK write filter, applied to the roles it names
// (or every role when none are named). The query executor ANDs the applicable
// USING predicates into a SELECT's row filter and enforces the WITH CHECK on an
// INSERT (deny-by-default; admin / unrestricted / the trusted-local session
// bypass). See docs/governance/access-control.md.
//
// A Policy is **replicated state** (the same capture/reconcile path as Role): the
// Automerge CRDT document is the source of truth (flat key `policy:<uuid>`), and
// this is the in-memory + on-disk projection — persisted in a UUID-named folder
// under the data dir with policy.json holding the metadata. The id is the folder
// name and never changes.
//
// The predicate expressions are stored as **opaque JSON** in `using_predicate` /
// `check_predicate`: the query layer (zetta::query::PolicyPredicate) owns parsing
// them from SQL, serializing them into this JSON, and evaluating them against a
// row + the acting identity. Governance only replicates + persists them — it
// validates the envelope (each predicate is an object or absent), not the
// predicate grammar; a predicate that fails to evaluate denies the row
// (fail-safe), so a corrupt predicate can never widen access.
struct PolicyValue {
    std::string name;                // display name (unique on the node)
    std::string database;            // the owning database
    std::string table;               // the table it protects
    std::string command;             // "select" | "insert" | "update" | "all"
    nlohmann::json using_predicate;  // read filter (USING); null/absent when none
    nlohmann::json check_predicate;  // write filter (WITH CHECK); null/absent when none
    std::vector<std::string> roles;  // role names it applies to (empty => all roles)
    std::string created;

    bool operator==(const PolicyValue&) const = default;
};

// The canonical command spellings a policy stores ("select"/"insert"/"update"/
// "all"). The single referenceable set the parser, the executor's engagement
// decision, and the boundary check share.
bool is_policy_command(std::string_view command);

// The replicated/on-disk value shape (the id lives in the CRDT key / folder name,
// so it isn't repeated here):
//   { name, database, table, command, using?, check?, roles?, created }
// The `using` / `check` / `roles` fields are omitted when empty, so a minimal
// policy serializes compactly.
nlohmann::json policy_value_to_json(const PolicyValue& value);

// Parse a policy value from JSON at the trust boundary (mirrors the other
// governance entities). std::nullopt on anything malformed — not an object, a
// missing/mis-typed required field, an unknown command, a non-object predicate,
// or a non-array/non-string `roles` — so a corrupt policy.json is skipped rather
// than enforced (and a broken policy never silently disables RLS).
std::optional<PolicyValue> policy_value_from_json(const nlohmann::json& document);

// The Policy JSON Schema (draft 2020-12) — the code-owned source of truth for the
// policy.json / replicated `policy:<uuid>` value shape; schemas/policy.schema.json
// is generated from it and drift-checked by tests/test_schemas.cpp.
const nlohmann::json& policy_schema();

class Policy {
public:
    Policy(std::string id, PolicyValue value, std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const PolicyValue& value() const { return value_; }
    const std::string& name() const { return value_.name; }
    const std::filesystem::path& dir() const { return dir_; }

    // Replace the value to match a (replicated) one, then rewrite policy.json. The
    // folder/id is unchanged.
    void set_value(PolicyValue value);

    // The replicated/on-disk value shape (no id — the key carries it).
    nlohmann::json metadata() const;

    // Write this policy's metadata (id + fields) to dir/policy.json.
    void persist_meta() const;

private:
    std::string id_;
    PolicyValue value_;
    std::filesystem::path dir_;
};

}  // namespace zetta::governance
