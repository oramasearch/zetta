#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/connect/connector.hpp"

// The **MySQL source connector** — a native implementation of the MySQL
// client/server protocol (#895). No libmysqlclient, no MariaDB Connector/C: the
// protocol is implemented here, per the first principle (ADR-17: "if a
// capability needs a protocol, implement the protocol").
//
// The protocol is split along the module boundary the HTTP client already uses:
//
//   * **This header (zetta-core)** holds the protocol itself as a *sans-I/O*
//     state machine, `MysqlClientSession`: it is fed the bytes a socket produced
//     and answers with the bytes to write next and what to do after (read more,
//     upgrade to TLS, or stop). It never touches a socket, so every packet path
//     — the handshake, TLS negotiation, each authentication plugin, the result
//     set, the read-only fencing — is unit-tested against a scripted server with
//     no network (tests/connect/test_mysql_connector.cpp).
//   * **`src/core/api/mysql_protocol.cpp` (zetta-api)** drives a session over a
//     real coroutine TCP + TLS stream (`zetta::runtime::net`) and installs that
//     driver into the injection point below at startup. zetta-core names no
//     socket library.
//
// **Read-only, enforced at the driver** (on top of the connector's SQL guard,
// `connector_query_rejection`): the session never offers CLIENT_MULTI_STATEMENTS
// or CLIENT_LOCAL_FILES, it marks the session and the transaction READ ONLY
// before the caller's statement runs (`SET SESSION TRANSACTION READ ONLY`, then
// `START TRANSACTION READ ONLY`), it caps the rows server-side
// (`sql_select_limit`) and client-side, and it never commits — it rolls back.
//
// **TiDB** speaks the same protocol (connector type `tidb`, default port 4000;
// a `mysql` connection that reaches a TiDB is treated the same). TiDB parses
// `READ ONLY` but does not enforce it, so when the greeting names TiDB the
// session refuses a statement holding any word that writes (before any
// credential is sent), skips the READ ONLY statements, and runs the statement
// in a plain `START TRANSACTION` that is rolled back.
//
// **TLS by default.** An empty `sslmode` means `verify-full`: the connection is
// upgraded to TLS before any credential is sent, the server certificate is
// verified against the system trust store and must match the host. A server
// that does not offer TLS is refused before the password leaves the process.
// `require` encrypts without verifying (a self-signed server), `verify-ca`
// verifies the chain but not the name, and `disable` is plaintext, which must be
// asked for explicitly.
namespace zetta::connect {

// How the connection is protected — the connector's `sslmode` field.
enum class MysqlTlsMode {
    Disable,     // plaintext; only when asked for by name
    Require,     // TLS, certificate not verified
    VerifyCa,    // TLS, chain verified against the system trust store
    VerifyFull,  // TLS, chain verified and the certificate must match the host (default)
};

// Parse a connector `sslmode` for MySQL. Accepts the libpq-style spellings
// (`disable`, `require`, `verify-ca`, `verify-full`) and MySQL's own
// `--ssl-mode` spellings (`DISABLED`, `REQUIRED`, `VERIFY_CA`, `VERIFY_IDENTITY`),
// case-insensitively. Empty means the default, `verify-full`. Anything else —
// including `prefer`/`allow`, which would silently fall back to plaintext — is
// rejected (nullopt).
std::optional<MysqlTlsMode> parse_mysql_tls_mode(std::string_view sslmode);

// The canonical name of a mode (`disable`, `require`, `verify-ca`, `verify-full`).
std::string_view mysql_tls_mode_name(MysqlTlsMode mode);

// The challenge responses of the two password plugins MySQL servers default to.
// `nonce` is the 20-byte scramble the server sent; an empty password answers
// with an empty response, as the protocol requires.
//
//   mysql_native_password:  SHA1(password) XOR SHA1(nonce + SHA1(SHA1(password)))
//   caching_sha2_password:  SHA256(password) XOR SHA256(SHA256(SHA256(password)) + nonce)
std::string mysql_native_password_response(std::string_view password,
                                           std::string_view nonce);
std::string caching_sha2_password_response(std::string_view password,
                                           std::string_view nonce);

// Everything one session needs. `password` is the connector's `token`; it is
// written to the wire only as a challenge response, or in clear only inside an
// established TLS session, and it never appears in an error.
struct MysqlSessionOptions {
    std::string user;
    std::string password;
    std::string database;  // optional default schema
    std::string sql;       // the caller's statement, already through the guard
    std::size_t max_rows = 1;
    MysqlTlsMode tls_mode = MysqlTlsMode::VerifyFull;
};

// What the driver does after writing `send` (which may be empty).
struct MysqlStep {
    enum class Action {
        Read,      // read more bytes and pass them to receive()
        StartTls,  // run the TLS client handshake on this connection, then call tls_established()
        Finished,  // stop; close the connection; take_result()
    };
    Action action = Action::Read;
    std::string send;
};

// The protocol, as a sans-I/O state machine. One query per session: connect,
// negotiate, authenticate, fence the session read-only, run the statement, read
// the rows, roll back, quit. Non-throwing; every failure is a finished session
// whose result carries a `mysql: …` reason.
class MysqlClientSession {
public:
    explicit MysqlClientSession(MysqlSessionOptions options);

