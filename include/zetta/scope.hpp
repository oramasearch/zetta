#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {

// What kind of node-owned resource an access grant names. V1 (RULE-3 / ADR-6)
// scopes keys to the **coarse, top-level** resources — databases and projects.
// Finer tiers (per-table, per-dataset, per-row) layer on later by extending this
// enum without changing the Scope shape; team/partner/per-dataset tiers are
// deliberately deferred (see docs/access-control.md).
enum class ResourceKind { Database, Project };

// Canonical lowercase name of a resource kind ("database" / "project"). This is
// BOTH the display label AND the stored representation (the strings written to
// the CRDT key value + key.json), so it is a stable on-disk identifier.
std::string_view to_string(ResourceKind kind);

// Parse a stored/canonical kind name back to the enum; false (out untouched) on
// an unknown spelling — the boundary check for the enum domain.
bool resource_kind_from_string(std::string_view text, ResourceKind& out);

// A single resource an access request targets, and that a key's scope is
// matched against. V1 identifies a resource by its (kind, name) — the
// human-facing handle a key is granted against (e.g. the `churn` project).
struct Resource {
    ResourceKind kind = ResourceKind::Database;
    std::string name;

    bool operator==(const Resource&) const = default;
};

// A key's scope: the set of resources it may touch. The realization of RULE-3
// (key-scoped access, V1): **deny-by-default** — a resource is permitted only
// when it is explicitly granted, or when the scope is unrestricted.
//
// `unrestricted` is the "*" grant: an admin key that reaches every resource (so
// the in-scope set need not enumerate them). With it false (the default), only
// the resources in `grants` are in scope; everything else is denied.
struct Scope {
    bool unrestricted = false;
    std::vector<Resource> grants;

    // The access decision: does this scope permit `resource`? Deny-by-default —
    // true iff `unrestricted`, or `resource` exactly matches a granted (kind,
    // name). No wildcards within a kind in V1 (that is the deferred finer tier).
    bool permits(const Resource& resource) const;

    // Add a resource to the scope. Returns *this so grants can be chained when
    // building a scope by hand or in a test.
    Scope& grant(ResourceKind kind, std::string name);

    bool operator==(const Scope&) const = default;
};

// A scope that reaches everything — the admin/unrestricted scope.
Scope unrestricted_scope();

// JSON serialization. to_json are ADL hooks for nlohmann::json, so
// `nlohmann::json document = scope;` works.
//
//   Resource -> { "kind": "<kind>", "name": "<name>" }
//   Scope    -> { "unrestricted": <bool>, "grants": [ <Resource>... ] }
void to_json(nlohmann::json& document, const Resource& resource);
void to_json(nlohmann::json& document, const Scope& scope);

// Parse from JSON, validating the shape at the boundary so a malformed scope
// fails fast instead of silently widening or narrowing access (a
// security-sensitive boundary — see docs/access-control.md). Each returns
// std::nullopt on anything malformed: not an object, a missing or wrong-typed
// field, an unknown resource kind, or a non-array / malformed `grants`. Mirrors
// schemas/access_key.schema.json ($defs) — keep them in sync.
std::optional<Resource> resource_from_json(const nlohmann::json& document);
std::optional<Scope> scope_from_json(const nlohmann::json& document);

}  // namespace zetta
