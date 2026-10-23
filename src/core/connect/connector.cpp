#include "zetta/connect/connector.hpp"

#include "lake_sql_protocol.hpp"
#include "snowflake_protocol.hpp"
#include "zetta/connect/mysql_client.hpp"
#include "zetta/connect/postgres_runner.hpp"
#include "zetta/connect/distributed_fetch.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <string_view>
#include <thread>
#include <utility>

namespace zetta::connect {
namespace {

std::string to_lower(std::string text) {
    for (char& character : text) {
        character =
            static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

// Strip leading whitespace and SQL comments (`--` to end of line, `/* … */`) so
// the read-only guard sees the first real keyword. `hash_comments` also strips
// `#` to end of line — MySQL's third comment form.
std::string_view skip_leading_noise(std::string_view sql, bool hash_comments = false) {
    for (;;) {
        while (!sql.empty() &&
               std::isspace(static_cast<unsigned char>(sql.front()))) {
            sql.remove_prefix(1);
        }
        if (sql.starts_with("--") || (hash_comments && sql.starts_with("#"))) {
            const std::size_t line_end = sql.find('\n');
            if (line_end == std::string_view::npos) return {};
            sql.remove_prefix(line_end + 1);
            continue;
        }
        if (sql.starts_with("/*")) {
            const std::size_t close = sql.find("*/");
            if (close == std::string_view::npos) return {};
            sql.remove_prefix(close + 2);
            continue;
        }
        return sql;
    }
}

// A trailing `;` is fine; a `;` with anything after it is a second statement.
bool has_trailing_statement(std::string_view sql, bool hash_comments = false) {
    const std::size_t semicolon = sql.find(';');
    if (semicolon == std::string_view::npos) return false;
    std::string_view rest = sql.substr(semicolon + 1);
    rest = skip_leading_noise(rest, hash_comments);
    return !rest.empty();
}

// Does `sql` start with `keyword` as a whole word (case-insensitive)?
bool starts_with_keyword(std::string_view sql, std::string_view keyword) {
    if (sql.size() < keyword.size()) return false;
    for (std::size_t index = 0; index < keyword.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(sql[index])) != keyword[index]) {
            return false;
        }
    }
    return sql.size() == keyword.size() ||
           std::isspace(static_cast<unsigned char>(sql[keyword.size()])) ||
           sql[keyword.size()] == '(' || sql[keyword.size()] == '`';
}

// Does the word `word` (lowercase) appear anywhere in `sql` as a whole
// identifier-like token? Deliberately blunt — it does not skip string literals,
// so a literal containing the word is refused too. A false refusal costs a
// rephrased query; a missed `INTO OUTFILE` writes a file on the server.
bool contains_word(std::string_view sql, std::string_view word) {
    const std::string lowered = to_lower(std::string(sql));
    const auto is_word_char = [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_' ||
               character == '$';
    };
    for (std::size_t found = lowered.find(word); found != std::string::npos;
         found = lowered.find(word, found + 1)) {
        const bool starts = found == 0 || !is_word_char(lowered[found - 1]);
        const std::size_t end = found + word.size();
        const bool ends = end == lowered.size() || !is_word_char(lowered[end]);
        if (starts && ends) return true;
    }
    return false;
}

// The MySQL guard (#895). The driver enforces read-only too (a READ ONLY
// session and transaction, no multi-statements, no LOCAL INFILE — see
// mysql_client.hpp); this refuses at the boundary what can be recognised
// before a connection is opened.
std::string mysql_query_rejection(const std::string& sql) {
    // `/*! … */` is not a comment to MySQL: the server executes its contents.
    // The shared guard would skip it as leading noise and see the SELECT after
    // it, so refuse it outright, wherever it appears.
    if (sql.find("/*!") != std::string::npos) {
        return "MySQL executable comments (/*! ... */) may not run through a "
               "connector";
    }
    const std::string_view trimmed = skip_leading_noise(sql, /*hash_comments=*/true);
    if (trimmed.empty()) return "the query is empty";
    if (!starts_with_keyword(trimmed, "select") && !starts_with_keyword(trimmed, "with") &&
        !starts_with_keyword(trimmed, "show")) {
        return "only a read-only SELECT, WITH or SHOW statement may run through "
               "the MySQL connector";
    }
    if (has_trailing_statement(trimmed, /*hash_comments=*/true)) {
        return "only a single statement may run through a connector";
    }
    // SELECT … INTO OUTFILE / DUMPFILE writes a file on the database server —
    // a read-only transaction does not stop it.
    if (contains_word(trimmed, "outfile") || contains_word(trimmed, "dumpfile")) {
        return "SELECT ... INTO OUTFILE / DUMPFILE writes a file on the server "
               "and may not run through a connector";
    }
    return {};
}

// --- Snowflake (SQL API v2 over HTTPS) ---------------------------------------
//
// Submitting + framing lives in snowflake_protocol.hpp (shared with the tiled
// concurrent fetch in distributed_fetch.cpp); what remains here is the
// single-node orchestration: submit once, then pull the result partitions.

ConnectorResult run_snowflake_query(const ConnectorConfig& config,
                                    const std::string& sql, std::size_t max_rows,
                                    const HttpTransport& transport,
                                    const AsyncHttpTransport& async_transport) {
    ConnectorResult result;

    StatementSubmission submitted =
        submit_connector_statement(config, sql, max_rows, transport);
    if (!submitted.ok()) {
        result.error = submitted.error;
        return result;
    }
    result.data.columns = std::move(submitted.plan.columns);
    submitted.plan.columns = result.data.columns;  // the plan still needs them
    result.data.rows = std::move(submitted.inline_rows.rows);

    // A single-partition (or partition-less) result arrived wholly inline —
    // there is nothing to fan out, so degrade to what we already have. This is
    // the small-result path and it never opens a second socket.
    if (submitted.plan.fetchable_partitions() == 0) return result;
    if (result.data.rows.size() >= max_rows) return result;

    // Everything past partition 0 is an independently-fetchable slice. On one
    // node that means CONCURRENT reads rather than a serial walk; across a
    // cluster the same indices are tiled by plan_partition_assignments so each
    // node pulls a disjoint slice with its own link.
    std::vector<std::size_t> rest;
    rest.reserve(submitted.plan.fetchable_partitions());
    for (std::size_t index = 1; index < submitted.plan.partition_rows.size();
         ++index) {
        rest.push_back(index);
    }

    // Degrade gracefully: with only a synchronous transport (an injected fake,
    // or a build with no async client) walk the partitions in order. Correct,
    // just not concurrent — the fan-out is an optimization, never a correctness
    // requirement.
    PartitionFetchResult fetched =
        async_transport
            ? fetch_result_partitions(config, submitted.plan, rest,
                                      max_rows - result.data.rows.size(),
                                      kDefaultPartitionsInFlight, async_transport)
            : fetch_result_partitions_serially(config, submitted.plan, rest,
                                               max_rows - result.data.rows.size(),
                                               transport);
    if (!fetched.ok()) {
        result.error = fetched.error;
        return result;
    }
    for (nlohmann::json& row : fetched.rows) {
        if (result.data.rows.size() >= max_rows) break;
        result.data.rows.push_back(std::move(row));
    }
    return result;
}

}  // namespace

// --- ConnectorConfig ----------------------------------------------------------

nlohmann::json ConnectorConfig::to_json() const {
    // `token` is deliberately absent — a config must be safe to echo or log.
    nlohmann::json value{{"type", type}, {"account", account}, {"auth", auth}};
    if (!database.empty()) value["database"] = database;
    if (!schema.empty()) value["schema"] = schema;
    if (!warehouse.empty()) value["warehouse"] = warehouse;
    if (!role.empty()) value["role"] = role;
    if (!host.empty()) value["host"] = host;
    if (!port.empty()) value["port"] = port;
    if (!user.empty()) value["user"] = user;
    if (!sslmode.empty()) value["sslmode"] = sslmode;
    if (!endpoint.empty()) value["endpoint"] = endpoint;
    return value;
}

std::optional<ConnectorConfig> ConnectorConfig::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    static const std::vector<std::string> known = {
        "type", "account", "token", "auth", "database", "schema", "warehouse",
        "role", "host", "port", "user", "sslmode", "endpoint"};
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (std::find(known.begin(), known.end(), field.key()) == known.end()) {
            return std::nullopt;  // unknown key
        }
    }

    const auto string_at = [&value](const char* key) -> std::optional<std::string> {
        const auto found = value.find(key);
        if (found == value.end()) return std::string();
        if (!found->is_string()) return std::nullopt;
        return found->get<std::string>();
    };

    ConnectorConfig config;
    const std::optional<std::string> type = string_at("type");
    if (!type || type->empty()) return std::nullopt;
    const std::vector<std::string>& types = connector_types();
    if (std::find(types.begin(), types.end(), *type) == types.end()) {
        return std::nullopt;
    }
    config.type = *type;

    // Read every optional string field first; then enforce the per-type required
    // set below (a non-throwing, type-aware boundary — the JSON schema stays
    // permissive so both connector shapes validate against it).
    for (const auto& [key, target] :
         {std::pair{"account", &config.account}, std::pair{"database", &config.database},
          std::pair{"schema", &config.schema}, std::pair{"warehouse", &config.warehouse},
          std::pair{"role", &config.role}, std::pair{"host", &config.host},
          std::pair{"port", &config.port}, std::pair{"user", &config.user},
          std::pair{"sslmode", &config.sslmode},
          std::pair{"endpoint", &config.endpoint}}) {
        const std::optional<std::string> field = string_at(key);
        if (!field) return std::nullopt;
        *target = *field;
    }

    // The secret (Snowflake bearer / Postgres or MySQL password). Optional for
    // Postgres trust/peer auth and a passwordless MySQL account; required for
    // Snowflake.
    const std::optional<std::string> token = string_at("token");
    if (!token) return std::nullopt;
    config.token = *token;

    const auto auth_field = value.find("auth");
    if (auth_field != value.end()) {
        if (!auth_field->is_string()) return std::nullopt;
        const std::string auth = auth_field->get<std::string>();
        if (auth != "pat" && auth != "oauth") return std::nullopt;
        config.auth = auth;
    }

    // Per-type required fields.
    if (config.type == "snowflake") {
        if (config.account.empty() || config.token.empty()) return std::nullopt;
    } else if (config.type == "postgres") {
        if (config.host.empty() || config.user.empty() || config.database.empty()) {
            return std::nullopt;
        }
    } else if (lake_sql::is_lake_sql_type(config.type)) {
        // A lake service: its base URL and its bearer token, nothing else.
        if (config.endpoint.empty() || config.token.empty()) return std::nullopt;
        if (!lake_sql::parse_endpoint(config.endpoint, nullptr)) return std::nullopt;
    } else if (config.type == "mysql") {
        // database is optional: a query may name `schema.table` itself.
        if (config.host.empty() || config.user.empty()) return std::nullopt;
        // An unknown sslmode is refused rather than read as "no TLS" — and
        // `prefer`, which silently falls back to plaintext, is not a mode here.
        if (!parse_mysql_tls_mode(config.sslmode)) return std::nullopt;
        if (!config.port.empty()) {
            unsigned port = 0;
            const char* const first = config.port.data();
            const char* const last = first + config.port.size();
            const auto [end, error] = std::from_chars(first, last, port);
            if (error != std::errc() || end != last || port == 0 || port > 65535) {
                return std::nullopt;
            }
        }
    }

    return config;
}