    // Feed bytes read from the connection (any amount, split anywhere).
    MysqlStep receive(std::string_view bytes);

    // The TLS handshake the session asked for (StartTls) completed.
    MysqlStep tls_established();

    // The transport failed or the peer hung up before the session finished.
    MysqlStep transport_failed(std::string reason);

    bool finished() const { return phase_ == Phase::Done; }

    // The outcome — rows when ok(). Valid once finished().
    const ConnectorResult& result() const { return result_; }
    ConnectorResult take_result() { return std::move(result_); }

    // Diagnostics for the driver and tests: the server version string from the
    // greeting, and whether the session ran over TLS.
    const std::string& server_version() const { return server_version_; }
    bool tls_active() const { return tls_active_; }
    // Whether the greeting named TiDB (and the session is fenced TiDB's way).
    bool tidb() const { return tidb_; }

private:
    enum class Phase { Greeting, Tls, Auth, Command, Columns, Rows, Rollback, Done };

    MysqlStep handle_packet(std::string_view payload);
    MysqlStep on_greeting(std::string_view payload);
    MysqlStep on_auth(std::string_view payload);
    MysqlStep on_command_response(std::string_view payload);
    MysqlStep on_column(std::string_view payload);
    MysqlStep on_row(std::string_view payload);
    MysqlStep on_rollback(std::string_view payload);

    MysqlStep send_handshake_response();
    MysqlStep send_next_command();
    MysqlStep fail(std::string reason);
    MysqlStep finish_with_quit();
    std::string frame(std::string_view payload);
    std::string auth_response_for(const std::string& plugin, std::string& reason) const;

    MysqlSessionOptions options_;
    Phase phase_ = Phase::Greeting;
    std::string buffer_;           // bytes received and not yet framed
    std::string partial_payload_;  // a logical packet split over 16 MiB frames
    std::uint8_t sequence_ = 0;    // the last sequence id seen
    std::uint32_t server_capabilities_ = 0;
    std::uint32_t client_capabilities_ = 0;
    std::string nonce_;
    std::string auth_plugin_;
    std::string server_version_;
    bool tls_active_ = false;
    bool tidb_ = false;

    std::vector<std::string> commands_;  // the fencing statements, then the query
    std::size_t next_command_ = 0;
    std::size_t column_count_ = 0;
    bool columns_eof_seen_ = false;

    struct Column {
        std::string name;
        std::uint16_t charset = 0;
        std::uint8_t type = 0;
        std::uint16_t flags = 0;
    };
    std::vector<Column> columns_;
    ConnectorResult result_;
};

// Whether a greeting's server version names TiDB (`8.0.11-TiDB-v7.5.1`).
bool is_tidb_version(std::string_view server_version);

// --- The injection point ------------------------------------------------------
//
// zetta-core holds the protocol; the TCP/TLS driver lives in zetta-api
// (`src/core/api/mysql_protocol.cpp`) and installs itself at startup with
// `install_mysql_protocol()`. When none is installed (a unit test that did not
// install it, a tool without the API target) a MySQL query fails with a clear
// reason rather than pretending the server was unreachable.
using MysqlQueryRunner = std::function<ConnectorResult(
    const ConnectorConfig& config, const std::string& sql, std::size_t max_rows)>;

void install_mysql_query_runner(MysqlQueryRunner runner);
bool has_mysql_query_runner();
const MysqlQueryRunner& mysql_query_runner();

}  // namespace zetta::connect
