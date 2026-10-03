#include "zetta/connect/connector.hpp"

#include "snowflake_protocol.hpp"
#include "zetta/connect/postgres_runner.hpp"
#include "zetta/connect/distributed_fetch.hpp"

#include <algorithm>
#include <cctype>
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
// the read-only guard sees the first real keyword.
std::string_view skip_leading_noise(std::string_view sql) {
    for (;;) {
        while (!sql.empty() &&
               std::isspace(static_cast<unsigned char>(sql.front()))) {
            sql.remove_prefix(1);
        }
        if (sql.starts_with("--")) {
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
bool has_trailing_statement(std::string_view sql) {
    const std::size_t semicolon = sql.find(';');
    if (semicolon == std::string_view::npos) return false;
    std::string_view rest = sql.substr(semicolon + 1);
    rest = skip_leading_noise(rest);
    return !rest.empty();
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
    return value;
}

std::optional<ConnectorConfig> ConnectorConfig::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    static const std::vector<std::string> known = {
        "type", "account", "token", "auth", "database", "schema", "warehouse",
        "role", "host", "port", "user", "sslmode"};
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
          std::pair{"sslmode", &config.sslmode}}) {
        const std::optional<std::string> field = string_at(key);
        if (!field) return std::nullopt;
        *target = *field;
    }

    // The secret (Snowflake bearer / Postgres password). Optional for Postgres
    // trust/peer auth; required for Snowflake.
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
    }

    return config;
}

// --- The registry + the guard -------------------------------------------------

const std::vector<std::string>& connector_types() {
    static const std::vector<std::string> types = {"snowflake", "postgres"};
    return types;
}

const nlohmann::json& connector_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSON({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/connector_config.schema.json",
  "title": "ConnectorConfig",
  "description": "Where and how to reach an external source for a connector query. The secret (Snowflake bearer / Postgres password) rides in `token` and is never serialized back out. Per-type required fields are enforced in from_json (Snowflake: account+token; Postgres: host+user+database).",
  "type": "object",
  "required": ["type"],
  "additionalProperties": false,
  "properties": {
    "type": {"type": "string", "enum": ["snowflake", "postgres"], "description": "Which connector implementation to use."},
    "account": {"type": "string", "description": "Snowflake account identifier (the host prefix, e.g. ab12345.us-east-1)."},
    "token": {"type": "string", "description": "The secret: a Snowflake programmatic access / OAuth token, or a Postgres password. Never serialized back out."},
    "auth": {"type": "string", "enum": ["pat", "oauth"], "default": "pat", "description": "Snowflake: how to present the bearer token."},
    "database": {"type": "string", "description": "Session database (both connectors)."},
    "schema": {"type": "string", "description": "Session schema (both connectors)."},
    "warehouse": {"type": "string", "description": "Snowflake compute warehouse (required for token auth)."},
    "role": {"type": "string", "description": "Optional session role."},
    "host": {"type": "string", "description": "Postgres host, e.g. 127.0.0.1."},
    "port": {"type": "string", "description": "Postgres port (default 5432)."},
    "user": {"type": "string", "description": "Postgres role/user."},
    "sslmode": {"type": "string", "description": "Postgres sslmode (disable / require / verify-full / …)."}
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

    const std::string rejection = read_only_query_rejection(sql);
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
    if (config.type == "snowflake") {
        return run_snowflake_query(config, sql, max_rows, exchange,
                                   async_transport);
    }
    result.error = "unknown connector type '" + config.type + "'";
    return result;
}

}  // namespace zetta::connect