std::string endpoint_host(const std::string& endpoint) {
    const std::optional<lake_sql::Endpoint> parsed =
        lake_sql::parse_endpoint(endpoint, nullptr);
    return parsed ? parsed->host : std::string{};
}

// --- The registry + the guard -------------------------------------------------

const std::vector<std::string>& connector_types() {
    static const std::vector<std::string> types = {
        "snowflake", "postgres", "mysql", "lake_sqlite", "lake_duckdb", "lake_polars"};
    return types;
}

const nlohmann::json& connector_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSON({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/connector_config.schema.json",
  "title": "ConnectorConfig",
  "description": "Where and how to reach an external source for a connector query. The secret (Snowflake bearer / Postgres or MySQL password / lake service bearer) rides in `token` and is never serialized back out. Per-type required fields are enforced in from_json (Snowflake: account+token; Postgres: host+user+database; MySQL: host+user; lake_sqlite / lake_duckdb / lake_polars: endpoint+token).",
  "type": "object",
  "required": ["type"],
  "additionalProperties": false,
  "properties": {
    "type": {"type": "string", "enum": ["snowflake", "postgres", "mysql", "lake_sqlite", "lake_duckdb", "lake_polars"], "description": "Which connector implementation to use. lake_sqlite, lake_duckdb and lake_polars read lake's SQLite, DuckDB and Polars services over their HTTP API."},
    "account": {"type": "string", "description": "Snowflake account identifier (the host prefix, e.g. ab12345.us-east-1)."},
    "token": {"type": "string", "description": "The secret: a Snowflake programmatic access / OAuth token, a Postgres or MySQL password, or a lake service's bearer token. Never serialized back out."},
    "auth": {"type": "string", "enum": ["pat", "oauth"], "default": "pat", "description": "Snowflake: how to present the bearer token."},
    "database": {"type": "string", "description": "Session database (Snowflake, Postgres; MySQL's optional default schema)."},
    "schema": {"type": "string", "description": "Session schema (Snowflake, Postgres)."},
    "warehouse": {"type": "string", "description": "Snowflake compute warehouse (required for token auth)."},
    "role": {"type": "string", "description": "Optional session role."},
    "host": {"type": "string", "description": "Postgres or MySQL host, e.g. 127.0.0.1."},
    "port": {"type": "string", "description": "Postgres or MySQL port (default 5432 / 3306)."},
    "user": {"type": "string", "description": "Postgres role / MySQL user."},
    "sslmode": {"type": "string", "description": "Postgres sslmode (disable / require / verify-full / …). MySQL: verify-full (the default — TLS, certificate verified against the system trust store and matched to the host), verify-ca, require (TLS without verification), or disable (plaintext, only when named)."},
    "endpoint": {"type": "string", "description": "lake_sqlite / lake_duckdb / lake_polars: the service's base URL, e.g. http://orders.ws-1.svc.cluster.local:8080. Without a scheme it is https://; plaintext http:// must be written out. No credentials, query or fragment in it: the bearer token goes in `token`."}
  }
})JSON",
                                                              nullptr,
                                                              /*allow_exceptions=*/false);
    return schema;
}

