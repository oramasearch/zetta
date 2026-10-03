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
    std::string type;              // "snowflake" | "postgres" (see connector_types())
    std::string account;           // Snowflake: "ab12345.us-east-1" (the host prefix)
    std::string token;             // the secret — never serialized (Snowflake bearer / Postgres password)
    std::string auth = "pat";      // Snowflake: "pat" (default) | "oauth"
    std::string database;          // session database (both)
    std::string schema;            // session schema (both)
    std::string warehouse;         // Snowflake: compute warehouse (required for token auth)
    std::string role;              // optional session role (both)
    // Postgres fields (type == "postgres"). The password rides in `token` (so the
    // one redaction rule covers it); host + user + database are required.
    std::string host;              // Postgres host (e.g. 127.0.0.1)
    std::string port;              // Postgres port (default 5432)
    std::string user;              // Postgres role/user
    std::string sslmode;           // Postgres sslmode (disable/require/verify-full/…)

    // The JSON form (token omitted — see above). This is the OUTWARD-facing
    // shape: what a capability returns, what a system view renders, what a log
    // line may carry.
    nlohmann::json to_json() const;


    // Parse + validate at the boundary. Returns std::nullopt (never throws)
    // unless `value` is an object with a known non-empty `type`, a non-empty
    // `account`, a non-empty `token`, an `auth` of "pat"/"oauth", optional
    // string database/schema/warehouse/role, and no unknown keys.
    static std::optional<ConnectorConfig> from_json(const nlohmann::json& value);
};

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
