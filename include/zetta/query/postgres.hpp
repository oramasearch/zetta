#pragma once

#include <string_view>

#include "zetta/query/dialect.hpp"

namespace zetta::query {

// A PostgreSQL-flavored frontend. Supports a basic subset:
//   CREATE DATABASE <name>            DROP DATABASE <name>
//   CREATE TABLE [db.]<name> (cols)   DROP TABLE [db.]<name>
//   CREATE VIRTUAL TABLE <name> OVER CRDT '<prefix>' (col type TIER…, …)  (#809)
//   INSERT INTO [db.]<name> (cols) VALUES (...), (...)
//   SELECT * | col,... FROM [db.]<name> [WHERE col = value]
// Keywords are case-insensitive; '--' line comments and ';'-separated
// statements are allowed.
class PostgresDialect final : public Dialect {
public:
    const char* name() const override { return "postgres"; }
    ParseResult parse(std::string_view sql) const override;
};

}  // namespace zetta::query
