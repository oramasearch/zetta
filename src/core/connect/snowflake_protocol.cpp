// SPDX-License-Identifier: MIT

#include "snowflake_protocol.hpp"

#include <cctype>
#include <utility>

namespace zetta::connect::snowflake {
namespace {

std::string to_lower(std::string text) {
    for (char& character : text) {
        character =
            static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

}  // namespace

std::string statements_url(const ConnectorConfig& config) {
    return "https://" + config.account + ".snowflakecomputing.com/api/v2/statements";
}

std::vector<std::pair<std::string, std::string>> headers(
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

HttpRequest partition_request(const ConnectorConfig& config,
                              const std::string& handle, long long partition) {
    HttpRequest request;
    request.destinations = DestinationPolicy::Connector;
    request.method = "GET";
    request.url = statements_url(config) + "/" + handle;
    if (partition >= 0) {
        request.url += "?partition=" + std::to_string(partition);
    }
    request.headers = headers(config);
    request.secret_headers = {"Authorization"};
    return request;
}

nlohmann::json cell(const nlohmann::json& raw, const std::string& type) {
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

void append_data(const nlohmann::json& body,
                 const std::vector<std::string>& columns,
                 const std::vector<std::string>& types, std::size_t max_rows,
                 std::vector<nlohmann::json>& rows) {
    const auto data = body.find("data");
    if (data == body.end() || !data->is_array()) return;
    for (const nlohmann::json& row : *data) {
        if (rows.size() >= max_rows) return;
        if (!row.is_array()) continue;
        nlohmann::json object = nlohmann::json::object();
        for (std::size_t index = 0; index < columns.size(); ++index) {
            object[columns[index]] =
                index < row.size()
                    ? cell(row[index],
                           index < types.size() ? types[index] : std::string())
                    : nlohmann::json(nullptr);
        }
        rows.push_back(std::move(object));
    }
}

std::string status_error(const HttpResponse& response) {
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

std::string statement_handle(const nlohmann::json& body) {
    if (!body.is_object()) return {};
    const auto found = body.find("statementHandle");
    if (found == body.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

std::vector<std::uint64_t> partition_row_counts(const nlohmann::json& body) {
    std::vector<std::uint64_t> counts;
    if (!body.is_object()) return counts;
    const auto metadata = body.find("resultSetMetaData");
    if (metadata == body.end() || !metadata->is_object()) return counts;
    const auto partitions = metadata->find("partitionInfo");
    if (partitions == metadata->end() || !partitions->is_array()) return counts;
    for (const nlohmann::json& partition : *partitions) {
        std::uint64_t rows = 0;
        if (partition.is_object()) {
            const auto count = partition.find("rowCount");
            if (count != partition.end() && count->is_number_unsigned()) {
                rows = count->get<std::uint64_t>();
            } else if (count != partition.end() && count->is_number_integer() &&
                       count->get<long long>() > 0) {
                rows = static_cast<std::uint64_t>(count->get<long long>());
            }
        }
        counts.push_back(rows);
    }
    return counts;
}

}  // namespace zetta::connect::snowflake
