// The Postgres source connector — the libpqxx adapter (#892).
//
// This is the ONE translation unit that names libpqxx / libpq (the module-split
// rule, like Corosio in cluster.cpp). It lives in the zetta-api target so
// zetta-core links no libpq; it installs itself into the connector interface
// (postgres_runner.hpp) at startup via install_postgres_protocol().
//
// **Coroutine-wrapped so it never disturbs zetta's threading.** libpqxx is
// blocking; every libpq call runs inside `runtime::offload`, i.e. on a runtime
// pool worker, so no zetta network/main/coroutine thread is ever pinned by a DB
// round-trip. The synchronous `ConnectorResult(...)` the interface expects is
// `block_on(offload(...))` — the blocking work is on the pool, the result flows
// back.
//
// **Read-only by construction (three ways):** the shared SQL guard
// (`read_only_query_rejection`, SELECT/WITH only, single statement) runs at the
// connector boundary before we get here; the query executes inside a
// `pqxx::read_transaction` (a `SET TRANSACTION READ ONLY` session); and the
// transaction is **never committed** (it aborts on scope exit). A row cap wraps
// the user query in a bounded subquery.

#include "zetta/api/postgres_protocol.hpp"

#include "zetta/connect/postgres_runner.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/connector_destinations.hpp"
#include "zetta/runtime.hpp"

#include <arpa/inet.h>
#include <netdb.h>

#include <array>
#include <memory>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

namespace zetta::connect {
namespace {

// Quote a libpq connection-string value: wrap in single quotes, backslash-escape
// embedded backslashes and quotes (the libpq keyword/value rules).
std::string quote_conninfo(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\\' || c == '\'') out += '\\';
        out += c;
    }
    out += '\'';
    return out;
}

// Build a libpq connection string from the config. The password rides in `token`.
std::string connection_string(const ConnectorConfig& config, const std::string& address) {
    std::string conn;
    const auto add = [&](const char* key, const std::string& value) {
        if (value.empty()) return;
        if (!conn.empty()) conn += ' ';
        conn += key;
        conn += '=';
        conn += quote_conninfo(value);
    };
    // host retains the TLS hostname; hostaddr pins libpq to the checked address
    // and prevents a second DNS lookup (including a PGHOSTADDR environment default).
    add("host", config.host);
    add("hostaddr", address);
    add("port", config.port.empty() ? std::string("5432") : config.port);
    add("user", config.user);
    add("password", config.token);
    add("dbname", config.database);
    add("sslmode", config.sslmode);
    // Query text and JSON are UTF-8. An inherited multibyte client encoding
    // must not change the character boundaries of escaped catalog literals.
    add("client_encoding", "UTF8");
    // Never block a pool worker forever on an unreachable host.
    add("connect_timeout", "10");
    return conn;
}

// Map a Postgres type OID to a JSON coercion of the field's text value, so the
// landed dataset is directly featurizable (numbers stay numbers). Unknown types
// stay strings — safe, lossless.
nlohmann::json coerce(const pqxx::field& field, pqxx::oid type_oid) {
    if (field.is_null()) return nullptr;
    const char* text = field.c_str();
    switch (type_oid) {
        case 16:  // bool
            return field.c_str()[0] == 't';
        case 20:  // int8
        case 21:  // int2
        case 23:  // int4
            try { return field.as<long long>(); } catch (...) { return std::string(text); }
        case 700:   // float4
        case 701:   // float8
        case 1700:  // numeric
            try { return field.as<double>(); } catch (...) { return std::string(text); }
        default:
            return std::string(text);
    }
}

