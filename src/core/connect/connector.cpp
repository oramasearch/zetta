#include "zetta/connect/connector.hpp"

#include "snowflake_protocol.hpp"
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

    if (!async_transport) {
        // Degrade gracefully: with only a synchronous transport (an injected
        // fake, or a build with no async client) walk the partitions in order.
        // Correct, just not concurrent — the fan-out is an optimization, never
        // a correctness requirement.
        for (const std::size_t partition : rest) {
            if (result.data.rows.size() >= max_rows) break;
            const HttpResponse page = transport(
                snowflake::partition_request(config, submitted.plan.handle,
                                             static_cast<long long>(partition)));
            if (!page.error.empty()) {
                result.error = "snowflake: partition " +
                               std::to_string(partition) + ": " + page.error;
                return result;
            }
            if (!page.ok()) {
                result.error = snowflake::status_error(page);
                return result;
            }
            const auto page_body =
                nlohmann::json::parse(page.body, nullptr, /*allow_exceptions=*/false);
            if (!page_body.is_object()) {
                result.error = "snowflake: partition " +
                               std::to_string(partition) +
                               " body is not a JSON object";
                return result;
            }
            snowflake::append_data(page_body, submitted.plan.columns,
                                   submitted.plan.column_types, max_rows,
                                   result.data.rows);
        }
        return result;
    }

    PartitionFetchResult fetched = fetch_result_partitions(
        config, submitted.plan, rest, max_rows - result.data.rows.size(),
        kDefaultPartitionsInFlight, async_transport);
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
    return value;
}

nlohmann::json ConnectorConfig::to_storage_json() const {
    // The ONE serialization that carries the credential — see the header. It is
    // written only into the replicated connection store, whose whole purpose is
    // to put the token on every node that has to present it.
    nlohmann::json value = to_json();
    value["token"] = token;
    return value;
}

std::optional<ConnectorConfig> ConnectorConfig::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    static const std::vector<std::string> known = {
        "type", "account", "token", "auth", "database", "schema", "warehouse", "role"};
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

    const std::optional<std::string> account = string_at("account");
    if (!account || account->empty()) return std::nullopt;
    config.account = *account;

    const std::optional<std::string> token = string_at("token");
    if (!token || token->empty()) return std::nullopt;
    config.token = *token;

    const auto auth_field = value.find("auth");
    if (auth_field != value.end()) {
        if (!auth_field->is_string()) return std::nullopt;
        const std::string auth = auth_field->get<std::string>();
        if (auth != "pat" && auth != "oauth") return std::nullopt;
        config.auth = auth;
    }

    for (const auto& [key, target] :
         {std::pair{"database", &config.database}, std::pair{"schema", &config.schema},
          std::pair{"warehouse", &config.warehouse}, std::pair{"role", &config.role}}) {
        const std::optional<std::string> field = string_at(key);
        if (!field) return std::nullopt;
        *target = *field;
    }

    return config;
}

// --- The registry + the guard -------------------------------------------------

const std::vector<std::string>& connector_types() {
    static const std::vector<std::string> types = {"snowflake"};
    return types;
}

const nlohmann::json& connector_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSON({
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/connector_config.schema.json",
  "title": "ConnectorConfig",
  "description": "Where and how to reach an external source for a connector query. The token is a bearer credential and is never serialized back out.",
  "type": "object",
  "required": ["type", "account", "token"],
  "additionalProperties": false,
  "properties": {
    "type": {"type": "string", "enum": ["snowflake"], "description": "Which connector implementation to use."},
    "account": {"type": "string", "minLength": 1, "description": "Source account identifier (for Snowflake, the host prefix, e.g. ab12345.us-east-1)."},
    "token": {"type": "string", "minLength": 1, "description": "Bearer credential: a Snowflake programmatic access token, or an OAuth token."},
    "auth": {"type": "string", "enum": ["pat", "oauth"], "default": "pat", "description": "How to present the bearer token."},
    "database": {"type": "string", "description": "Session database."},
    "schema": {"type": "string", "description": "Session schema."},
    "warehouse": {"type": "string", "description": "Compute warehouse (required for token auth on Snowflake)."},
    "role": {"type": "string", "description": "Optional session role."}
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
