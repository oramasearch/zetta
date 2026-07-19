#include "zetta/connect/connector.hpp"

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

std::string snowflake_statements_url(const ConnectorConfig& config) {
    return "https://" + config.account + ".snowflakecomputing.com/api/v2/statements";
}

std::vector<std::pair<std::string, std::string>> snowflake_headers(
    const ConnectorConfig& config) {
    // PAT and OAuth are both bearer credentials; only the token-type header
    // differs (the SQL API rejects a bearer whose type it was not told).
    const std::string token_type =
        config.auth == "oauth" ? "OAUTH" : "PROGRAMMATIC_ACCESS_TOKEN";
    return {{"Authorization", "Bearer " + config.token},
            {"X-Snowflake-Authorization-Token-Type", token_type},
            {"Content-Type", "application/json"},
            {"Accept", "application/json"}};
}

// Coerce one SQL-API cell to a real JSON type using its declared column type.
// The SQL API returns every value as a JSON string (or null), so without this a
// landed dataset would be all strings and nothing downstream could featurize it.
nlohmann::json snowflake_cell(const nlohmann::json& raw, const std::string& type) {
    if (raw.is_null()) return nullptr;
    if (!raw.is_string()) return raw;  // already typed — pass through
    const std::string text = raw.get<std::string>();
    if (text.empty()) return nullptr;

    if (type == "boolean") {
        const std::string lowered = to_lower(text);
        if (lowered == "true" || lowered == "1") return true;
        if (lowered == "false" || lowered == "0") return false;
        return text;
    }
    if (type == "fixed" || type == "real" || type == "number" ||
        type == "float" || type == "double" || type == "integer") {
        // Parse through the JSON number grammar: locale-independent and
        // whole-token by construction, so "12abc" stays a string.
        const auto parsed =
            nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_number()) return parsed;
        return text;
    }
    return text;  // text / date / timestamp / variant — keep verbatim
}

// Pull `resultSetMetaData.rowType` into column names + declared types.
bool read_row_types(const nlohmann::json& body, std::vector<std::string>& columns,
                    std::vector<std::string>& types, std::string& error) {
    const auto metadata = body.find("resultSetMetaData");
    if (metadata == body.end() || !metadata->is_object()) {
        error = "snowflake: response has no resultSetMetaData";
        return false;
    }
    const auto row_type = metadata->find("rowType");
    if (row_type == metadata->end() || !row_type->is_array()) {
        error = "snowflake: response has no resultSetMetaData.rowType";
        return false;
    }
    for (const nlohmann::json& column : *row_type) {
        if (!column.is_object()) continue;
        const auto name = column.find("name");
        if (name == column.end() || !name->is_string()) {
            error = "snowflake: a rowType entry has no name";
            return false;
        }
        columns.push_back(name->get<std::string>());
        const auto type = column.find("type");
        types.push_back(type != column.end() && type->is_string()
                            ? to_lower(type->get<std::string>())
                            : std::string());
    }
    return true;
}

// Append one `data` array (an array of row arrays) onto the accumulating rows,
// stopping at `max_rows`.
void append_data(const nlohmann::json& body,
                 const std::vector<std::string>& columns,
                 const std::vector<std::string>& types, std::size_t max_rows,
                 ConnectorRows& out) {
    const auto data = body.find("data");
    if (data == body.end() || !data->is_array()) return;
    for (const nlohmann::json& row : *data) {
        if (out.rows.size() >= max_rows) return;
        if (!row.is_array()) continue;
        nlohmann::json object = nlohmann::json::object();
        for (std::size_t index = 0; index < columns.size(); ++index) {
            object[columns[index]] =
                index < row.size() ? snowflake_cell(row[index], types[index])
                                   : nlohmann::json(nullptr);
        }
        out.rows.push_back(std::move(object));
    }
}

// The error text a non-2xx SQL API response carries (`message`, else the body).
std::string snowflake_status_error(const HttpResponse& response) {
    const auto body =
        nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    std::string detail;
    if (body.is_object()) {
        const auto message = body.find("message");
        if (message != body.end() && message->is_string()) {
            detail = message->get<std::string>();
        }
    }
    if (detail.empty()) detail = response.body.substr(0, 400);
    return "snowflake: HTTP " + std::to_string(response.status) +
           (detail.empty() ? std::string() : " — " + detail);
}

