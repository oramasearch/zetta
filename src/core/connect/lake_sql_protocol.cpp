#include "lake_sql_protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <utility>
#include <vector>

namespace zetta::connect::lake_sql {
namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

bool is_word_start(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
}

bool is_word_char(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_' ||
           character == '$';
}

// The bare words of `sql`, lower-cased, in order — everything outside a
// '…' string, a "…" identifier, a `--` comment and a `/* … */` comment.
// Only the two quote forms all three engines share are skipped: a form one
// engine does not treat as a quote (backticks, brackets, $$) is scanned as
// code, so a disagreement can only refuse more, never admit more.
std::vector<std::string> bare_words(std::string_view sql) {
    std::vector<std::string> words;
    std::size_t index = 0;
    while (index < sql.size()) {
        const char character = sql[index];
        if (character == '\'' || character == '"') {
            // A doubled quote inside is an escaped quote, not the end.
            ++index;
            while (index < sql.size()) {
                if (sql[index] == character) {
                    if (index + 1 < sql.size() && sql[index + 1] == character) {
                        index += 2;
                        continue;
                    }
                    break;
                }
                ++index;
            }
            ++index;
            continue;
        }
        if (sql.substr(index).starts_with("--")) {
            const std::size_t line_end = sql.find('\n', index);
            index = line_end == std::string_view::npos ? sql.size() : line_end + 1;
            continue;
        }
        if (sql.substr(index).starts_with("/*")) {
            const std::size_t close = sql.find("*/", index + 2);
            index = close == std::string_view::npos ? sql.size() : close + 2;
            continue;
        }
        if (is_word_start(character) &&
            (index == 0 || !is_word_char(sql[index - 1]))) {
            std::size_t end = index;
            while (end < sql.size() && is_word_char(sql[end])) ++end;
            words.push_back(lowered(sql.substr(index, end - index)));
            index = end;
            continue;
        }
        ++index;
    }
    return words;
}

// Words that start a write, change the schema, touch files or extensions, or
// change the session or transaction, in SQLite, DuckDB or Polars SQL.
constexpr std::array<std::string_view, 30> kRefusedWords = {
    "insert",  "update",   "delete",     "merge",    "upsert",  "create",
    "drop",    "alter",    "truncate",   "attach",   "detach",  "pragma",
    "vacuum",  "reindex",  "analyze",    "copy",     "install", "load",
    "export",  "import",   "call",       "checkpoint", "set",   "reset",
    "begin",   "commit",   "rollback",   "savepoint", "grant",  "revoke"};

bool parse_port(std::string_view text) {
    unsigned port = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), port);
    return error == std::errc() && end == text.data() + text.size() && port > 0 &&
           port <= 65535;
}

// `object[key]` when it is a string, else empty — never throws on a wrong type.
std::string string_field(const nlohmann::json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>()
                                                       : std::string{};
}

std::string authorization(const ConnectorConfig& config) {
    return "Bearer " + config.token;
}

}  // namespace

bool is_lake_sql_type(std::string_view type) {
    return type == "lake_sqlite" || type == "lake_duckdb" || type == "lake_polars";
}

std::string engine_name(std::string_view type) {
    if (type == "lake_sqlite") return "SQLite";
    if (type == "lake_duckdb") return "DuckDB";
    if (type == "lake_polars") return "Polars";
    return std::string(type);
}

std::string Endpoint::base_url() const {
    std::string url = secure ? "https://" : "http://";
    url += host.find(':') != std::string::npos ? "[" + host + "]" : host;
    if (!port.empty()) url += ":" + port;
    url += path;
    return url;
}

