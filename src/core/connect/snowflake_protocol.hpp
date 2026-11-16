// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/connect/connector.hpp"
#include "zetta/connect/http_transport.hpp"

// **Internal** — the Snowflake SQL API v2 wire details, shared by the two TUs
// that speak it: `connector.cpp` (submit, poll, single-node read) and
// `distributed_fetch.cpp` (the tiled, concurrent partition read). Not a public
// header; nothing outside `src/core/connect/` includes it.
//
// Everything here is pure request-building / response-reading — no I/O, no
// transport — so both callers get identical framing and typing without one
// reaching into the other. See docs/data/connector-query.md.
namespace zetta::connect::snowflake {

// The statements endpoint for an account.
std::string statements_url(const ConnectorConfig& config);

// The headers every request carries. The bearer is the credential; callers must
// name "Authorization" in `HttpRequest::secret_headers`.
std::vector<std::pair<std::string, std::string>> headers(
    const ConnectorConfig& config);

// A GET against the statement handle, optionally for one result partition
// (`partition < 0` = the statement itself, used while polling a 202).
HttpRequest partition_request(const ConnectorConfig& config,
                              const std::string& handle, long long partition);

// Coerce one SQL-API cell to a real JSON type using its declared column type.
// The API returns every value as a JSON string (or null), so without this a
// landed dataset would be all strings and nothing downstream could featurize it.
nlohmann::json cell(const nlohmann::json& raw, const std::string& type);

// Pull `resultSetMetaData.rowType` into column names + declared types. Returns
// false with `error` set when the response does not carry it.
bool read_row_types(const nlohmann::json& body, std::vector<std::string>& columns,
                    std::vector<std::string>& types, std::string& error);

// Append one `data` array (an array of row arrays) as typed row objects, capped
// at `max_rows` total.
void append_data(const nlohmann::json& body,
                 const std::vector<std::string>& columns,
                 const std::vector<std::string>& types, std::size_t max_rows,
                 std::vector<nlohmann::json>& rows);

// The error text a non-2xx SQL API response carries (`message`, else the body).
std::string status_error(const HttpResponse& response);

// The `statementHandle` a response carries, or empty.
std::string statement_handle(const nlohmann::json& body);

// The per-partition row counts from `resultSetMetaData.partitionInfo[]`. Empty
// when the response declares none (a small result answered wholly inline).
std::vector<std::uint64_t> partition_row_counts(const nlohmann::json& body);

}  // namespace zetta::connect::snowflake