std::string read_only_query_rejection(const std::string& sql) {
    const std::string_view trimmed = skip_leading_noise(sql);
    if (trimmed.empty()) return "the query is empty";

    // Only a leading SELECT / WITH may run: a connector reads, never writes.
    const std::string head = to_lower(std::string(trimmed.substr(0, 7)));
    const bool is_select = head.starts_with("select") &&
                           (trimmed.size() == 6 ||
                            std::isspace(static_cast<unsigned char>(trimmed[6])) ||
                            trimmed[6] == '(');
    const bool is_with = head.starts_with("with") &&
                         (trimmed.size() == 4 ||
                          std::isspace(static_cast<unsigned char>(trimmed[4])) ||
                          trimmed[4] == '(');
    if (!is_select && !is_with) {
        return "only a read-only SELECT (or WITH) query may run through a "
               "connector";
    }
    if (has_trailing_statement(trimmed)) {
        return "only a single statement may run through a connector";
    }
    return {};
}

std::string connector_query_rejection(const ConnectorConfig& config,
                                      const std::string& sql) {
    if (config.type == "mysql") return mysql_query_rejection(sql);
    if (lake_sql::is_lake_sql_type(config.type)) {
        if (std::string rejection = read_only_query_rejection(sql); !rejection.empty()) {
            return rejection;
        }
        return lake_sql::query_rejection(sql);
    }
    return read_only_query_rejection(sql);
}