std::optional<Endpoint> parse_endpoint(std::string_view endpoint, std::string* why) {
    const auto refuse = [why](std::string reason) -> std::optional<Endpoint> {
        if (why) *why = std::move(reason);
        return std::nullopt;
    };
    Endpoint parsed;
    std::string_view rest = endpoint;
    if (const std::size_t scheme_end = rest.find("://");
        scheme_end != std::string_view::npos) {
        const std::string scheme = lowered(rest.substr(0, scheme_end));
        if (scheme == "http") {
            parsed.secure = false;
        } else if (scheme != "https") {
            return refuse("endpoint: only http:// and https:// are supported");
        }
        rest.remove_prefix(scheme_end + 3);
    }
    if (rest.find_first_of("?#") != std::string_view::npos) {
        return refuse("endpoint: a query or fragment is not allowed");
    }
    const std::size_t slash = rest.find('/');
    std::string_view authority = rest.substr(0, slash);
    std::string_view path =
        slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    if (authority.find('@') != std::string_view::npos) {
        return refuse(
            "endpoint: credentials in the URL are not allowed — the token goes "
            "in `token`");
    }
    if (authority.starts_with("[")) {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos) return refuse("endpoint: unclosed [");
        parsed.host = std::string(authority.substr(1, close - 1));
        authority.remove_prefix(close + 1);
        if (!authority.empty()) {
            if (!authority.starts_with(":")) return refuse("endpoint: bad port");
            parsed.port = std::string(authority.substr(1));
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        parsed.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos) {
            parsed.port = std::string(authority.substr(colon + 1));
        }
    }
    if (parsed.host.empty()) return refuse("endpoint: no host");
    if (std::ranges::any_of(parsed.host, [](unsigned char character) {
            return std::isspace(character) || std::iscntrl(character);
        })) {
        return refuse("endpoint: the host holds whitespace");
    }
    if (!parsed.port.empty() && !parse_port(parsed.port)) {
        return refuse("endpoint: the port must be 1-65535");
    }
    while (path.ends_with("/")) path.remove_suffix(1);
    parsed.path = std::string(path);
    return parsed;
}

std::string query_rejection(const std::string& sql) {
    const std::vector<std::string> words = bare_words(sql);
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::string& word = words[index];
        const bool refused =
            std::ranges::find(kRefusedWords, word) != kRefusedWords.end() ||
            // `replace(…)` is a string function; `REPLACE INTO` is a write.
            (word == "replace" && index + 1 < words.size() &&
             words[index + 1] == "into");
        if (refused) {
            return "'" + word +
                   "' may not appear in a query through a lake service connector "
                   "(it writes or changes the session); only a read-only SELECT "
                   "may run — quote an identifier that has this name";
        }
    }
    return {};
}

nlohmann::json cell(const nlohmann::json& raw) {
    if (raw.is_object()) {
        if (raw.size() == 1) {
            const auto base64 = raw.find("$base64");
            if (base64 != raw.end() && base64->is_string()) return *base64;
        }
        return raw.dump();
    }
    if (raw.is_array()) return raw.dump();
    return raw;
}

HttpRequest query_request(const ConnectorConfig& config, const Endpoint& endpoint,
                          const std::string& sql) {
    HttpRequest request;
    request.method = "POST";
    request.url = endpoint.base_url() + "/v1/query";
    request.headers = {{"Content-Type", "application/json"},
                       {"Accept", "application/json"},
                       {"Authorization", authorization(config)}};
    request.secret_headers = {"Authorization"};
    // Only `sql`: every service refuses unknown fields.
    request.body = nlohmann::json{{"sql", sql}}.dump();
    // The services stop a query at 30 s; leave room for the answer.
    request.timeout_seconds = 60;
    return request;
}

HttpRequest datasets_request(const ConnectorConfig& config, const Endpoint& endpoint) {
    HttpRequest request;
    request.method = "GET";
    request.url = endpoint.base_url() + "/v1/datasets";
    request.headers = {{"Accept", "application/json"},
                       {"Authorization", authorization(config)}};
    request.secret_headers = {"Authorization"};
    request.timeout_seconds = 60;
    return request;
}

namespace {

// The reason a failed exchange carries. Built from the engine, the endpoint and
// what the service said — never from a header or the token.
std::string exchange_error(const ConnectorConfig& config, const Endpoint& endpoint,
                           const HttpResponse& response) {
    const std::string where =
        "the " + engine_name(config.type) + " service at " + endpoint.base_url();
    if (!response.error.empty()) {
        return "could not reach " + where + ": " + response.error;
    }
    if (response.status == 401) {
        return where + " refused the token (401 unauthorized)";
    }
    std::string detail;
    const nlohmann::json body =
        nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    if (!body.is_discarded() && body.is_object()) {
        const auto error = body.find("error");
        if (error != body.end() && error->is_object()) {
            const std::string code = string_field(*error, "code");
            const std::string message = string_field(*error, "message");
            detail = code.empty() ? message : code + ": " + message;
        }
    }
    if (detail.empty()) detail = response.body.substr(0, 300);
    return where + " answered " + std::to_string(response.status) +
           (detail.empty() ? std::string{} : " " + detail);
}

}  // namespace

