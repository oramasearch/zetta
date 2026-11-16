// SPDX-License-Identifier: MIT

// The MySQL source connector — the TCP/TLS driver (#895).
//
// The protocol itself is zetta-core's sans-I/O `MysqlClientSession`
// (connect/mysql_client.hpp). This translation unit only moves its bytes: it
// resolves and dials the server, upgrades the connection to TLS when the session
// asks (after the plaintext SSLRequest, as the protocol requires), and feeds
// every read back into the session until it finishes. It is the **third**
// translation unit allowed to name Corosio (through `zetta::runtime::net`),
// alongside `cluster.cpp` (inbound/peer traffic) and `native_http_transport.cpp`
// (outbound HTTPS): this one owns outbound MySQL wire traffic. It lives in
// zetta-api, so zetta-core names no socket library.
//
// **Coroutine I/O, off zetta's threads.** Each query runs on its own
// `io_context`, driven inside `runtime::offload` — on a runtime pool worker —
// so no zetta network/main thread is ever pinned by a database round trip. The
// synchronous `ConnectorResult(...)` the injection point expects is
// `block_on(offload(...))`, exactly the Postgres adapter's contract.
//
// **Credentials:** the password is the connector's `token`, handed to the
// session and never logged; every error string comes from the session or the
// transport and names hosts and server messages, never the secret.

#include "zetta/api/mysql_protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "system_trust_store.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/connector_destinations.hpp"
#include "zetta/connect/mysql_client.hpp"
#include "zetta/runtime.hpp"
#include "zetta/runtime/net.hpp"

