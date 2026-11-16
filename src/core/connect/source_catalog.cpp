// SPDX-License-Identifier: MIT

#include "zetta/connect/source_catalog.hpp"

#include "lake_sql_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace zetta::connect {
namespace {

std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

enum class LiteralSyntax { Standard, BackslashEscapes, PostgresEscape };

// A single-quoted SQL literal. The inputs here are caller-supplied filters that
// go into a catalog query, so they are escaped rather than concatenated raw —
// the connector's read-only guard stops a second statement, but it is not an
// argument for building the first one carelessly. Snowflake and MySQL treat
// backslashes as escapes, so they must be doubled as well as quotes. Postgres
// uses E'…' so the encoding is independent of standard_conforming_strings.
// SQLite and DuckDB use ordinary SQL literals, where backslashes are text.
std::string sql_literal(const std::string& value,
                        LiteralSyntax syntax = LiteralSyntax::Standard) {
    std::string out = syntax == LiteralSyntax::PostgresEscape ? "E'" : "'";
    for (const char character : value) {
        if (character == '\'') out += '\'';
        if (syntax != LiteralSyntax::Standard && character == '\\') out += '\\';
        out += character;
    }
    out += "'";
    return out;
}

// A string cell, or empty — a catalog row read without trusting its types.
std::string text(const nlohmann::json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end() || found->is_null()) return {};
    if (found->is_string()) return found->get<std::string>();
    return found->dump();
}

// `is_nullable` is 'YES'/'NO' in information_schema, but a source may answer a
// boolean or 0/1 instead.
bool nullable(const nlohmann::json& row) {
    const auto found = row.find("is_nullable");
    if (found == row.end() || found->is_null()) return true;
    if (found->is_boolean()) return found->get<bool>();
    if (found->is_number()) return found->get<double>() != 0;
    if (found->is_string()) return lowered(found->get<std::string>()) != "no";
    return true;
}

// The two catalog statements for a source with a SQL catalog.
struct CatalogStatements {
    std::string tables;
    std::string columns;
};

CatalogStatements information_schema_statements(const ConnectorConfig& config,
                                                const SourceCatalogRequest& request) {
    // information_schema is ISO SQL and Postgres, Snowflake, MySQL and DuckDB
    // expose it, so one statement shape serves them. The row cap is applied by
    // the caller rather than with LIMIT, which the read-only guard need not
    // care about.
    //
    // MySQL differs in three places: 8.0 reports information_schema columns in
    // upper case unless aliased (so every column is aliased to the lower-case
    // name the reader expects), its own catalog schemas are `mysql`,
    // `performance_schema` and `sys`, and its literals take backslash escapes.
    // TiDB is read the same way; it also has `metrics_schema`, and names its
    // system schemas in upper case under a binary collation, so the schema is
    // lower-cased before it is compared.
    const bool is_mysql = config.type == "mysql" || config.type == "tidb";
    const LiteralSyntax literal_syntax = config.type == "postgres"
        ? LiteralSyntax::PostgresEscape
        : is_mysql || config.type == "snowflake"
            ? LiteralSyntax::BackslashEscapes
            : LiteralSyntax::Standard;
    const std::string excluded_schemas =
        is_mysql ? "('information_schema', 'mysql', 'performance_schema', 'sys', "
                   "'metrics_schema')"
                 : "('pg_catalog', 'information_schema')";
    const std::string table_schema = is_mysql ? "LOWER(t.table_schema)" : "t.table_schema";
    const std::string column_schema = is_mysql ? "LOWER(c.table_schema)" : "c.table_schema";
    const auto alias = [is_mysql](const char* column) {
        return is_mysql ? std::string(column) + " AS " + (std::string(column).substr(2))
                        : std::string(column);
    };
    CatalogStatements statements;
    statements.tables =
        "SELECT " + alias("t.table_schema") + ", " + alias("t.table_name") + ", " +
        alias("t.table_type") +
        " FROM information_schema.tables t"
        " WHERE " + table_schema + " NOT IN " + excluded_schemas;
    if (!request.schema.empty()) {
        statements.tables +=
            " AND t.table_schema = " + sql_literal(request.schema, literal_syntax);
    }
    if (!request.match.empty()) {
        statements.tables += " AND LOWER(t.table_name) LIKE " +
                             sql_literal("%" + lowered(request.match) + "%", literal_syntax);
    }
    statements.tables += " ORDER BY t.table_schema, t.table_name";

    statements.columns =
        "SELECT " + alias("c.table_schema") + ", " + alias("c.table_name") + ", " +
        alias("c.column_name") + ", " + alias("c.data_type") + "," + " " +
        alias("c.is_nullable") + ", " + alias("c.ordinal_position") +
        " FROM information_schema.columns c"
        " WHERE " + column_schema + " NOT IN " + excluded_schemas;
    if (!request.schema.empty()) {
        statements.columns +=
            " AND c.table_schema = " + sql_literal(request.schema, literal_syntax);
    }
    statements.columns += " ORDER BY c.table_schema, c.table_name, c.ordinal_position";
    return statements;
}

