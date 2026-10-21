#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/connect/connector.hpp"
#include "zetta/connect/http_transport.hpp"

// **Internal** — the wire details of lake's SQL-over-HTTP services
// ([#1014](https://github.com/oramasearch/zetta/issues/1014)): `lake-sqlite`,
// `lake-duckdb` and `lake-polars`, the small web services behind lake's
// `sqlite`, `duckdb` and `polars` service kinds. All three answer
// `POST /v1/query {"sql"}` with `{"columns", "rows", "truncated"}` behind a
// bearer token, so one connector serves the three connector types
// `lake_sqlite`, `lake_duckdb` and `lake_polars`. Not a public header; nothing
// outside `src/core/connect/` includes it.
//
// The HTTP exchange is the injected `HttpTransport`, so everything here runs
// against a fake in a unit test. See docs/data/connectors/lake-sql.md.
namespace zetta::connect::lake_sql {

// The service sends at most this many rows per answer and marks a longer
// result `"truncated": true` (each service's docs/api.md).
inline constexpr std::size_t kServiceRowCap = 10000;

// Whether `type` is one of the three lake service connector types.
bool is_lake_sql_type(std::string_view type);

// The engine a type names, as a person reads it: "SQLite", "DuckDB", "Polars".
std::string engine_name(std::string_view type);

// A parsed base URL. `secure` is false only for an explicit `http://`.
struct Endpoint {
    bool secure = true;
    std::string host;  // without brackets for IPv6
    std::string port;  // empty = the scheme's default
    std::string path;  // the base path without a trailing '/', may be empty

    // `https://host[:port][/path]`, with IPv6 hosts bracketed.
    std::string base_url() const;
};

// Parse a configured endpoint. No scheme means `https://`; only `http` and
// `https` are accepted; user info, a query or a fragment is refused (a
// credential belongs in `token`, never in a URL that could be logged). Returns
// std::nullopt with `why` set when it does not parse.
std::optional<Endpoint> parse_endpoint(std::string_view endpoint, std::string* why);

// The extra read-only rule for these services, applied after the shared
// SELECT/WITH guard. The services run what they are sent (lake-sqlite runs
// every `;`-separated statement, and SQLite and DuckDB accept a WITH that feeds
// an INSERT, UPDATE or DELETE), so any word that starts a write or changes the
// session is refused wherever it appears outside a string literal, a quoted
// identifier or a comment (write_words.hpp). Returns a reason, or empty.
std::string query_rejection(const std::string& sql);

// Coerce one answered cell to what a dataset stores: a scalar passes through,
// `{"$base64": "…"}` (a BLOB) becomes its base64 text, and any other object or
// array (a DuckDB LIST / STRUCT / MAP / INTERVAL) becomes its JSON text.
nlohmann::json cell(const nlohmann::json& raw);

// The `POST <endpoint>/v1/query` request. The bearer is named secret.
HttpRequest query_request(const ConnectorConfig& config, const Endpoint& endpoint,
                          const std::string& sql);

// The `GET <endpoint>/v1/datasets` request (lake-polars only).
HttpRequest datasets_request(const ConnectorConfig& config, const Endpoint& endpoint);

// Read one `/v1/query` answer into rows, capped at `max_rows`. A truncated
// answer that holds fewer rows than `max_rows` is an error rather than a short
// dataset.
ConnectorResult read_query_response(const ConnectorConfig& config,
                                    const Endpoint& endpoint,
                                    const HttpResponse& response,
                                    std::size_t max_rows);

// Run one query through `transport` (already resolved, never empty).
ConnectorResult run_query(const ConnectorConfig& config, const std::string& sql,
                          std::size_t max_rows, const HttpTransport& transport);

// lake-polars has no SQL catalog (its sandbox admits only queries), so its
// tables come from `GET /v1/datasets`. One row per dataset:
// `{"table_name", "rows", "columns": [{"name", "type"}]}`.
ConnectorResult list_datasets(const ConnectorConfig& config,
                              const HttpTransport& transport);

}  // namespace zetta::connect::lake_sql
