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

// A single-column WHERE predicate: WHERE <column> <op> <value>.
struct WhereClause {
    std::string column;
    CompareOp op = CompareOp::Equal;
    nlohmann::json value;
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

struct CreateTableStmt {
    TableRef table;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    TableRef table;
};

struct InsertStmt {
    TableRef table;
    std::vector<std::string> columns;                // explicit column list
    std::vector<std::vector<nlohmann::json>> rows;   // one inner vector per VALUES tuple
};

struct SelectStmt {
    TableRef table;
    bool all = false;                  // SELECT *
    std::vector<std::string> columns;  // projection when !all (and not grouped)
    std::optional<WhereClause> where;

    // GROUP BY (the DDF aggregation path). `group_by` holds the grouping-key
    // columns (the DDF engine groups by exactly one); `items` is the ordered
    // select list for the grouped query (key passthroughs + aggregates), so result
    // columns come back in the order written. Both are empty for a plain SELECT.
    std::vector<std::string> group_by;
    std::vector<GroupedItem> items;

    // Whether this is an aggregating query (routes through the DDF engine).
    bool grouped() const { return !group_by.empty(); }
};

using Statement = std::variant<CreateDatabaseStmt, DropDatabaseStmt,
                               CreateUserStmt, CreateTokenStmt,
                               CreateTableStmt, DropTableStmt, InsertStmt,
                               SelectStmt>;

// Wrap a CreateTokenStmt into a Statement. Defined in its OWN translation unit
// (statement_builders.cpp), never inline: constructing this particular variant
// alternative inside the SQL parser's TU (src/core/query/postgres.cpp)
// miscompiles that TU's exception unwinding under the pinned Clang +
// -fexperimental-library — a thrown ParseError then escapes every catch and
// aborts. Building the variant here keeps the parser TU free of that construction
// (the parser just moves the returned Statement). Remove when the toolchain bug
// is gone.
Statement make_create_token_statement(std::string user, std::string type);

}  // namespace zetta::query
