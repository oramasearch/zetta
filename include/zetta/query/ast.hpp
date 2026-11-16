// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

// The generic, dialect-neutral query representation.
//
// Every SQL dialect frontend (see dialect.hpp) parses its text into these
// statements, and the executor runs only these — so nothing about a specific
// dialect leaks into execution. To support a new database format, write a
// frontend that targets this AST; the executor and everything downstream are
// reused unchanged.
namespace zetta::query {

// A table reference, optionally qualified by database: "db.table" or "table".
// When `database` is empty the executor uses the caller's current database.
struct TableRef {
    std::string database;
    std::string table;
};

// A column definition from CREATE TABLE. `type` is captured but not yet
// enforced — the storage engine is schemaless JSON.
struct ColumnDef {
    std::string name;
    std::string type;
};

// The comparison a WHERE clause applies. Equality is the default (and the only
// one that works over non-numeric values); the ordering comparisons are numeric
// (or lexicographic over strings) — see the executor.
enum class CompareOp { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

// A WHERE predicate: either one comparison `<column> <op> <value>`, or AND / OR /
// NOT over other predicates (#912), e.g. `churned = true AND segment = 'MCS'`.
//
// A comparison (`kind == Compare`) uses `column` / `op` / `value`; a compound
// (`And`, `Or`, `Not`) uses `operands` instead (two or more for AND and OR,
// exactly one for NOT). The comparison fields come first, so the single
// comparison is still written `WhereClause{"col", CompareOp::Equal, value}`.
// Evaluation is SQL's three-valued logic (see row_matches in predicate.hpp).
struct WhereClause {
    enum class Kind { Compare, And, Or, Not };