// How long to wait for an asynchronously-executing statement before giving up.
// The SQL API answers 202 while a statement is still running; we poll its
// handle. This is a synchronous data-load action (like add_dataset), so the
// wait happens on the caller's thread.
constexpr int kMaxAsyncPolls = 40;
constexpr int kAsyncPollMillis = 500;

ConnectorResult run_snowflake_query(const ConnectorConfig& config,
                                    const std::string& sql, std::size_t max_rows,
                                    const HttpTransport& transport) {
    ConnectorResult result;

    HttpRequest request;
    request.method = "POST";
    request.url = snowflake_statements_url(config);
    request.headers = snowflake_headers(config);
    request.secret_headers = {"Authorization"};
    nlohmann::json payload{{"statement", sql}, {"timeout", 60}};
    if (!config.database.empty()) payload["database"] = config.database;
    if (!config.schema.empty()) payload["schema"] = config.schema;
    if (!config.warehouse.empty()) payload["warehouse"] = config.warehouse;
    if (!config.role.empty()) payload["role"] = config.role;
    request.body = payload.dump();

    HttpResponse response = transport(request);
    if (!response.error.empty()) {
        result.error = "snowflake: " + response.error;
        return result;
    }

    // 202 => still executing: poll the statement handle until it completes.
    int polls = 0;
    while (response.status == 202) {
        const auto body =
            nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
        std::string handle;
        if (body.is_object()) {
            const auto found = body.find("statementHandle");
            if (found != body.end() && found->is_string()) {
                handle = found->get<std::string>();
            }
        }
        if (handle.empty()) {
            result.error = "snowflake: async response carried no statementHandle";
            return result;
        }
        if (++polls > kMaxAsyncPolls) {
            result.error =
                "snowflake: statement still running after " +
                std::to_string(kMaxAsyncPolls * kAsyncPollMillis / 1000) +
                "s (narrow the query, or pre-materialize it in the warehouse)";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kAsyncPollMillis));

        HttpRequest poll;
        poll.method = "GET";
        poll.url = snowflake_statements_url(config) + "/" + handle;
        poll.headers = snowflake_headers(config);
        poll.secret_headers = {"Authorization"};
        response = transport(poll);
        if (!response.error.empty()) {
            result.error = "snowflake: " + response.error;
            return result;
        }
    }

    if (!response.ok()) {
        result.error = snowflake_status_error(response);
        return result;
    }

    const auto body =
        nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    if (!body.is_object()) {
        result.error = "snowflake: response body is not a JSON object";
        return result;
    }

    std::vector<std::string> types;
    if (!read_row_types(body, result.data.columns, types, result.error)) {
        return result;
    }
    append_data(body, result.data.columns, types, max_rows, result.data);

    // A large result set is split across partitions; the first arrives inline,
    // the rest are fetched by index off the same statement handle.
    const auto metadata = body.find("resultSetMetaData");
    std::string handle;
    const auto handle_value = body.find("statementHandle");
    if (handle_value != body.end() && handle_value->is_string()) {
        handle = handle_value->get<std::string>();
    }
    if (metadata != body.end() && metadata->is_object() && !handle.empty()) {
        const auto partitions = metadata->find("partitionInfo");
        if (partitions != metadata->end() && partitions->is_array()) {
            for (std::size_t index = 1;
                 index < partitions->size() && result.data.rows.size() < max_rows;
                 ++index) {
                HttpRequest fetch;
                fetch.method = "GET";
                fetch.url = snowflake_statements_url(config) + "/" + handle +
                            "?partition=" + std::to_string(index);
                fetch.headers = snowflake_headers(config);
                fetch.secret_headers = {"Authorization"};
                const HttpResponse page = transport(fetch);
                if (!page.error.empty()) {
                    result.error = "snowflake: partition " + std::to_string(index) +
                                   ": " + page.error;
                    return result;
                }
                if (!page.ok()) {
                    result.error = snowflake_status_error(page);
                    return result;
                }
                const auto page_body = nlohmann::json::parse(
                    page.body, nullptr, /*allow_exceptions=*/false);
                if (!page_body.is_object()) {
                    result.error = "snowflake: partition " + std::to_string(index) +
                                   " body is not a JSON object";
                    return result;
                }
                append_data(page_body, result.data.columns, types, max_rows,
                            result.data);
            }
        }
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
        return run_snowflake_query(config, sql, max_rows, exchange);
    }
    result.error = "unknown connector type '" + config.type + "'";
    return result;
}

}  // namespace zetta::connect