ConnectorResult read_query_response(const ConnectorConfig& config,
                                    const Endpoint& endpoint,
                                    const HttpResponse& response,
                                    std::size_t max_rows) {
    ConnectorResult result;
    if (!response.ok()) {
        result.error = exchange_error(config, endpoint, response);
        return result;
    }
    const nlohmann::json body =
        nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    const auto malformed = [&](const std::string& what) {
        result.error = "the " + engine_name(config.type) +
                       " service answered a malformed result: " + what;
        return result;
    };
    if (body.is_discarded() || !body.is_object()) return malformed("not a JSON object");
    const auto columns = body.find("columns");
    const auto rows = body.find("rows");
    if (columns == body.end() || !columns->is_array()) return malformed("no columns");
    if (rows == body.end() || !rows->is_array()) return malformed("no rows");
    for (const nlohmann::json& column : *columns) {
        if (!column.is_string()) return malformed("a column name is not a string");
        result.data.columns.push_back(column.get<std::string>());
    }
    const auto truncated_field = body.find("truncated");
    const bool truncated = truncated_field != body.end() &&
                           truncated_field->is_boolean() &&
                           truncated_field->get<bool>();
    for (const nlohmann::json& row : *rows) {
        if (result.data.rows.size() >= max_rows) break;
        if (!row.is_array() || row.size() != result.data.columns.size()) {
            return malformed("a row does not match the columns");
        }
        nlohmann::json object = nlohmann::json::object();
        for (std::size_t index = 0; index < row.size(); ++index) {
            object[result.data.columns[index]] = cell(row[index]);
        }
        result.data.rows.push_back(std::move(object));
    }
    if (truncated && result.data.rows.size() < max_rows) {
        // The service stopped at its own cap, short of what was asked for.
        // Landing those rows would be a silently partial dataset.
        result.data = {};
        result.error =
            "the " + engine_name(config.type) + " service returns at most " +
            std::to_string(kServiceRowCap) +
            " rows per query and this result has more — narrow the query (a "
            "WHERE or an aggregate), or set max_rows to " +
            std::to_string(kServiceRowCap) + " or fewer to take the first rows";
    }
    return result;
}

ConnectorResult run_query(const ConnectorConfig& config, const std::string& sql,
                          std::size_t max_rows, const HttpTransport& transport) {
    std::string why;
    const std::optional<Endpoint> endpoint = parse_endpoint(config.endpoint, &why);
    if (!endpoint) {
        ConnectorResult result;
        result.error = why;
        return result;
    }
    return read_query_response(config, *endpoint,
                               transport(query_request(config, *endpoint, sql)),
                               max_rows);
}

ConnectorResult list_datasets(const ConnectorConfig& config,
                              const HttpTransport& transport) {
    ConnectorResult result;
    std::string why;
    const std::optional<Endpoint> endpoint = parse_endpoint(config.endpoint, &why);
    if (!endpoint) {
        result.error = why;
        return result;
    }
    const HttpResponse response = transport(datasets_request(config, *endpoint));
    if (!response.ok()) {
        result.error = exchange_error(config, *endpoint, response);
        return result;
    }
    const nlohmann::json body =
        nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
    const auto datasets = body.is_object() ? body.find("datasets") : body.end();
    if (body.is_discarded() || !body.is_object() || datasets == body.end() ||
        !datasets->is_array()) {
        result.error = "the Polars service answered a malformed dataset list";
        return result;
    }
    result.data.columns = {"table_name", "rows", "columns"};
    for (const nlohmann::json& dataset : *datasets) {
        if (!dataset.is_object()) continue;
        const auto name = dataset.find("name");
        if (name == dataset.end() || !name->is_string()) continue;
        nlohmann::json columns = nlohmann::json::array();
        if (const auto listed = dataset.find("columns");
            listed != dataset.end() && listed->is_array()) {
            for (const nlohmann::json& column : *listed) {
                if (!column.is_object()) continue;
                columns.push_back({{"name", string_field(column, "name")},
                                   {"type", string_field(column, "type")}});
            }
        }
        const auto row_count = dataset.find("rows");
        result.data.rows.push_back(
            {{"table_name", *name},
             {"rows", row_count != dataset.end() && row_count->is_number_integer()
                          ? *row_count
                          : nlohmann::json(nullptr)},
             {"columns", std::move(columns)}});
    }
    return result;
}

}  // namespace zetta::connect::lake_sql
