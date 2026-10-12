#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::governance {

// What kind of node-owned resource an access grant names. The coarse, top-level
// tiers (RULE-3 / ADR-6) are `Database` / `Project`; the fine-grained object
// tiers (#747, epic #743 item 4) extend this so a grant can target a specific
// object — a `Table` (`db.table`), a `Column` set (`db.table(col_a,col_b)`), a
// `Blob` (a large-object id or an id prefix), or a `Tool` (an MCP tool name).
// The extra tiers add optional fields to `Resource` (below) without changing the
// Scope shape; see docs/governance/access-control.md.
// `Topic` (#1029) names a pub/sub topic by name or name prefix, like `Blob`.
// `Collection` (#1231) names a keyed record collection by name or name prefix.
// `Bucket` (#1005) names an object bucket by name or name prefix.
enum class ResourceKind { Database, Project, Table, Column, Blob, Tool, Topic, Collection, Bucket };

// Canonical lowercase name of a resource kind ("database" / "project" / "table"
// / "column" / "blob" / "tool"). This is BOTH the display label AND the stored
// representation (the strings written to the CRDT key value + key.json / role.json),
// so it is a stable on-disk identifier.
std::string_view to_string(ResourceKind kind);

// Parse a stored/canonical kind name back to the enum; false (out untouched) on
// an unknown spelling — the boundary check for the enum domain.
bool resource_kind_from_string(std::string_view text, ResourceKind& out);

// A single object an access request targets, and that a grant's scope is matched
// against. The coarse tiers (database/project) identify a resource by its (kind,
// name). The fine-grained object tiers (#747) reuse the same struct with a couple
// of optional fields:
//
//   kind      name                database        columns
//   database  db name             —               —
//   project   project name        —               —
//   table     table name          owning db       —
//   column    table name          owning db       the column set
//   blob      blob id / prefix     —               —
//   tool      MCP tool name        —               —
//
// `database` / `columns` are empty for the coarse kinds, so their JSON + equality
// are unchanged (backward compatible). `object_covers()` (below) is the covering
// decision a granted object makes over a requested target.
struct Resource {
    ResourceKind kind = ResourceKind::Database;
    std::string name;
    std::string database;              // table/column: the owning database
    std::vector<std::string> columns;  // column: the granted column set

    bool operator==(const Resource&) const = default;

    // Builders for the fine-grained object tiers (#747) — keep call sites terse
    // and the field packing in one place.
    static Resource table(std::string database, std::string table);
    static Resource column(std::string database, std::string table,
                           std::vector<std::string> columns);
    static Resource blob(std::string id_or_prefix);
    static Resource tool(std::string name);
    static Resource topic(std::string name_or_prefix);
    static Resource collection(std::string name_or_prefix);
    static Resource bucket(std::string name_or_prefix);
};

// Does a granted object cover a requested `target`? The deny-by-default object
// decision (#747), a superset of exact match:
//   - a database grant covers every table/column IN that database;
//   - a table grant covers the whole table (all its columns);
//   - a column-set grant covers the table for reachability and exactly its
//     columns for column-level checks;
//   - a blob grant covers every blob id that starts with its (id or) prefix;
//   - a tool grant covers exactly that tool;
//   - database/project otherwise match exactly (same kind + name).
// A `target` Column carrying a single column asks "is THIS column covered?".
bool object_covers(const Resource& granted, const Resource& target);

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
// `nlohmann::json document = scope;` works. `database` / `columns` are emitted
// only for the object tiers that use them, so a coarse database/project resource
// serializes exactly as before (backward compatible on disk + over the CRDT):
//
//   Resource -> { "kind": "<kind>", "name": "<name>",
//                 "database": "<db>"?, "columns": [ "<col>"... ]? }
//   Scope    -> { "unrestricted": <bool>, "grants": [ <Resource>... ] }
void to_json(nlohmann::json& document, const Resource& resource);
void to_json(nlohmann::json& document, const Scope& scope);

// Parse from JSON, validating the shape at the boundary so a malformed scope
// fails fast instead of silently widening or narrowing access (a
// security-sensitive boundary — see docs/governance/access-control.md). Each returns
// std::nullopt on anything malformed: not an object, a missing or wrong-typed
// field, an unknown resource kind, or a non-array / malformed `grants`. Mirrors
// schemas/access_key.schema.json ($defs) — keep them in sync.
std::optional<Resource> resource_from_json(const nlohmann::json& document);
std::optional<Scope> scope_from_json(const nlohmann::json& document);

}  // namespace zetta::governance