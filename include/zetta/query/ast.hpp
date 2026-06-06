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

// The only predicate supported for now: WHERE <column> = <value>.
struct WhereEq {
    std::string column;
    nlohmann::json value;
};

struct CreateDatabaseStmt {
    std::string name;
};

struct DropDatabaseStmt {
    std::string name;
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
    std::vector<std::string> columns;  // projection when !all
    std::optional<WhereEq> where;
};

using Statement = std::variant<CreateDatabaseStmt, DropDatabaseStmt,
                               CreateTableStmt, DropTableStmt, InsertStmt,
                               SelectStmt>;

}  // namespace zetta::query