    std::string column;
    CompareOp op = CompareOp::Equal;
    nlohmann::json value;
    Kind kind = Kind::Compare;
    std::vector<WhereClause> operands;
};

// An aggregate function in a GROUP BY query's select list. Maps 1:1 to the DDF
// engine's aggregate ops (Avg -> ddf::AggregateOp::Mean); the mapping lives in the
// executor so the AST stays independent of the DDF backend.
enum class AggregateFunc { Count, Sum, Min, Max, Avg };

// One aggregate item in a grouped SELECT: FUNC(column), or COUNT(*) (empty column).
struct AggregateExpr {
    AggregateFunc func = AggregateFunc::Count;
    std::string column;   // the argument column; empty for COUNT(*)
};

// One item of an aggregating SELECT's projection, kept in select order so result
// columns materialize in the order written. Either a grouping-key column
// passthrough (`aggregate` unset, `column` is the key) or an aggregate.
struct GroupedItem {
    std::optional<AggregateExpr> aggregate;  // set => aggregate; unset => key column
    std::string column;                      // the grouping-key column (when !aggregate)
    std::string alias;                       // optional output-name override (AS ...)
};

struct CreateDatabaseStmt {
    std::string name;
};

struct DropDatabaseStmt {
    std::string name;
};

// CREATE USER <name> — mint an authenticatable identity (FEAT-9 / #744).
struct CreateUserStmt {
    std::string name;
};

// CREATE TOKEN FOR <user> [TYPE access|api] — issue a bearer credential for a
// user; the executor returns the PLAINTEXT secret once (it is never stored). The
// parser always sets `type`, defaulting to "api" when TYPE is omitted.
struct CreateTokenStmt {
    std::string user;   // the owning user's name
    std::string type;   // "access" | "api" (the parser defaults to "api")
};

// CREATE ROLE <name> — mint a custom, empty role (FEAT-9 / #746); permissions are
// added with GRANT. Gated by the roles:manage permission at execution.
struct CreateRoleStmt {
    std::string name;
};

// DROP ROLE <name> — remove a role and its user assignments (#746). roles:manage.
struct DropRoleStmt {
    std::string name;
};

// The optional `ON <object>` scope of a GRANT/REVOKE (#747). Dialect-neutral: the
// executor translates it into a governance::Resource, keeping the AST free of the
// governance types. `RoleWide` (the default, no ON clause) is the pre-#747 flat
// grant — the permission on every object.
enum class GrantObjectKind { RoleWide, Database, Table, Columns, Blob, Tool, Collection, Bucket };

struct GrantObject {
    GrantObjectKind kind = GrantObjectKind::RoleWide;
    std::string database;              // Database / Table / Columns
    std::string table;                 // Table / Columns
    std::vector<std::string> columns;  // Columns
    std::string name;                  // Blob id-or-prefix / Tool name / Collection or Bucket prefix
};

// GRANT <perm>[, <perm>]* [ON <object>] TO ROLE <name> — add permissions from the
// vocabulary (permission.hpp) to a role's rights (#746); an ON clause narrows
// them to a specific object (#747). roles:manage.
struct GrantPermissionStmt {
    std::string role;
    std::vector<std::string> permissions;  // e.g. {"sql:read", "roles:manage"}
    GrantObject object;                     // RoleWide unless an ON clause is given
};

// REVOKE <perm>[, <perm>]* [ON <object>] FROM ROLE <name> — remove permissions
// from a role (#746/#747). roles:manage.
struct RevokePermissionStmt {
    std::string role;
    std::vector<std::string> permissions;
    GrantObject object;
};

// GRANT ROLE <role> TO <user> — assign a role to a user; the user's effective
// permissions become the union across its roles (#746). roles:manage.
struct GrantRoleStmt {
    std::string role;   // the role's name
    std::string user;   // the user's name
};

// REVOKE ROLE <role> FROM <user> — unassign a role from a user (#746). roles:manage.
struct RevokeRoleStmt {
    std::string role;
    std::string user;
};

// ----- Row-level security (RLS, #748, epic #743 item 5) --------------------

// A term in a row-security policy predicate: a row column, a constant literal, or
// a context function resolved at execution against the acting identity —
// current_user() (the acting user's name) or current_roles() (its role-name set,
// meaningful only inside ANY(...)). The dialect-neutral shape the executor
// evaluates; no governance types leak into the AST.
//
// A literal is carried as its raw TEXT plus a type tag rather than a parsed
// nlohmann::json, keeping the predicate AST lightweight and free of a json member
// in the SQL parser's translation unit (which carries the fragile EH workaround —
// see the CMakeLists note on postgres.cpp). The executor decodes (literal,
// literal_type) into a json value at evaluation time via compare_cell.
struct PolicyTerm {
    enum class Kind { Column, Literal, CurrentUser, CurrentRoles };
    enum class LiteralType { String, Number, Boolean, Null };
    Kind kind = Kind::Literal;
    std::string column;                              // Kind::Column — the row column
    std::string literal;                             // Kind::Literal — the raw text
    LiteralType literal_type = LiteralType::String;  // how to decode `literal`
};

// A row-security predicate (#748): ONE comparison `lhs <op> rhs`, evaluated with
// the same comparison semantics as a WHERE clause (the executor's compare_cell —
// no new predicate language). Either side may be a row column, a literal, or a
// context function. `rhs_any` marks the RHS as ANY(<array>) — a membership test
// (lhs is a member of the resolved array, e.g. 'admin' = ANY(current_roles())).
// `always_true` is USING (true) / WITH CHECK (true): a permissive predicate that
// admits every row.
struct PolicyPredicate {
    bool always_true = false;
    PolicyTerm lhs;
    CompareOp op = CompareOp::Equal;
    PolicyTerm rhs;
    bool rhs_any = false;
};

// The command class a policy governs (Postgres FOR clause). SELECT filters reads
// (USING); INSERT/UPDATE filter writes (WITH CHECK); ALL covers both.
enum class PolicyCommand { Select, Insert, Update, All };

// CREATE POLICY <name> ON <db.table> FOR SELECT|INSERT|UPDATE|ALL
//   [TO ROLE <role>[, <role>]*] USING (<predicate>) [WITH CHECK (<predicate>)]
// — a row-security policy (#748). Gated on the roles:manage permission at
// execution. `using_predicate` filters reads; `check_predicate` filters writes;
// either may be absent (the executor requires the one its command needs).
struct CreatePolicyStmt {
    std::string name;
    TableRef table;
    PolicyCommand command = PolicyCommand::All;
    std::vector<std::string> roles;                  // role names (empty => all roles)
    std::optional<PolicyPredicate> using_predicate;  // USING (...)
    std::optional<PolicyPredicate> check_predicate;  // WITH CHECK (...)
};

// DROP POLICY <name> [ON <db.table>] — remove a row-security policy (#748). The
// optional ON clause is accepted for symmetry with CREATE but ignored (policy
// names are unique on the node). roles:manage.
struct DropPolicyStmt {
    std::string name;
};

struct CreateTableStmt {
    TableRef table;
    std::vector<ColumnDef> columns;
};

// The tier a CREATE VIRTUAL TABLE column draws from (#809 slice 3). DURABLE is a
// field of the CRDT entry (replicated); the ephemeral tiers are enrichment.
// Mirrors data::VirtualColumnTier; the executor translates one to the other so
// the AST stays independent of the data layer.
enum class VirtualTableTier { Durable, EphemeralLocal, EphemeralSynced };

// One column of a CREATE VIRTUAL TABLE: a name + a captured (unenforced) type +
// its tier, with `is_key` set for the DURABLE KEY column (the CRDT key — the id
// after the prefix).
struct VirtualColumnDef {
    std::string name;
    std::string type;
    VirtualTableTier tier = VirtualTableTier::Durable;
    bool is_key = false;
};

// CREATE VIRTUAL TABLE <name> OVER CRDT '<prefix>' ( <col> <type> <TIER…>, … )
// — declare a read-only virtual table over the CRDT key-family `crdt_prefix`
// (#809 slice 3). OVER CRDT is REQUIRED (the must-extend-a-CRDT invariant). The
// executor persists it as durable catalog metadata (a `vtable:<name>` CRDT key)
// and projects one row per CRDT entry under the prefix at query time.
struct CreateVirtualTableStmt {
    std::string name;
    std::string crdt_prefix;
    std::vector<VirtualColumnDef> columns;
};

struct DropTableStmt {
    TableRef table;
};

struct InsertStmt {
    TableRef table;
    std::vector<std::string> columns;                // explicit column list
    std::vector<std::vector<nlohmann::json>> rows;   // one inner vector per VALUES tuple
};

// One ORDER BY key: a result column, ascending unless DESC. NULLs sort last
// ascending and first descending, as in Postgres.
struct OrderKey {
    std::string column;
    bool descending = false;
};

struct SelectStmt {
    TableRef table;
    bool all = false;                  // SELECT *
    std::vector<std::string> columns;  // projection when !all (and not grouped)
    std::optional<WhereClause> where;

