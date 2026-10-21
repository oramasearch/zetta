#pragma once

#include <string>
#include <string_view>

// **Internal** — the write-word refusal shared by the connectors whose source
// has no read-only mode the connector can switch on (#1014): lake's SQLite,
// DuckDB and Polars services, and TiDB (which parses `READ ONLY` but does not
// enforce it). Not a public header; nothing outside `src/core/connect/`
// includes it.
//
// It refuses a statement in which a word that writes, changes the schema, or
// changes the session or transaction appears **anywhere outside a string, a
// quoted identifier or a comment** — not only at the start, because SQLite,
// DuckDB, MySQL and TiDB all accept `WITH … DELETE`.
//
// The scan is built to err towards refusing. A quote form or comment form one
// engine does not share is scanned as code. Because a server's string escapes
// are not known from the client (MySQL's backslash escapes depend on
// `sql_mode`; DuckDB has `E'…'` strings), the statement is scanned twice, with
// and without backslash escapes, and refused if either scan finds a word. `--`
// starts a comment only when followed by whitespace, as in MySQL (`1 --1` is
// arithmetic there).
namespace zetta::connect {

enum class WriteWordDialect {
    Sql,    // SQLite, DuckDB, Polars SQL
    Mysql,  // MySQL / TiDB: also LOAD DATA, HANDLER, LOCK, RENAME, FLUSH, …
};

// A reason naming the refused word and `through` (e.g. "a lake service
// connector"), or empty when the statement holds none.
std::string write_word_rejection(std::string_view sql, WriteWordDialect dialect,
                                 std::string_view through);

}  // namespace zetta::connect
