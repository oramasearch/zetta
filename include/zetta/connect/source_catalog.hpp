#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/connect/connector.hpp"
#include "zetta/connect/http_transport.hpp"

// **Browse a source's catalog** — the tables (and their columns) behind a
// connector, without ingesting anything (#994). This is what the
// `list_source_tables` capability runs.
//
// Each source spells its catalog differently, so the per-type catalog reads
// live here, next to the connectors, rather than in the capability:
//
// - Postgres, Snowflake and lake's DuckDB service read the ISO
//   `information_schema.tables` / `.columns`;
// - MySQL and TiDB read the same views with every column aliased (MySQL 8
//   answers them in upper case), their own system schemas excluded (compared
//   lower-cased: TiDB names them in upper case), and backslashes doubled in
//   literals;
// - lake's SQLite service reads `sqlite_master` and `pragma_table_info`;
// - lake's Polars service has no SQL catalog (its sandbox admits only queries),
//   so its datasets come from `GET /v1/datasets`.
//
// Every read goes through `run_connector_query` (or the service's own listing),
// so the read-only guard and the injected transport apply exactly as they do to
// a dataset pull.
namespace zetta::connect {

struct SourceCatalogRequest {
    std::string schema;         // restrict to one schema; empty = every non-system schema
    std::string match;          // case-insensitive substring of the table name
    std::size_t max_tables = 200;
    bool include_columns = true;
};

// `tables` holds `{schema, table, kind[, columns: [{column, type, nullable}]]}`
// objects — the `list_source_tables` output shape. `truncated` says
// `max_tables` cut the list short.
struct SourceCatalog {
    nlohmann::json tables = nlohmann::json::array();
    bool truncated = false;
    std::string error;

    bool ok() const { return error.empty(); }
};

// Read the catalog of the source `config` describes. A source that refuses the
// column catalog still returns its tables (columns are detail). Non-throwing.
// `transport` is passed to the HTTP connectors (empty = the installed default).
SourceCatalog list_source_catalog(const ConnectorConfig& config,
                                  const SourceCatalogRequest& request,
                                  const HttpTransport& transport = {});

}  // namespace zetta::connect