    // Aggregation. `group_by` holds the grouping-key columns (one is supported;
    // empty for a whole-table aggregate such as `SELECT count(*) FROM t`); `items`
    // is the ordered select list of an aggregating query (key passthroughs +
    // aggregates), so result columns come back in the order written. `items` is
    // empty for a plain SELECT.
    std::vector<std::string> group_by;
    std::vector<GroupedItem> items;

    // ORDER BY / LIMIT / OFFSET, applied to the finished result of every path
    // (plain, grouped, system table, scattered over partitions). An ORDER BY key
    // names a column of the result: a projected column, a GROUP BY key or an
    // aggregate's alias; with SELECT *, any column. `limit` is set by `LIMIT n`
    // or `FETCH FIRST n ROWS ONLY`. A non-aggregating SELECT also pushes
    // `offset + limit` down to the scan (#1021): no partition owner sends more
    // rows than that, ORDER BY or not (each sorts its share and keeps the top).
    std::vector<OrderKey> order_by;
    std::optional<std::size_t> limit;
    std::size_t offset = 0;

    // Whether this is an aggregating query: it has a GROUP BY, an aggregate
    // function in its select list, or both (#1218). With no GROUP BY the whole
    // table is one group, and the answer is one row even over no rows.
    bool aggregating() const { return !items.empty(); }
};

// One SET assignment in an UPDATE: column = value.
struct Assignment {
    std::string column;
    nlohmann::json value;
};

// UPDATE <table> SET col = val [, col = val …] [WHERE …]. No WHERE => all rows.
struct UpdateStmt {
    TableRef table;
    std::vector<Assignment> assignments;
    std::optional<WhereClause> where;
};

// DELETE FROM <table> [WHERE …]. No WHERE => all rows.
struct DeleteStmt {
    TableRef table;
    std::optional<WhereClause> where;
};

// TRUNCATE [TABLE] <table> — remove every row (each tombstoned, so it replicates).
struct TruncateStmt {
    TableRef table;
};

using Statement = std::variant<CreateDatabaseStmt, DropDatabaseStmt,
                               CreateUserStmt, CreateTokenStmt,
                               CreateRoleStmt, DropRoleStmt,
                               GrantPermissionStmt, RevokePermissionStmt,
                               GrantRoleStmt, RevokeRoleStmt,
                               CreatePolicyStmt, DropPolicyStmt,
                               CreateTableStmt, DropTableStmt, InsertStmt,
                               CreateVirtualTableStmt,
                               UpdateStmt, DeleteStmt, TruncateStmt, SelectStmt>;

}  // namespace zetta::query