// The blocking body — runs on a runtime pool worker via offload. Non-throwing:
// every libpqxx/libpq failure comes back as ConnectorResult::error.
ConnectorResult run_blocking(const ConnectorConfig& config, const std::string& sql,
                             std::size_t max_rows) {
    ConnectorResult result;
    if (config.type != "postgres") {
        result.error = "postgres: expected connector.type='postgres'";
        return result;
    }
    if (const auto reason = config.destination_rejection(); !reason.empty()) {
        result.error = reason;
        return result;
    }
    // This adapter already runs on the offload pool. Resolve once there, check
    // every candidate, then give libpq only checked numeric hostaddr entries.
    const auto policy = connector_destinations();
    const std::string port = config.port.empty() ? "5432" : config.port;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* resolved = nullptr;
    const int failure = ::getaddrinfo(config.host.c_str(), port.c_str(), &hints, &resolved);
    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(resolved, ::freeaddrinfo);
    if (failure != 0) {
        result.error = "postgres: could not resolve connector host";
        return result;
    }
    std::string pinned_addresses;
    std::string pinned_hosts;
    std::string pinned_ports;
    for (const addrinfo* candidate = addresses.get(); candidate; candidate = candidate->ai_next) {
        std::array<char, INET6_ADDRSTRLEN> buffer{};
        const void* bytes = nullptr;
        if (candidate->ai_family == AF_INET) {
            bytes = &reinterpret_cast<const sockaddr_in*>(candidate->ai_addr)->sin_addr;
        } else if (candidate->ai_family == AF_INET6) {
            bytes = &reinterpret_cast<const sockaddr_in6*>(candidate->ai_addr)->sin6_addr;
        } else
            continue;
        if (!::inet_ntop(candidate->ai_family, bytes, buffer.data(), buffer.size())) continue;
        const std::string address(buffer.data());
        if (!policy.allows(config.host, port, address)) continue;
        if (!pinned_addresses.empty()) {
            pinned_addresses += ',';
            pinned_hosts += ',';
            pinned_ports += ',';
        }
        pinned_addresses += address;
        pinned_hosts += config.host;
        pinned_ports += port;
    }
    if (pinned_addresses.empty()) {
        result.error = "destination refused: Postgres connector has no approved resolved address";
        return result;
    }
    ConnectorConfig pinned = config;
    // Matching host/hostaddr/port lists preserve multi-address failover without
    // allowing libpq to select an unvalidated hostname or a socket directory.
    pinned.host = std::move(pinned_hosts);
    pinned.port = std::move(pinned_ports);
    try {
        pqxx::connection conn(connection_string(pinned, pinned_addresses));
        // A read-only transaction: SET TRANSACTION READ ONLY, and never
        // committed (it aborts on scope exit) — the source cannot be mutated.
        pqxx::read_transaction tx(conn);
        // Cap the rows by wrapping the (already SELECT/WITH-guarded) query in a
        // bounded subquery. A WITH query is a valid subquery in Postgres.
        const std::string capped = "SELECT * FROM (" + sql +
                                   ") AS zetta_connector_q LIMIT " +
                                   std::to_string(max_rows);
        const pqxx::result rows = tx.exec(capped);

        for (pqxx::row_size_type col = 0; col < rows.columns(); ++col) {
            result.data.columns.emplace_back(rows.column_name(col));
        }
        for (const pqxx::row& row : rows) {
            nlohmann::json object = nlohmann::json::object();
            for (pqxx::row_size_type col = 0; col < rows.columns(); ++col) {
                object[rows.column_name(col)] = coerce(row[col], rows.column_type(col));
            }
            result.data.rows.emplace_back(std::move(object));
        }
        // No tx.commit() — the read-only transaction rolls back on scope exit.
    } catch (const std::exception& error) {
        result.data = {};
        result.error = std::string("postgres: ") + error.what();
    }
    return result;
}

}  // namespace

// The installed runner: wrap the blocking body in offload so the libpq call runs
// on a pool worker, and block_on it to satisfy the synchronous interface. Inputs are
// captured by value into the offload frame.
void install_postgres_protocol() {
    install_postgres_query_runner(
        [](const ConnectorConfig& config, const std::string& sql,
           std::size_t max_rows) -> ConnectorResult {
            ConnectorConfig config_copy = config;
            std::string sql_copy = sql;
            return runtime::block_on(runtime::offload(
                [config_copy = std::move(config_copy), sql_copy = std::move(sql_copy),
                 max_rows]() -> ConnectorResult {
                    return run_blocking(config_copy, sql_copy, max_rows);
                }));
        });
}

}  // namespace zetta::connect