ConnectorResult run_connector_query(const ConnectorConfig& config,
                                    const std::string& sql, std::size_t max_rows,
                                    const HttpTransport& transport) {
    // An INJECTED transport (a test fake) must be the only thing that speaks:
    // pairing it with the installed async client would let a unit test reach
    // the network. So the concurrent path is used only when the caller took the
    // installed defaults; an injected transport degrades to the serial walk.
    return run_connector_query(config, sql, max_rows, transport,
                               transport ? AsyncHttpTransport{}
                                         : default_async_http_transport());
}

ConnectorResult run_connector_query(const ConnectorConfig& config,
                                    const std::string& sql, std::size_t max_rows,
                                    const HttpTransport& transport,
                                    const AsyncHttpTransport& async_transport) {
    ConnectorResult result;

    const std::string rejection = connector_query_rejection(config, sql);
    if (!rejection.empty()) {
        result.error = rejection;
        return result;
    }
    if (max_rows == 0) {
        result.error = "max_rows must be at least 1";
        return result;
    }

    // Postgres speaks its own wire protocol (libpqxx), not HTTP — dispatch it
    // before the HTTP-transport check. The libpqxx-linking runner is injected by
    // zetta-api (postgres_runner.hpp), so zetta-core links no libpq.
    if (config.type == "postgres") {
        if (!has_postgres_query_runner()) {
            result.error =
                "the Postgres connector is not built into this binary "
                "(ZETTA_WITH_POSTGRES off, or no zetta-api layer installed it)";
            return result;
        }
        return postgres_query_runner()(config, sql, max_rows);
    }
    // MySQL speaks its own wire protocol too, implemented natively
    // (mysql_client.hpp); the TCP/TLS driver is installed by zetta-api.
    if (config.type == "mysql") {
        if (!has_mysql_query_runner()) {
            result.error =
                "the MySQL connector is not installed in this binary (no zetta-api "
                "layer called install_mysql_protocol)";
            return result;
        }
        return mysql_query_runner()(config, sql, max_rows);
    }

    const HttpTransport& exchange = transport ? transport : default_http_transport();
    if (!exchange) {
        // Nothing installed the native client (api::install_native_http_transport)
        // — a unit test, or a tool built without the API target. Fail cleanly
        // rather than pretending the source was unreachable.
        result.error =
            "no outbound HTTP transport is installed in this build — a connector "
            "query cannot reach an external source";
        return result;
    }
    if (lake_sql::is_lake_sql_type(config.type)) {
        return lake_sql::run_query(config, sql, max_rows, exchange);
    }
    if (config.type == "snowflake") {
        return run_snowflake_query(config, sql, max_rows, exchange,
                                   async_transport);
    }
    result.error = "unknown connector type '" + config.type + "'";
    return result;
}

}  // namespace zetta::connect