// lake's SQLite service: one database, schema `main`, catalog in sqlite_master.
// Internal `sqlite_%` tables are left out. The column read joins the
// `pragma_table_info` table-valued function (a function call, not a PRAGMA
// statement, so the service connector's guard admits it).
CatalogStatements sqlite_statements(const SourceCatalogRequest& request) {
    const std::string user_objects =
        "m.type IN ('table', 'view') AND m.name NOT LIKE 'sqlite\\_%' ESCAPE '\\'";
    const std::string schema_filter =
        request.schema.empty() ? std::string{}
                               : " AND 'main' = " + sql_literal(request.schema);
    CatalogStatements statements;
    statements.tables =
        "SELECT 'main' AS table_schema, m.name AS table_name,"
        " CASE m.type WHEN 'view' THEN 'VIEW' ELSE 'BASE TABLE' END AS table_type"
        " FROM sqlite_master AS m WHERE " + user_objects + schema_filter;
    if (!request.match.empty()) {
        statements.tables += " AND LOWER(m.name) LIKE " +
                             sql_literal("%" + lowered(request.match) + "%");
    }
    statements.tables += " ORDER BY m.name";
    statements.columns =
        "SELECT 'main' AS table_schema, m.name AS table_name, p.name AS column_name,"
        " p.type AS data_type,"
        " CASE WHEN p.\"notnull\" = 1 THEN 'NO' ELSE 'YES' END AS is_nullable,"
        " p.cid + 1 AS ordinal_position"
        " FROM sqlite_master AS m JOIN pragma_table_info(m.name) AS p"
        " WHERE " + user_objects + schema_filter +
        " ORDER BY m.name, p.cid";
    return statements;
}

// lake's Polars service: the datasets it holds, from its own listing. A
// dataset is queried by its bare name, so it has no schema.
SourceCatalog polars_catalog(const ConnectorConfig& config,
                             const SourceCatalogRequest& request,
                             const HttpTransport& transport) {
    SourceCatalog catalog;
    const HttpTransport& exchange = transport ? transport : default_http_transport();
    if (!exchange) {
        catalog.error =
            "no outbound HTTP transport is installed in this build — the Polars "
            "service cannot be listed";
        return catalog;
    }
    const ConnectorResult listed = lake_sql::list_datasets(config, exchange);
    if (!listed.ok()) {
        catalog.error = listed.error;
        return catalog;
    }
    const std::string needle = lowered(request.match);
    for (const nlohmann::json& dataset : listed.data.rows) {
        if (!request.schema.empty()) break;  // no dataset has a schema
        const std::string name = text(dataset, "table_name");
        if (!needle.empty() && lowered(name).find(needle) == std::string::npos) {
            continue;
        }
        if (catalog.tables.size() >= request.max_tables) {
            catalog.truncated = true;
            break;
        }
        nlohmann::json entry = {{"schema", ""}, {"table", name}, {"kind", "DATASET"}};
        if (request.include_columns) {
            entry["columns"] = nlohmann::json::array();
            for (const nlohmann::json& column : dataset.at("columns")) {
                entry["columns"].push_back({{"column", text(column, "name")},
                                            {"type", text(column, "type")},
                                            {"nullable", true}});
            }
        }
        catalog.tables.push_back(std::move(entry));
    }
    return catalog;
}

}  // namespace

std::vector<std::string> source_catalog_statements(const ConnectorConfig& config,
                                                   const SourceCatalogRequest& request) {
    if (config.type == "lake_polars") return {"GET /v1/datasets"};
    const CatalogStatements statements = config.type == "lake_sqlite"
                                             ? sqlite_statements(request)
                                             : information_schema_statements(config, request);
    std::vector<std::string> out{statements.tables};
    if (request.include_columns) out.push_back(statements.columns);
    return out;
}

SourceCatalog list_source_catalog(const ConnectorConfig& config,
                                  const SourceCatalogRequest& request,
                                  const HttpTransport& transport) {
    if (config.type == "lake_polars") return polars_catalog(config, request, transport);

    const CatalogStatements statements = config.type == "lake_sqlite"
                                             ? sqlite_statements(request)
                                             : information_schema_statements(config, request);

    SourceCatalog catalog;
    const ConnectorResult listed = run_connector_query(
        config, statements.tables, request.max_tables + 1, transport);
    if (!listed.ok()) {
        catalog.error = listed.error;
        return catalog;
    }

    catalog.truncated = listed.data.rows.size() > request.max_tables;
    for (const nlohmann::json& row : listed.data.rows) {
        if (catalog.tables.size() >= request.max_tables) break;
        catalog.tables.push_back({{"schema", text(row, "table_schema")},
                                  {"table", text(row, "table_name")},
                                  {"kind", text(row, "table_type")}});
    }

    if (request.include_columns && !catalog.tables.empty()) {
        // One catalog query for every column of every table listed, rather than
        // one round trip per table — a schema with 200 tables would otherwise be
        // 200 connections.
        const ConnectorResult described = run_connector_query(
            config, statements.columns, /*max_rows=*/100000, transport);
        if (described.ok()) {
            for (const nlohmann::json& row : described.data.rows) {
                const std::string schema_name = text(row, "table_schema");
                const std::string table_name = text(row, "table_name");
                for (nlohmann::json& entry : catalog.tables) {
                    if (entry["schema"] != schema_name || entry["table"] != table_name) {
                        continue;
                    }
                    entry["columns"].push_back({{"column", text(row, "column_name")},
                                                {"type", text(row, "data_type")},
                                                {"nullable", nullable(row)}});
                    break;
                }
            }
        }
        // A source that refuses the column catalog still returns its table list;
        // the tables are the answer to "what is in here", columns are detail.
    }
    return catalog;
}

}  // namespace zetta::connect
