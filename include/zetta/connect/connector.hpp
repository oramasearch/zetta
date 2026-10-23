#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/connect/http_transport.hpp"

// **Source connectors** — run a read-only query against an external system and
// return its rows ([#827](https://github.com/oramasearch/zetta/issues/827)).
//
// This is the missing half of *"Connect the sources"*: until now zetta could only
// ingest a **local file** (`data::add_dataset_file`), so data living in a
// warehouse had to be exported by hand first. A connector closes that gap — it
// yields rows, and [`data::add_dataset_rows`](../data/add_dataset.hpp) shards and
// places them across the cluster. **The connector's only job is to produce rows;
// Zetta owns the distribution.**
//
// **No foreign runtime (ADR-17).** There is no warehouse driver or SDK in the
// engine. A connector builds plain HTTP requests against the source's public REST
// API and parses JSON — and even the HTTP exchange is injected through the
// [`HttpTransport`](http_transport.hpp) seam, so connectors are unit-testable
// against a fake with no network and no credentials.
//
// **Read-only by construction.** `run_connector_query` accepts only a single
// `SELECT` / `WITH` statement; anything that could mutate the source is rejected
// at the boundary before a request is built. Reaching an external system with
// credentials is additionally consent-gated at the capability layer.
//
// Snowflake is the first connector (its SQL API v2 over HTTPS). See
// docs/data/connector-query.md.
namespace zetta::connect {

// Where + how to reach a source. A value-type DTO with a hand-written,
// non-throwing `from_json` mirroring `connector_config_schema()`.
//
// `token` is a bearer credential (a Snowflake Programmatic Access Token or an
// OAuth token). It is **input-only** — `to_json()` never emits it, so a config
// can be echoed back to a caller or logged without leaking the secret.
struct ConnectorConfig {
    std::string type;              // "snowflake" | "postgres" | "mysql" | "tidb" | "lake_sqlite" | "lake_duckdb" | "lake_polars" (see connector_types())
    std::string account;           // Snowflake: "ab12345.us-east-1" (the host prefix)
    std::string token;             // the secret — never serialized (Snowflake bearer / Postgres or MySQL password)
    std::string auth = "pat";      // Snowflake: "pat" (default) | "oauth"
    std::string database;          // session database (both)
    std::string schema;            // session schema (both)
    std::string warehouse;         // Snowflake: compute warehouse (required for token auth)
    std::string role;              // optional session role (both)
    // Wire-protocol fields (type == "postgres", "mysql" or "tidb"). The password
    // rides in `token` (so the one redaction rule covers it). Postgres requires
    // host + user + database; MySQL and TiDB require host + user (database is
    // the optional default schema).
    std::string host;              // server host (e.g. 127.0.0.1)
    std::string port;              // server port (Postgres 5432, MySQL 3306, TiDB 4000 by default)
    std::string user;              // role/user
    std::string sslmode;           // Postgres sslmode; MySQL: verify-full (default) / verify-ca / require / disable
    // lake's SQL-over-HTTP services (type == "lake_sqlite" / "lake_duckdb" /
    // "lake_polars", #1014): the service's base URL, e.g.
    // `http://orders.ws-1.svc.cluster.local:8080`. The scheme, `http://` or
    // `https://`, must be written out (#1414). The bearer token rides in `token`.
    std::string endpoint;

    // The JSON form (token omitted — see above). This is the OUTWARD-facing
    // shape: what a capability returns, what a system view renders, what a log
    // line may carry.
    nlohmann::json to_json() const;

    // Validate authority and protocol fields for JSON and direct C++ callers (#1414).
    // Returns an empty string on success; never includes credential values.
    std::string destination_rejection() const;

    // Parse + validate at the boundary. Unknown keys/types and malformed
    // authority/protocol fields fail without throwing. Snowflake needs an
    // account + token; Postgres needs host + user + database; MySQL and TiDB
    // need host + user; lake's services need an explicit http(s) endpoint + token.
    static std::optional<ConnectorConfig> from_json(const nlohmann::json& value);
};

// The host an `endpoint` URL names (`http://127.0.0.1:8080/x` → `127.0.0.1`,
// `https://[::1]:8443` → `::1`), or empty when it does not parse. The consent
// gate uses it to tell a loopback service from a remote one.
std::string endpoint_host(const std::string& endpoint);

// The rows a query returned: the column names in result order, and one JSON
// object per row keyed by those names. Cells are coerced to real JSON types
// (numbers stay numbers, booleans booleans, NULL null) so the landed dataset is
// directly featurizable rather than a table of strings.
struct ConnectorRows {
    std::vector<std::string> columns;
    std::vector<nlohmann::json> rows;
};

// The outcome of a query: the rows (populated only when ok()) plus a
// path-qualified reason on failure. Non-throwing throughout.
struct ConnectorResult {
    ConnectorRows data;
    std::string error;

    bool ok() const { return error.empty(); }
};

// The connector types this build knows, in menu order. One entry per
// implementation `run_connector_query` dispatches on.
const std::vector<std::string>& connector_types();

// The ConnectorConfig JSON Schema (draft-07) — the code-owned source of truth.
const nlohmann::json& connector_config_schema();

// Whether `sql` is a single read-only statement (a `SELECT` or `WITH`, no
// trailing statement). The guard `run_connector_query` applies before building a
// request; exposed so a caller can validate earlier. Returns a reason when it is
// not accepted, else an empty string.
std::string read_only_query_rejection(const std::string& sql);

// The guard for a specific connector: `read_only_query_rejection`, plus the
// source's own dialect rules. For MySQL that also admits `SHOW`, treats `#` as
// a comment, and rejects executable comments (`/*! … */`, which the server runs
// as SQL) and `INTO OUTFILE` / `INTO DUMPFILE` (which write files on the
// server). `run_connector_query` and the distributed ingest apply this one.
std::string connector_query_rejection(const ConnectorConfig& config,
                                      const std::string& sql);

// Run `sql` against the source described by `config`, returning up to `max_rows`
// rows. Dispatches on `config.type`. `transport` performs the HTTP exchanges (an
// empty transport uses `default_http_transport()`).
//
// Non-throwing: an unknown type, a non-read-only statement, a transport failure,
// an error status from the source, or a malformed response all come back as a
// result whose `error` explains why.
// A large result arrives as several independently-fetchable partitions; they
// are pulled **concurrently** through the installed async transport (see
// [distributed_fetch.hpp](distributed_fetch.hpp)). Passing an explicit
// `transport` opts out of that — an injected fake is then the only thing that
// speaks, and the partitions are walked serially — so a unit test can never
// reach the network through the async default.
ConnectorResult run_connector_query(const ConnectorConfig& config,
                                    const std::string& sql, std::size_t max_rows,
                                    const HttpTransport& transport = {});

// The same, with the coroutine transport named explicitly: the submission (and
// any 202 poll) goes through `transport`, and the result partitions are fetched
// concurrently through `async_transport`. An empty `async_transport` degrades to
// the serial walk. This is the overload a test uses to exercise the concurrent
// path against a fake.
ConnectorResult run_connector_query(const ConnectorConfig& config,
                                    const std::string& sql, std::size_t max_rows,
                                    const HttpTransport& transport,
                                    const AsyncHttpTransport& async_transport);

}  // namespace zetta::connect