namespace zetta::connect {
namespace {

namespace net = zetta::runtime::net;

constexpr auto kConnectTimeout = std::chrono::seconds(10);
constexpr auto kHandshakeTimeout = std::chrono::seconds(15);
// A query may be silent while the server works; a read idle this long is a
// dead connection, not a slow query.
constexpr auto kReadIdleTimeout = std::chrono::seconds(300);

// Write what the session asked for, then read and feed until the session wants
// something other than more bytes (a TLS upgrade, or the end). The returned step
// has already had its `send` written.
template <class Stream>
runtime::io_task<MysqlStep> pump(Stream& stream, MysqlClientSession& session,
                                 MysqlStep step) {
    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        if (!step.send.empty()) {
            auto [write_error, written] = co_await net::write(
                stream, net::const_buffer(step.send.data(), step.send.size()));
            if (write_error && step.action != MysqlStep::Action::Finished) {
                co_return {{}, session.transport_failed("could not write to the server: " +
                                                        write_error.message())};
            }
            // A failed COM_QUIT at the very end loses nothing.
            step.send.clear();
        }
        if (step.action != MysqlStep::Action::Read) co_return {{}, std::move(step)};

        auto [read_error, count] = co_await runtime::timeout(
            stream.read_some(net::mutable_buffer(buffer.data(), buffer.size())),
            kReadIdleTimeout);
        if (read_error) {
            co_return {{}, session.transport_failed(
                               "the connection to the server failed: " +
                               read_error.message())};
        }
        step = session.receive(std::string_view(buffer.data(), count));
    }
}

// Build the client TLS context the mode calls for. Returns false with a reason
// when the trust store cannot be configured.
bool configure_tls(net::tls_context& context, MysqlTlsMode mode, const std::string& host,
                   std::string& reason) {
    if (mode == MysqlTlsMode::Require) {
        // Encrypted, not authenticated: for a server with a self-signed
        // certificate, chosen by name.
        if (const std::error_code error = context.set_verify_mode(net::tls_verify_mode::none)) {
            reason = "could not configure TLS: " + error.message();
            return false;
        }
        return true;
    }
    if (mode == MysqlTlsMode::VerifyFull) {
        context.set_hostname(host);  // SNI + the certificate must match the host
    }
    std::string roots_reason;
    const std::vector<std::string> certificates = api::system_ca_certificates(roots_reason);
    int added = 0;
    for (const std::string& certificate : certificates) {
        if (!context.add_certificate_authority(certificate)) ++added;
    }
    if (added == 0) {
        reason = "could not load the system CA trust store" +
                 (roots_reason.empty() ? std::string() : " (" + roots_reason + ")");
        return false;
    }
    if (const std::error_code error =
            context.set_verify_mode(net::tls_verify_mode::peer)) {
        reason = "could not enable certificate verification: " + error.message();
        return false;
    }
    return true;
}

// One query, start to finish, on `io`. Parameters by value: they live in the
// coroutine frame. `result` outlives the coroutine (the caller runs `io` to
// completion before reading it).
runtime::task<> run_session(net::io_context& io, ConnectorConfig config, std::string sql,
                            std::size_t max_rows, ConnectorResult& result) {
    // Bound to its connector types and re-validated for direct C++ callers
    // (#1414): one DNS host or IP literal, one numeric port, a known TLS mode.
    if (config.type != "mysql" && config.type != "tidb") {
        result.error = "mysql: expected connector.type 'mysql' or 'tidb'";
        co_return;
    }
    if (const std::string reason = config.destination_rejection(); !reason.empty()) {
        result.error = reason;
        co_return;
    }
    const std::optional<MysqlTlsMode> mode = parse_mysql_tls_mode(config.sslmode);
    if (!mode) {
        result.error = "mysql: unknown sslmode '" + config.sslmode +
                       "' (verify-full, verify-ca, require or disable)";
        co_return;
    }
    MysqlSessionOptions options;
    options.user = config.user;
    options.password = config.token;
    options.database = config.database;
    options.sql = std::move(sql);
    options.max_rows = max_rows;
    options.tls_mode = *mode;
    MysqlClientSession session(std::move(options));

    // TiDB's SQL port is 4000; MySQL's is 3306.
    const std::string port = !config.port.empty() ? config.port
                             : config.type == "tidb" ? std::string("4000")
                                                     : std::string("3306");
    const std::string where = config.host + ":" + port;

    net::resolver resolver(io);
    auto [resolve_error, endpoints] = co_await resolver.resolve(config.host, port);
    if (resolve_error) {
        result.error = "mysql: could not resolve " + config.host + ": " +
                       resolve_error.message();
        co_return;
    }
    // The connector destination policy (#1414): every resolved address is
    // checked, and only a checked numeric address is dialled, so no second
    // lookup can change where the connection goes. Public addresses pass;
    // private and loopback ones need an exact operator host:port approval;
    // metadata, link-local and reserved addresses never pass. TLS still
    // verifies the certificate against `config.host`, not the address.
    const ConnectorDestinations policy = connector_destinations();
    net::tcp_socket socket(io);
    socket.open();
    bool connected = false;
    bool any_allowed = false;
    std::string connect_reason = "mysql: no address resolved for " + config.host;
    for (const auto& entry : endpoints) {
        const net::endpoint endpoint = entry.get_endpoint();
        const std::string address = endpoint.is_v4() ? endpoint.v4_address().to_string()
                                                     : endpoint.v6_address().to_string();
        if (!policy.allows(config.host, port, address)) continue;
        any_allowed = true;
        auto [connect_error] =
            co_await runtime::timeout(socket.connect(endpoint), kConnectTimeout);
        if (!connect_error) {
            connected = true;
            break;
        }
        connect_reason = "mysql: could not connect to " + where + ": " +
                         connect_error.message();
    }
    if (!any_allowed) {
        result.error = "destination refused: MySQL connector has no approved resolved address";
        co_return;
    }
    if (!connected) {
        result.error = std::move(connect_reason);
        co_return;
    }

    auto [plain_error, step] = co_await pump(socket, session, MysqlStep{});
    (void)plain_error;
    if (step.action == MysqlStep::Action::StartTls) {
        net::tls_context context;
        std::string tls_reason;
        if (!configure_tls(context, *mode, config.host, tls_reason)) {
            session.transport_failed(tls_reason);
            result = session.take_result();
            co_return;
        }
        // Reference mode: `socket` outlives `secure` (both live in this frame).
        net::openssl_stream secure(&socket, context);
        auto [handshake_error] = co_await runtime::timeout(
            secure.handshake(net::tls_stream::client), kHandshakeTimeout);
        if (handshake_error) {
            session.transport_failed("TLS handshake with " + where + " (sslmode " +
                                     std::string(mysql_tls_mode_name(*mode)) +
                                     ") failed: " + handshake_error.message());
            result = session.take_result();
            co_return;
        }
        auto [tls_error, finished] =
            co_await pump(secure, session, session.tls_established());
        (void)tls_error;
        (void)finished;
        // Best-effort close_notify; the answer is already in hand.
        [[maybe_unused]] const auto shutdown_result =
            co_await runtime::timeout(secure.shutdown(), std::chrono::seconds(2));
    }
    socket.close();
    result = session.take_result();
}

// The blocking body — runs on a runtime pool worker via offload. Owns one
// io_context for the life of the query; run() returns when the session
// coroutine (the only work on it) completes.
ConnectorResult run_blocking(const ConnectorConfig& config, const std::string& sql,
                             std::size_t max_rows) {
    ConnectorResult result;
    net::io_context io;
    runtime::run_async(io.get_executor())(run_session(io, config, sql, max_rows, result));
    io.run();
    return result;
}

}  // namespace

void install_mysql_protocol() {
    install_mysql_query_runner([](const ConnectorConfig& config, const std::string& sql,
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
