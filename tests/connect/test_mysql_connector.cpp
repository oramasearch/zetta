// The native MySQL source connector (#895).
//
// Three layers, each without a MySQL installation:
//
//   1. The boundary: ConnectorConfig::from_json for "mysql", the sslmode
//      parsing, and the MySQL read-only guard (connector_query_rejection).
//   2. The protocol: `MysqlClientSession` (sans I/O) driven packet by packet
//      against a scripted server — the handshake, the TLS negotiation, every
//      authentication path, the read-only fencing, the result set and its
//      typed cells, the row cap, and the refusals.
//   3. The driver: `install_mysql_protocol()` over real loopback sockets against
//      a fake MySQL server on a helper thread, in plaintext and over real TLS
//      (an OpenSSL server with a certificate minted by the cluster CA code), so
//      the coroutine TCP + TLS upgrade path is exercised end to end.
//
// A live run against a real MySQL server is the guarded case at the bottom: it
// runs when ZETTA_MYSQL_TEST_HOST is set (with _PORT/_USER/_PASSWORD/_DB/
// _SSLMODE) and skips otherwise.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include "temp_dir.hpp"
#include "zetta/api/mysql_protocol.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/connector_destinations.hpp"
#include "zetta/connect/mysql_client.hpp"

namespace {

using zetta::connect::ConnectorConfig;
using zetta::connect::MysqlClientSession;
using zetta::connect::MysqlSessionOptions;
using zetta::connect::MysqlStep;
using zetta::connect::MysqlTlsMode;

// --- Server-side packet builders ------------------------------------------------

constexpr std::uint32_t kLongPassword = 0x1, kLongFlag = 0x4, kConnectWithDb = 0x8,
                        kLocalFiles = 0x80, kProtocol41 = 0x200, kSsl = 0x800,
                        kTransactions = 0x2000, kSecureConnection = 0x8000,
                        kMultiStatements = 0x10000, kPluginAuth = 0x80000,
                        kPluginAuthLenenc = 0x200000;

// A modern server offers everything, including the two capabilities the client
// must never take (multi-statements, LOCAL INFILE).
constexpr std::uint32_t kServerCaps = kLongPassword | kLongFlag | kConnectWithDb |
                                      kLocalFiles | kProtocol41 | kTransactions |
                                      kSecureConnection | kMultiStatements |
                                      kPluginAuth | kPluginAuthLenenc;

std::string le16(std::uint32_t value) {
    return {static_cast<char>(value & 0xff), static_cast<char>((value >> 8) & 0xff)};
}
std::string le32(std::uint32_t value) { return le16(value) + le16(value >> 16); }
std::string lenenc(const std::string& text) {
    return std::string(1, static_cast<char>(text.size())) + text;
}

std::string frame(std::uint8_t sequence, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(payload.size() & 0xff));
    out.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
    out.push_back(static_cast<char>((payload.size() >> 16) & 0xff));
    out.push_back(static_cast<char>(sequence));
    return out + payload;
}

std::string test_nonce() {
    std::string nonce;
    for (int value = 1; value <= 20; ++value) nonce.push_back(static_cast<char>(value));
    return nonce;
}

std::string greeting(std::uint32_t caps, const std::string& plugin,
                     const std::string& nonce = test_nonce(),
                     const std::string& version = "8.0.36-fake") {
    std::string payload(1, '\x0a');
    payload += version;
    payload.push_back('\0');
    payload += le32(42);  // connection id
    payload += nonce.substr(0, 8);
    payload.push_back('\0');
    payload += le16(caps & 0xffff);
    payload.push_back(static_cast<char>(45));
    payload += le16(2);  // status
    payload += le16(caps >> 16);
    payload.push_back(static_cast<char>(21));
    payload += std::string(10, '\0');
    payload += nonce.substr(8, 12);
    payload.push_back('\0');
    payload += plugin;
    payload.push_back('\0');
    return payload;
}

std::string ok_packet() { return std::string("\x00\x00\x00\x02\x00\x00\x00", 7); }
std::string eof_packet() { return std::string("\xfe\x00\x00\x02\x00", 5); }
std::string err_packet(std::uint16_t code, const std::string& state,
                       const std::string& message) {
    return std::string(1, '\xff') + le16(code) + "#" + state + message;
}
std::string column_def(const std::string& name, std::uint8_t type,
                       std::uint16_t charset = 45, std::uint16_t flags = 0) {
    return lenenc("def") + lenenc("shop") + lenenc("t") + lenenc("t") + lenenc(name) +
           lenenc(name) + "\x0c" + le16(charset) + le32(255) +
           std::string(1, static_cast<char>(type)) + le16(flags) + std::string(3, '\0');
}
std::string text_row(const std::vector<std::optional<std::string>>& cells) {
    std::string out;
    for (const auto& cell : cells) out += cell ? lenenc(*cell) : std::string(1, '\xfb');
    return out;
}

// --- Client-side packet readers --------------------------------------------------

struct Packet {
    std::uint8_t sequence = 0;
    std::string payload;
};

std::vector<Packet> unframe(const std::string& bytes) {
    std::vector<Packet> packets;
    std::size_t offset = 0;
    while (offset + 4 <= bytes.size()) {
        const std::size_t length = static_cast<unsigned char>(bytes[offset]) |
                                   (static_cast<unsigned char>(bytes[offset + 1]) << 8) |
                                   (static_cast<unsigned char>(bytes[offset + 2]) << 16);
        packets.push_back({static_cast<std::uint8_t>(bytes[offset + 3]),
                           bytes.substr(offset + 4, length)});
        offset += 4 + length;
    }
    return packets;
}

Packet only_packet(const std::string& bytes) {
    const std::vector<Packet> packets = unframe(bytes);
    EXPECT_EQ(packets.size(), 1u);
    return packets.empty() ? Packet{} : packets.front();
}

std::uint32_t read_le32(const std::string& bytes, std::size_t at) {
    return static_cast<unsigned char>(bytes[at]) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
}

struct HandshakeResponse {
    std::uint32_t caps = 0;
    std::string user;
    std::string auth;
    std::string database;
    std::string plugin;
};

HandshakeResponse parse_handshake_response(const std::string& payload) {
    HandshakeResponse parsed;
    parsed.caps = read_le32(payload, 0);
    std::size_t at = 32;
    const auto until_nul = [&]() {
        const std::size_t end = payload.find('\0', at);
        std::string out = payload.substr(at, end - at);
        at = end == std::string::npos ? payload.size() : end + 1;
        return out;
    };
    parsed.user = until_nul();
    const std::size_t length = static_cast<unsigned char>(payload[at++]);
    parsed.auth = payload.substr(at, length);
    at += length;
    if ((parsed.caps & kConnectWithDb) != 0) parsed.database = until_nul();
    if ((parsed.caps & kPluginAuth) != 0) parsed.plugin = until_nul();
    return parsed;
}

std::string query_text(const Packet& packet) {
    EXPECT_FALSE(packet.payload.empty());
    if (packet.payload.empty()) return {};
    EXPECT_EQ(packet.payload[0], '\x03');  // COM_QUERY
    EXPECT_EQ(packet.sequence, 0);
    return packet.payload.substr(1);
}

MysqlSessionOptions session_options(MysqlTlsMode mode, std::size_t max_rows = 100) {
    MysqlSessionOptions options;
    options.user = "reader";
    options.password = "secret";
    options.database = "shop";
    options.sql = "SELECT * FROM t";
    options.max_rows = max_rows;
    options.tls_mode = mode;
    return options;
}

// Drive a session through the read-only fencing after authentication: answers
// each fencing statement with OK and returns the step that carries the caller's
// query. `step` is the step that followed the auth OK.
MysqlStep through_fencing(MysqlClientSession& session, MysqlStep step,
                          std::size_t max_rows, std::vector<std::string>* sent = nullptr) {
    const std::vector<std::string> expected = {
        "SET SESSION TRANSACTION READ ONLY",
        "SET SESSION sql_select_limit = " + std::to_string(max_rows),
        "START TRANSACTION READ ONLY"};
    for (const std::string& statement : expected) {
        const std::string text = query_text(only_packet(step.send));
        EXPECT_EQ(text, statement);
        if (sent != nullptr) sent->push_back(text);
        step = session.receive(frame(1, ok_packet()));
    }
    return step;
}

// The product-catalogue result set the protocol and driver tests return.
std::string catalogue_result(std::uint8_t first_sequence, int row_count = 2) {
    std::uint8_t sequence = first_sequence;
    std::string out = frame(sequence++, std::string(1, '\x08'));  // 8 columns
    out += frame(sequence++, column_def("id", 0x03));
    out += frame(sequence++, column_def("price", 0xf6));
    out += frame(sequence++, column_def("name", 0xfd));
    out += frame(sequence++, column_def("note", 0xfd));
    out += frame(sequence++, column_def("big", 0x08, 63, 0x20));  // BIGINT UNSIGNED
    out += frame(sequence++, column_def("raw", 0xfc, 63));        // BLOB
    out += frame(sequence++, column_def("created", 0x0c, 63));    // DATETIME
    out += frame(sequence++, column_def("flag", 0x10, 63));       // BIT(1)
    out += frame(sequence++, eof_packet());
    for (int row = 0; row < row_count; ++row) {
        out += frame(sequence++,
                     text_row({std::to_string(row + 1), "9.50", "Ad\xc3\xa1", std::nullopt,
                               "18446744073709551615", std::string("\x01\x02", 2),
                               "2026-10-21 09:30:00", std::string("\x01", 1)}));
    }
    out += frame(sequence++, eof_packet());
    return out;
}

ConnectorConfig mysql_config() {
    ConnectorConfig config;
    config.type = "mysql";
    config.host = "127.0.0.1";
    config.user = "reader";
    config.token = "secret";
    config.database = "shop";
    return config;
}

}  // namespace

// --- 1. The boundary ---------------------------------------------------------------

TEST(MysqlConnectorConfig, RequiresHostAndUserAndValidatesTheTransport) {
    using zetta::connect::ConnectorConfig;
    EXPECT_TRUE(ConnectorConfig::from_json(
        nlohmann::json{{"type", "mysql"}, {"host", "db.example.com"}, {"user", "reader"}}));
    EXPECT_FALSE(ConnectorConfig::from_json(nlohmann::json{{"type", "mysql"}, {"host", "h"}}));
    EXPECT_FALSE(ConnectorConfig::from_json(nlohmann::json{{"type", "mysql"}, {"user", "u"}}));

    const auto base = nlohmann::json{{"type", "mysql"}, {"host", "h"}, {"user", "u"}};
    for (const char* mode : {"disable", "require", "verify-ca", "verify-full", "REQUIRED",
                             "VERIFY_IDENTITY", "DISABLED"}) {
        nlohmann::json config = base;
        config["sslmode"] = mode;
        EXPECT_TRUE(ConnectorConfig::from_json(config)) << mode;
    }
    // `prefer` would fall back to plaintext without saying so; it is not a mode.
    for (const char* mode : {"prefer", "allow", "maybe"}) {
        nlohmann::json config = base;
        config["sslmode"] = mode;
        EXPECT_FALSE(ConnectorConfig::from_json(config)) << mode;
    }
    for (const char* port : {"0", "65536", "33o6", "-1"}) {
        nlohmann::json config = base;
        config["port"] = port;
        EXPECT_FALSE(ConnectorConfig::from_json(config)) << port;
    }
    nlohmann::json with_port = base;
    with_port["port"] = "3307";
    EXPECT_TRUE(ConnectorConfig::from_json(with_port));
}

TEST(MysqlConnectorConfig, ThePasswordIsNeverSerialized) {
    const auto config = zetta::connect::ConnectorConfig::from_json(nlohmann::json{
        {"type", "mysql"}, {"host", "h"}, {"user", "u"}, {"token", "hunter2"}});
    ASSERT_TRUE(config);
    EXPECT_EQ(config->token, "hunter2");
    EXPECT_EQ(config->to_json().dump().find("hunter2"), std::string::npos);
}

TEST(MysqlConnectorConfig, MysqlIsAKnownTypeInTheSchema) {
    const auto& types = zetta::connect::connector_types();
    EXPECT_NE(std::find(types.begin(), types.end(), "mysql"), types.end());
    const auto& schema = zetta::connect::connector_config_schema();
    const auto& allowed = schema.at("properties").at("type").at("enum");
    EXPECT_NE(std::find(allowed.begin(), allowed.end(), "mysql"), allowed.end());
}

TEST(MysqlTlsMode, DefaultsToVerifyFull) {
    using zetta::connect::parse_mysql_tls_mode;
    EXPECT_EQ(parse_mysql_tls_mode(""), MysqlTlsMode::VerifyFull);
    EXPECT_EQ(parse_mysql_tls_mode("verify_identity"), MysqlTlsMode::VerifyFull);
    EXPECT_EQ(parse_mysql_tls_mode("Verify-CA"), MysqlTlsMode::VerifyCa);
    EXPECT_EQ(parse_mysql_tls_mode("required"), MysqlTlsMode::Require);
    EXPECT_EQ(parse_mysql_tls_mode("disable"), MysqlTlsMode::Disable);
    EXPECT_FALSE(parse_mysql_tls_mode("preferred"));
}

TEST(MysqlQueryGuard, AdmitsSelectWithAndShow) {
    const ConnectorConfig config = mysql_config();
    using zetta::connect::connector_query_rejection;
    EXPECT_EQ(connector_query_rejection(config, "SELECT * FROM orders"), "");
    EXPECT_EQ(connector_query_rejection(config, "select 1;"), "");
    EXPECT_EQ(connector_query_rejection(config, "WITH x AS (SELECT 1) SELECT * FROM x"), "");
    EXPECT_EQ(connector_query_rejection(config, "SHOW TABLES"), "");
    EXPECT_EQ(connector_query_rejection(config, "# a MySQL comment\nSELECT 1"), "");
    EXPECT_EQ(connector_query_rejection(config, "/* plain */ SELECT 1"), "");
    // An optimizer hint is a comment the server reads but does not execute.
    EXPECT_EQ(connector_query_rejection(config, "SELECT /*+ MAX_EXECUTION_TIME(1000) */ 1"), "");
}

TEST(MysqlQueryGuard, RefusesWritesAndTheServerSideEscapes) {
    const ConnectorConfig config = mysql_config();
    using zetta::connect::connector_query_rejection;
    for (const char* sql : {
             "DELETE FROM orders", "UPDATE orders SET paid = 1", "DROP TABLE orders",
             "INSERT INTO t VALUES (1)", "CALL proc()", "LOAD DATA LOCAL INFILE 'x' INTO TABLE t",
             "SET GLOBAL read_only = 0", "SELECT 1; DROP TABLE orders",
             "SELECT 1 # ;\n; DELETE FROM t",
             // The server EXECUTES the inside of /*! … */ — refused anywhere.
             "/*!50000 DROP TABLE orders */ SELECT 1", "SELECT /*! 1; DELETE FROM t */ 1",
             // Writes a file on the database server; read-only transactions do not stop it.
             "SELECT * FROM orders INTO OUTFILE '/tmp/orders.csv'",
             "SELECT secret FROM t INTO DUMPFILE '/tmp/x'", "", "   -- only a comment"}) {
        EXPECT_NE(connector_query_rejection(config, sql), "") << sql;
    }
}

TEST(MysqlQueryGuard, ShowIsAMysqlOnlyAdmission) {
    // The shared guard (Snowflake, Postgres) is unchanged.
    ConnectorConfig postgres = mysql_config();
    postgres.type = "postgres";
    EXPECT_NE(zetta::connect::connector_query_rejection(postgres, "SHOW TABLES"), "");
}

TEST(MysqlConnector, RefusesAWriteBeforeAnyConnection) {
    // No runner involvement: the boundary refuses it.
    const auto result =
        zetta::connect::run_connector_query(mysql_config(), "DELETE FROM orders", 10);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("read-only"), std::string::npos) << result.error;
}

// --- 2. The protocol ----------------------------------------------------------------

TEST(MysqlProtocol, PasswordPluginResponsesMatchReferenceVectors) {
    // Computed independently (Python hashlib) for password "secret" and the
    // scramble bytes 0x01..0x14.
    const auto hex = [](const std::string& bytes) {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out;
        for (const char byte : bytes) {
            out.push_back(kDigits[static_cast<unsigned char>(byte) >> 4]);
            out.push_back(kDigits[static_cast<unsigned char>(byte) & 0x0f]);
        }
        return out;
    };
    EXPECT_EQ(hex(zetta::connect::mysql_native_password_response("secret", test_nonce())),
              "b32bb3a583e1340c0a1108d58b1be49781ad8c2f");
    EXPECT_EQ(hex(zetta::connect::caching_sha2_password_response("secret", test_nonce())),
              "746ebe205d56a0707acb3e796e834e0dd7b1d61743b26bd5202c7a623230c7c9");
    // An empty password answers with an empty response.
    EXPECT_EQ(zetta::connect::mysql_native_password_response("", test_nonce()), "");
    EXPECT_EQ(zetta::connect::caching_sha2_password_response("", test_nonce()), "");
}

TEST(MysqlProtocol, PlaintextSessionFencesReadOnlyAndTypesTheCells) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));

    // Greeting → HandshakeResponse41 at sequence 1.
    MysqlStep step = session.receive(frame(0, greeting(kServerCaps, "mysql_native_password")));
    ASSERT_EQ(step.action, MysqlStep::Action::Read);
    const Packet response = only_packet(step.send);
    EXPECT_EQ(response.sequence, 1);
    const HandshakeResponse handshake = parse_handshake_response(response.payload);
    EXPECT_EQ(handshake.user, "reader");
    EXPECT_EQ(handshake.database, "shop");
    EXPECT_EQ(handshake.plugin, "mysql_native_password");
    EXPECT_EQ(handshake.auth,
              zetta::connect::mysql_native_password_response("secret", test_nonce()));
    // Never taken, though the server offers them.
    EXPECT_EQ(handshake.caps & kMultiStatements, 0u);
    EXPECT_EQ(handshake.caps & kLocalFiles, 0u);
    EXPECT_EQ(handshake.caps & kSsl, 0u);
    EXPECT_NE(handshake.caps & kProtocol41, 0u);
    EXPECT_EQ(step.send.find("secret"), std::string::npos);

    // Auth OK → the fencing, in order, then the caller's statement.
    step = session.receive(frame(2, ok_packet()));
    std::vector<std::string> sent;
    step = through_fencing(session, step, 100, &sent);
    EXPECT_EQ(query_text(only_packet(step.send)), "SELECT * FROM t");

    // The result set, delivered one byte at a time (framing must not care).
    const std::string result = catalogue_result(1);
    for (std::size_t index = 0; index + 1 < result.size(); ++index) {
        step = session.receive(result.substr(index, 1));
        ASSERT_EQ(step.action, MysqlStep::Action::Read);
        ASSERT_TRUE(step.send.empty());
    }
    step = session.receive(result.substr(result.size() - 1));
    // The final EOF → ROLLBACK, never COMMIT.
    EXPECT_EQ(query_text(only_packet(step.send)), "ROLLBACK");
    step = session.receive(frame(1, ok_packet()));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    const Packet quit = only_packet(step.send);
    EXPECT_EQ(quit.payload, std::string(1, '\x01'));  // COM_QUIT

    ASSERT_TRUE(session.finished());
    const auto& outcome = session.result();
    ASSERT_TRUE(outcome.ok()) << outcome.error;
    EXPECT_EQ(outcome.data.columns,
              (std::vector<std::string>{"id", "price", "name", "note", "big", "raw",
                                        "created", "flag"}));
    ASSERT_EQ(outcome.data.rows.size(), 2u);
    const nlohmann::json& row = outcome.data.rows[0];
    EXPECT_TRUE(row.at("id").is_number_integer());
    EXPECT_EQ(row.at("id").get<long long>(), 1);
    EXPECT_DOUBLE_EQ(row.at("price").get<double>(), 9.5);
    EXPECT_EQ(row.at("name").get<std::string>(), "Ad\xc3\xa1");
    EXPECT_TRUE(row.at("note").is_null());
    EXPECT_TRUE(row.at("big").is_number_unsigned());
    EXPECT_EQ(row.at("big").get<std::uint64_t>(), 18446744073709551615ull);
    EXPECT_EQ(row.at("raw").get<std::string>(), "0x0102");
    EXPECT_EQ(row.at("created").get<std::string>(), "2026-10-21 09:30:00");
    EXPECT_EQ(row.at("flag").get<long long>(), 1);
    EXPECT_FALSE(session.tls_active());
    EXPECT_EQ(session.server_version(), "8.0.36-fake");
}

TEST(MysqlProtocol, TlsIsNegotiatedBeforeAnyCredential) {
    MysqlClientSession session(session_options(MysqlTlsMode::VerifyFull));
    MysqlStep step =
        session.receive(frame(0, greeting(kServerCaps | kSsl, "caching_sha2_password")));
    ASSERT_EQ(step.action, MysqlStep::Action::StartTls);
    // The SSLRequest: 32 bytes, sequence 1, CLIENT_SSL set, no user, no password.
    const Packet request = only_packet(step.send);
    EXPECT_EQ(request.sequence, 1);
    EXPECT_EQ(request.payload.size(), 32u);
    EXPECT_NE(read_le32(request.payload, 0) & kSsl, 0u);
    EXPECT_EQ(step.send.find("reader"), std::string::npos);

    // After the handshake: the response at sequence 2, caching_sha2 scramble.
    step = session.tls_established();
    const Packet response = only_packet(step.send);
    EXPECT_EQ(response.sequence, 2);
    const HandshakeResponse handshake = parse_handshake_response(response.payload);
    EXPECT_EQ(handshake.plugin, "caching_sha2_password");
    EXPECT_EQ(handshake.auth,
              zetta::connect::caching_sha2_password_response("secret", test_nonce()));

    // Full authentication: over TLS the password itself is sent.
    step = session.receive(frame(3, std::string("\x01\x04", 2)));
    EXPECT_EQ(only_packet(step.send).payload, std::string("secret\0", 7));
    step = session.receive(frame(5, ok_packet()));
    step = through_fencing(session, step, 100);
    EXPECT_EQ(query_text(only_packet(step.send)), "SELECT * FROM t");
    EXPECT_TRUE(session.tls_active());
}

TEST(MysqlProtocol, CachingSha2FastAuthNeedsNoPassword) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    MysqlStep step = session.receive(frame(0, greeting(kServerCaps, "caching_sha2_password")));
    // Fast-auth success, then OK.
    step = session.receive(frame(2, std::string("\x01\x03", 2)));
    EXPECT_EQ(step.action, MysqlStep::Action::Read);
    EXPECT_TRUE(step.send.empty());
    step = session.receive(frame(3, ok_packet()));
    EXPECT_EQ(query_text(only_packet(step.send)), "SET SESSION TRANSACTION READ ONLY");
}

TEST(MysqlProtocol, AServerWithoutTlsIsRefusedBeforeTheCredentials) {
    for (const MysqlTlsMode mode :
         {MysqlTlsMode::Require, MysqlTlsMode::VerifyCa, MysqlTlsMode::VerifyFull}) {
        MysqlClientSession session(session_options(mode));
        const MysqlStep step =
            session.receive(frame(0, greeting(kServerCaps, "mysql_native_password")));
        EXPECT_EQ(step.action, MysqlStep::Action::Finished);
        EXPECT_TRUE(step.send.empty());  // nothing — not the user, not a scramble
        EXPECT_NE(session.result().error.find("does not offer TLS"), std::string::npos)
            << session.result().error;
    }
}

TEST(MysqlProtocol, FullAuthenticationInPlaintextIsRefused) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    session.receive(frame(0, greeting(kServerCaps, "caching_sha2_password")));
    const MysqlStep step = session.receive(frame(2, std::string("\x01\x04", 2)));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    EXPECT_EQ(step.send.find("secret"), std::string::npos);
    EXPECT_NE(session.result().error.find("only done over TLS"), std::string::npos);
}

TEST(MysqlProtocol, FollowsAnAuthSwitch) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    session.receive(frame(0, greeting(kServerCaps, "caching_sha2_password")));
    std::string fresh_nonce(20, 'n');
    std::string switch_request = std::string(1, '\xfe') + "mysql_native_password" +
                                 std::string(1, '\0') + fresh_nonce + std::string(1, '\0');
    MysqlStep step = session.receive(frame(2, switch_request));
    const Packet answer = only_packet(step.send);
    EXPECT_EQ(answer.sequence, 3);
    EXPECT_EQ(answer.payload,
              zetta::connect::mysql_native_password_response("secret", fresh_nonce));
    step = session.receive(frame(4, ok_packet()));
    EXPECT_EQ(query_text(only_packet(step.send)), "SET SESSION TRANSACTION READ ONLY");
}

TEST(MysqlProtocol, ClearTextPluginsAreRefusedWithoutTls) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    session.receive(frame(0, greeting(kServerCaps, "mysql_native_password")));
    const std::string switch_request =
        std::string(1, '\xfe') + "mysql_clear_password" + std::string(1, '\0');
    const MysqlStep step = session.receive(frame(2, switch_request));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    EXPECT_EQ(step.send.find("secret"), std::string::npos);
}

TEST(MysqlProtocol, AnAuthenticationErrorNamesTheUserNotThePassword) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    session.receive(frame(0, greeting(kServerCaps, "mysql_native_password")));
    session.receive(frame(2, err_packet(1045, "28000",
                                        "Access denied for user 'reader'@'10.0.0.5'")));
    ASSERT_TRUE(session.finished());
    const std::string& error = session.result().error;
    EXPECT_NE(error.find("authentication failed"), std::string::npos) << error;
    EXPECT_NE(error.find("1045"), std::string::npos) << error;
    EXPECT_EQ(error.find("secret"), std::string::npos) << error;
}

TEST(MysqlProtocol, AWriteRefusedByTheReadOnlyTransactionSurfaces) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    session.receive(frame(0, greeting(kServerCaps, "mysql_native_password")));
    MysqlStep step = session.receive(frame(2, ok_packet()));
    step = through_fencing(session, step, 100);
    step = session.receive(frame(
        1, err_packet(1792, "25006", "Cannot execute statement in a READ ONLY transaction.")));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    EXPECT_NE(session.result().error.find("1792"), std::string::npos)
        << session.result().error;
    EXPECT_TRUE(session.result().data.rows.empty());
}

TEST(MysqlProtocol, StopsReadingAtTheRowCap) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable, /*max_rows=*/2));
    session.receive(frame(0, greeting(kServerCaps, "mysql_native_password")));
    MysqlStep step = session.receive(frame(2, ok_packet()));
    step = through_fencing(session, step, 2);
    step = session.receive(catalogue_result(1, /*row_count=*/5));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    ASSERT_TRUE(session.result().ok()) << session.result().error;
    EXPECT_EQ(session.result().data.rows.size(), 2u);
}

TEST(MysqlProtocol, RefusesALocalInfileRequest) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    session.receive(frame(0, greeting(kServerCaps, "mysql_native_password")));
    MysqlStep step = session.receive(frame(2, ok_packet()));
    step = through_fencing(session, step, 100);
    step = session.receive(frame(1, std::string("\xfb/etc/passwd", 12)));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    EXPECT_NE(session.result().error.find("local file"), std::string::npos);
}

TEST(MysqlProtocol, TruncatedGreetingIsAProtocolError) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    const MysqlStep step = session.receive(frame(0, std::string("\x0a" "8.0", 4)));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    EXPECT_FALSE(session.result().ok());
}

// --- 3. The driver, over real loopback sockets --------------------------------------

namespace {

// A fake MySQL server on a helper thread: one connection, the handshake
// (optionally upgraded to TLS with real OpenSSL), mysql_native_password, OK for
// every fencing statement, the catalogue result set for the query, OK for the
// ROLLBACK. It records what the client sent, for the assertions.
class FakeMysqlServer {
public:
    struct Options {
        bool offer_tls = false;
        std::string certificate_pem;
        std::string private_key_pem;
        std::string password = "secret";
        std::string version = "8.0.36-fake";
        // Answer READ ONLY statements with TiDB's error, as a TiDB with
        // tidb_enable_noop_functions OFF (its default) does.
        bool refuse_read_only = false;
    };

    explicit FakeMysqlServer(Options options) : options_(std::move(options)) {
#ifndef SO_NOSIGPIPE
        std::signal(SIGPIPE, SIG_IGN);  // a client closing mid-result must not kill the test
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 1);
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeMysqlServer() {
        if (thread_.joinable()) thread_.join();
        if (listener_ >= 0) ::close(listener_);
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }
    bool tls_used() const { return tls_used_; }
    const std::vector<std::string>& queries() const { return queries_; }
    const std::string& user() const { return user_; }
    const std::string& failure() const { return failure_; }

private:
    bool read_exact(std::string& out, std::size_t count) {
        out.resize(count);
        std::size_t got = 0;
        while (got < count) {
            const long read = ssl_ != nullptr
                                  ? SSL_read(ssl_, out.data() + got, static_cast<int>(count - got))
                                  : ::recv(connection_, out.data() + got, count - got, 0);
            if (read <= 0) return false;
            got += static_cast<std::size_t>(read);
        }
        return true;
    }

    bool write_all(const std::string& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const long wrote =
                ssl_ != nullptr
                    ? SSL_write(ssl_, bytes.data() + sent, static_cast<int>(bytes.size() - sent))
                    : ::send(connection_, bytes.data() + sent, bytes.size() - sent, 0);
            if (wrote <= 0) return false;
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    bool read_packet(Packet& packet) {
        std::string header;
        if (!read_exact(header, 4)) return false;
        const std::size_t length = static_cast<unsigned char>(header[0]) |
                                   (static_cast<unsigned char>(header[1]) << 8) |
                                   (static_cast<unsigned char>(header[2]) << 16);
        packet.sequence = static_cast<std::uint8_t>(header[3]);
        return read_exact(packet.payload, length);
    }

    bool start_tls() {
        context_ = SSL_CTX_new(TLS_server_method());
        BIO* certificate_bio = BIO_new_mem_buf(options_.certificate_pem.data(),
                                               static_cast<int>(options_.certificate_pem.size()));
        X509* certificate = PEM_read_bio_X509(certificate_bio, nullptr, nullptr, nullptr);
        BIO_free(certificate_bio);
        BIO* key_bio = BIO_new_mem_buf(options_.private_key_pem.data(),
                                       static_cast<int>(options_.private_key_pem.size()));
        EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        const bool loaded = certificate != nullptr && key != nullptr &&
                            SSL_CTX_use_certificate(context_, certificate) == 1 &&
                            SSL_CTX_use_PrivateKey(context_, key) == 1;
        X509_free(certificate);
        EVP_PKEY_free(key);
        if (!loaded) return false;
        ssl_ = SSL_new(context_);
        SSL_set_fd(ssl_, connection_);
        if (SSL_accept(ssl_) != 1) {
            SSL_free(ssl_);
            ssl_ = nullptr;
            return false;
        }
        tls_used_ = true;
        return true;
    }

    void serve() {
        pollfd waiting{listener_, POLLIN, 0};
        if (::poll(&waiting, 1, 20000) <= 0) {
            failure_ = "no client connected";
            return;
        }
        connection_ = ::accept(listener_, nullptr, nullptr);
        if (connection_ < 0) {
            failure_ = "accept failed";
            return;
        }
#ifdef SO_NOSIGPIPE
        int on = 1;
        ::setsockopt(connection_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        timeval limit{20, 0};
        ::setsockopt(connection_, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        converse();
        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (context_ != nullptr) SSL_CTX_free(context_);
        ::close(connection_);
    }

    void converse() {
        const std::uint32_t caps = kServerCaps | (options_.offer_tls ? kSsl : 0);
        if (!write_all(frame(0, greeting(caps, "mysql_native_password", test_nonce(),
                                         options_.version)))) {
            return;
        }
        Packet packet;
        if (!read_packet(packet)) return;
        std::uint8_t sequence = packet.sequence;
        if (packet.payload.size() == 32 && (read_le32(packet.payload, 0) & kSsl) != 0) {
            if (!start_tls()) {
                failure_ = "TLS accept failed";
                return;
            }
            if (!read_packet(packet)) return;
            sequence = packet.sequence;
        }
        const HandshakeResponse handshake = parse_handshake_response(packet.payload);
        user_ = handshake.user;
        if (handshake.auth != zetta::connect::mysql_native_password_response(
                                  options_.password, test_nonce())) {
            write_all(frame(sequence + 1,
                            err_packet(1045, "28000", "Access denied for user")));
            return;
        }
        if (!write_all(frame(sequence + 1, ok_packet()))) return;

        for (;;) {
            if (!read_packet(packet) || packet.payload.empty()) return;
            if (packet.payload[0] == '\x01') return;  // COM_QUIT
            const std::string text = packet.payload.substr(1);
            queries_.push_back(text);
            if (options_.refuse_read_only && text.find("READ ONLY") != std::string::npos) {
                if (!write_all(frame(1, err_packet(1105, "HY000",
                                                   "function READ ONLY has only noop "
                                                   "implementation in tidb now")))) {
                    return;
                }
                continue;
            }
            const bool is_query = text.starts_with("SELECT");
            if (!write_all(is_query ? catalogue_result(1, 3) : frame(1, ok_packet()))) return;
        }
    }

    Options options_;
    int listener_ = -1;
    int connection_ = -1;
    int port_ = 0;
    SSL_CTX* context_ = nullptr;
    SSL* ssl_ = nullptr;
    bool tls_used_ = false;
    std::vector<std::string> queries_;
    std::string user_;
    std::string failure_;
    std::thread thread_;
};

// Loopback is a private destination (#1414): each driver test approves the
// exact host:port its fake server listens on, and the fixture clears it.
ConnectorConfig loopback_config(int port, const std::string& sslmode) {
    ConnectorConfig config = mysql_config();
    config.port = std::to_string(port);
    config.sslmode = sslmode;
    const auto approved = zetta::connect::ConnectorDestinations::parse(
        config.host + ":" + config.port + ",localhost:" + config.port);
    if (approved) zetta::connect::set_connector_destinations(*approved);
    return config;
}

struct LoopbackDriverTest : ::testing::Test {
    void TearDown() override { zetta::connect::set_connector_destinations({}); }
};
struct MysqlDriver : LoopbackDriverTest {};
struct TidbDriver : LoopbackDriverTest {};

// A server certificate signed by a throwaway CA, minted with the cluster CA code
// (Ed25519, TLS 1.3). Its name is the node id, so it does NOT match 127.0.0.1.
zetta::cluster::ClusterSecurity mint_server_identity() {
    std::string reason;
    const auto security = zetta::cluster::derive_cluster_security(
        zetta::cluster::generate_join_token(), "fake-mysql", &reason);
    EXPECT_TRUE(security) << reason;
    return security.value_or(zetta::cluster::ClusterSecurity{});
}

// Point the trust store at `pem` for the life of the object.
class ScopedTrustStore {
public:
    ScopedTrustStore(const std::filesystem::path& file, const std::string& pem) {
        std::ofstream(file) << pem;
        if (const char* existing = std::getenv("SSL_CERT_FILE")) previous_ = existing;
        ::setenv("SSL_CERT_FILE", file.c_str(), 1);
    }
    ~ScopedTrustStore() {
        if (previous_) {
            ::setenv("SSL_CERT_FILE", previous_->c_str(), 1);
        } else {
            ::unsetenv("SSL_CERT_FILE");
        }
    }

private:
    std::optional<std::string> previous_;
};

}  // namespace

TEST_F(MysqlDriver, RunsAQueryOverLoopbackInPlaintextWhenAskedByName) {
    zetta::connect::install_mysql_protocol();
    FakeMysqlServer server({});
    const auto result = zetta::connect::run_connector_query(
        loopback_config(server.port(), "disable"), "SELECT * FROM t", 10);
    server.join();
    ASSERT_TRUE(result.ok()) << result.error << " / server: " << server.failure();
    EXPECT_EQ(result.data.rows.size(), 3u);
    EXPECT_EQ(result.data.rows[2].at("id").get<long long>(), 3);
    EXPECT_FALSE(server.tls_used());
    EXPECT_EQ(server.user(), "reader");
    EXPECT_EQ(server.queries(),
              (std::vector<std::string>{"SET SESSION TRANSACTION READ ONLY",
                                        "SET SESSION sql_select_limit = 10",
                                        "START TRANSACTION READ ONLY", "SELECT * FROM t",
                                        "ROLLBACK"}));
}

TEST_F(MysqlDriver, TheDefaultRefusesAPlaintextServer) {
    zetta::connect::install_mysql_protocol();
    FakeMysqlServer server({});
    const auto result = zetta::connect::run_connector_query(
        loopback_config(server.port(), ""), "SELECT * FROM t", 10);
    server.join();
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("does not offer TLS"), std::string::npos) << result.error;
    EXPECT_TRUE(server.user().empty());  // no handshake response was ever sent
}

TEST_F(MysqlDriver, RunsAQueryOverRealTls) {
    zetta::connect::install_mysql_protocol();
    const auto identity = mint_server_identity();
    FakeMysqlServer server({.offer_tls = true,
                            .certificate_pem = identity.certificate_pem,
                            .private_key_pem = identity.private_key_pem});
    // `require`: encrypted, the self-signed chain not verified.
    const auto result = zetta::connect::run_connector_query(
        loopback_config(server.port(), "require"), "SELECT * FROM t", 2);
    server.join();
    ASSERT_TRUE(result.ok()) << result.error << " / server: " << server.failure();
    EXPECT_TRUE(server.tls_used());
    EXPECT_EQ(result.data.rows.size(), 2u);  // the client-side cap held
    EXPECT_EQ(result.data.rows[0].at("name").get<std::string>(), "Ad\xc3\xa1");
}

TEST_F(MysqlDriver, VerifyCaTrustsOnlyTheConfiguredRoots) {
    zetta::connect::install_mysql_protocol();
    zetta::test::TempDir dir;
    const auto identity = mint_server_identity();
    {
        // The server's CA is in the trust store: the chain verifies.
        ScopedTrustStore trust(dir.path() / "ca.pem", identity.ca_certificate_pem);
        FakeMysqlServer server({.offer_tls = true,
                                .certificate_pem = identity.certificate_pem,
                                .private_key_pem = identity.private_key_pem});
        const auto result = zetta::connect::run_connector_query(
            loopback_config(server.port(), "verify-ca"), "SELECT * FROM t", 10);
        server.join();
        EXPECT_TRUE(result.ok()) << result.error << " / server: " << server.failure();
    }
    {
        // A different CA: the handshake fails, and no credential was sent.
        const auto stranger = mint_server_identity();
        ScopedTrustStore trust(dir.path() / "other.pem", stranger.ca_certificate_pem);
        FakeMysqlServer server({.offer_tls = true,
                                .certificate_pem = identity.certificate_pem,
                                .private_key_pem = identity.private_key_pem});
        const auto result = zetta::connect::run_connector_query(
            loopback_config(server.port(), "verify-ca"), "SELECT * FROM t", 10);
        server.join();
        EXPECT_FALSE(result.ok());
        EXPECT_NE(result.error.find("TLS handshake"), std::string::npos) << result.error;
        EXPECT_TRUE(server.user().empty());
    }
}

TEST_F(MysqlDriver, VerifyFullRejectsACertificateForAnotherName) {
    zetta::connect::install_mysql_protocol();
    zetta::test::TempDir dir;
    const auto identity = mint_server_identity();
    ScopedTrustStore trust(dir.path() / "ca.pem", identity.ca_certificate_pem);
    FakeMysqlServer server({.offer_tls = true,
                            .certificate_pem = identity.certificate_pem,
                            .private_key_pem = identity.private_key_pem});
    // The chain is trusted, but the certificate is for "fake-mysql", not 127.0.0.1.
    const auto result = zetta::connect::run_connector_query(
        loopback_config(server.port(), "verify-full"), "SELECT * FROM t", 10);
    server.join();
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("TLS handshake"), std::string::npos) << result.error;
    EXPECT_TRUE(server.user().empty());
}

TEST_F(MysqlDriver, AWrongPasswordFailsWithoutEchoingIt) {
    zetta::connect::install_mysql_protocol();
    FakeMysqlServer server({.password = "the-real-one"});
    const auto result = zetta::connect::run_connector_query(
        loopback_config(server.port(), "disable"), "SELECT * FROM t", 10);
    server.join();
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("authentication failed"), std::string::npos) << result.error;
    EXPECT_EQ(result.error.find("secret"), std::string::npos);
    EXPECT_EQ(result.error.find("the-real-one"), std::string::npos);
}

TEST_F(MysqlDriver, AnUnreachableServerFailsCleanly) {
    zetta::connect::install_mysql_protocol();
    // Bind and close a port so nothing listens on it.
    int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);
    ::close(probe);
    const auto result = zetta::connect::run_connector_query(
        loopback_config(port, "disable"), "SELECT 1", 1);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("could not connect"), std::string::npos) << result.error;
}

// --- TiDB (#1014): the MySQL protocol, fenced the way TiDB allows ----------------------
//
// TiDB parses READ ONLY but does not enforce it: with tidb_enable_noop_functions
// OFF (its default) `SET SESSION TRANSACTION READ ONLY` and `START TRANSACTION
// READ ONLY` are errors, and ON they are silently ignored. So a greeting that
// names TiDB switches the session to the write-word refusal plus a plain
// transaction that is rolled back.

constexpr const char* kTidbVersion = "8.0.11-TiDB-v7.5.1";

TEST(TidbProtocol, RecognisesTidbVersions) {
    EXPECT_TRUE(zetta::connect::is_tidb_version("8.0.11-TiDB-v7.5.1"));
    EXPECT_TRUE(zetta::connect::is_tidb_version("5.7.25-TiDB-v6.5.0"));
    EXPECT_FALSE(zetta::connect::is_tidb_version("8.0.36"));
    EXPECT_FALSE(zetta::connect::is_tidb_version("10.11.6-MariaDB"));
}

TEST(TidbProtocol, AGreetingNamingTidbFencesWithAPlainRolledBackTransaction) {
    MysqlClientSession session(session_options(MysqlTlsMode::Disable));
    MysqlStep step = session.receive(
        frame(0, greeting(kServerCaps, "mysql_native_password", test_nonce(), kTidbVersion)));
    ASSERT_EQ(step.action, MysqlStep::Action::Read);
    EXPECT_TRUE(session.tidb());

    step = session.receive(frame(2, ok_packet()));
    for (const std::string& statement :
         {std::string("SET SESSION sql_select_limit = 100"), std::string("START TRANSACTION")}) {
        EXPECT_EQ(query_text(only_packet(step.send)), statement);
        step = session.receive(frame(1, ok_packet()));
    }
    EXPECT_EQ(query_text(only_packet(step.send)), "SELECT * FROM t");
    step = session.receive(catalogue_result(1));
    EXPECT_EQ(query_text(only_packet(step.send)), "ROLLBACK");
    step = session.receive(frame(1, ok_packet()));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    ASSERT_TRUE(session.result().ok()) << session.result().error;
    EXPECT_EQ(session.result().data.rows.size(), 2u);
}

TEST(TidbProtocol, AWriteWordIsRefusedBeforeAnyCredentialIsSent) {
    // A `mysql` connection admits `WITH … DELETE` at the guard (MySQL's READ
    // ONLY transaction refuses it); when the server turns out to be TiDB, the
    // session refuses it itself, before the handshake response.
    MysqlSessionOptions options = session_options(MysqlTlsMode::Disable);
    options.sql = "WITH x AS (SELECT 1) DELETE FROM t";
    MysqlClientSession session(options);
    const MysqlStep step = session.receive(
        frame(0, greeting(kServerCaps, "mysql_native_password", test_nonce(), kTidbVersion)));
    EXPECT_EQ(step.action, MysqlStep::Action::Finished);
    EXPECT_TRUE(step.send.empty());
    ASSERT_FALSE(session.result().ok());
    EXPECT_NE(session.result().error.find("'delete'"), std::string::npos)
        << session.result().error;
}

TEST(TidbConnectorConfig, ParsesLikeMysqlAndIsAKnownType) {
    const auto config = ConnectorConfig::from_json(
        {{"type", "tidb"}, {"host", "orders-tidb.ns.svc.cluster.local"}, {"port", "4000"},
         {"user", "root"}, {"token", "pw"}, {"sslmode", "disable"}});
    ASSERT_TRUE(config.has_value());
    EXPECT_FALSE(config->to_json().contains("token"));
    EXPECT_FALSE(ConnectorConfig::from_json({{"type", "tidb"}, {"user", "root"}}));
    EXPECT_FALSE(ConnectorConfig::from_json(
        {{"type", "tidb"}, {"host", "h"}, {"user", "u"}, {"sslmode", "prefer"}}));
    const auto& types = zetta::connect::connector_types();
    EXPECT_NE(std::find(types.begin(), types.end(), "tidb"), types.end());
}

TEST(TidbQueryGuard, AdmitsReadsAndRefusesWritesAnywhere) {
    ConnectorConfig config = mysql_config();
    config.type = "tidb";
    for (const char* sql : {"SELECT * FROM orders", "SHOW TABLES",
                            "WITH r AS (SELECT 1) SELECT * FROM r",
                            "SELECT note FROM t WHERE note = 'please delete'"}) {
        EXPECT_EQ(zetta::connect::connector_query_rejection(config, sql), "") << sql;
    }
    for (const char* sql :
         {"WITH x AS (SELECT 1) DELETE FROM t", "WITH x AS (SELECT 1) UPDATE t SET a = 1",
          "SELECT * FROM t FOR UPDATE", "SELECT 'a\\'' , 1 FROM t WHERE (DELETE)",
          "WITH x AS (SELECT 1 --1 ) DELETE FROM t", "SELECT /*! 1 */ 2",
          "SELECT * INTO OUTFILE '/tmp/x' FROM t"}) {
        EXPECT_NE(zetta::connect::connector_query_rejection(config, sql), "") << sql;
    }
}

TEST_F(TidbDriver, RunsAgainstATidbOverLoopbackWhicheverTypeNamedIt) {
    zetta::connect::install_mysql_protocol();
    for (const char* type : {"tidb", "mysql"}) {
        FakeMysqlServer::Options options;
        options.version = kTidbVersion;
        options.refuse_read_only = true;
        FakeMysqlServer server(options);
        ConnectorConfig config = loopback_config(server.port(), "disable");
        config.type = type;
        const auto result = zetta::connect::run_connector_query(config, "SELECT * FROM t", 10);
        server.join();
        ASSERT_TRUE(result.ok()) << type << ": " << result.error
                                 << " / server: " << server.failure();
        EXPECT_EQ(result.data.rows.size(), 3u);
        EXPECT_EQ(server.queries(),
                  (std::vector<std::string>{"SET SESSION sql_select_limit = 10",
                                            "START TRANSACTION", "SELECT * FROM t",
                                            "ROLLBACK"}))
            << type;
    }
}

TEST(TidbLive, RunsAgainstARealTidbWhenConfigured) {
    const char* host = std::getenv("ZETTA_TIDB_TEST_HOST");
    if (host == nullptr || host[0] == '\0') {
        GTEST_SKIP() << "set ZETTA_TIDB_TEST_HOST (and _PORT/_USER/_PASSWORD/_DB/_SSLMODE) "
                        "to run against a real TiDB";
    }
    const auto env_or = [](const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return std::string(value != nullptr ? value : fallback);
    };
    zetta::connect::install_mysql_protocol();
    ConnectorConfig config;
    config.type = "tidb";
    config.host = host;
    config.port = env_or("ZETTA_TIDB_TEST_PORT", "4000");
    config.user = env_or("ZETTA_TIDB_TEST_USER", "root");
    config.token = env_or("ZETTA_TIDB_TEST_PASSWORD", "");
    config.database = env_or("ZETTA_TIDB_TEST_DB", "");
    config.sslmode = env_or("ZETTA_TIDB_TEST_SSLMODE", "disable");

    const auto typed = zetta::connect::run_connector_query(
        config, "SELECT 42 AS n, CAST(2.25 AS DOUBLE) AS f, 'hi' AS s, NULL AS absent", 10);
    ASSERT_TRUE(typed.ok()) << typed.error;
    ASSERT_EQ(typed.data.rows.size(), 1u);
    EXPECT_EQ(typed.data.rows[0].at("n").get<long long>(), 42);
    EXPECT_DOUBLE_EQ(typed.data.rows[0].at("f").get<double>(), 2.25);
    EXPECT_EQ(typed.data.rows[0].at("s").get<std::string>(), "hi");
    EXPECT_TRUE(typed.data.rows[0].at("absent").is_null());

    const auto capped = zetta::connect::run_connector_query(
        config, "SELECT table_name FROM information_schema.columns", 3);
    ASSERT_TRUE(capped.ok()) << capped.error;
    EXPECT_LE(capped.data.rows.size(), 3u);

    const auto shown = zetta::connect::run_connector_query(config, "SHOW DATABASES", 100);
    ASSERT_TRUE(shown.ok()) << shown.error;
    EXPECT_FALSE(shown.data.rows.empty());
}

// --- The live server (guarded) ---------------------------------------------------------

TEST(MysqlLive, RunsAgainstARealServerWhenConfigured) {
    const char* host = std::getenv("ZETTA_MYSQL_TEST_HOST");
    if (host == nullptr || host[0] == '\0') {
        GTEST_SKIP() << "set ZETTA_MYSQL_TEST_HOST (and _PORT/_USER/_PASSWORD/_DB/_SSLMODE) "
                        "to run against a real MySQL";
    }
    const auto env_or = [](const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return std::string(value != nullptr ? value : fallback);
    };
    zetta::connect::install_mysql_protocol();
    ConnectorConfig config;
    config.type = "mysql";
    config.host = host;
    config.port = env_or("ZETTA_MYSQL_TEST_PORT", "3306");
    config.user = env_or("ZETTA_MYSQL_TEST_USER", "root");
    config.token = env_or("ZETTA_MYSQL_TEST_PASSWORD", "");
    config.database = env_or("ZETTA_MYSQL_TEST_DB", "");
    config.sslmode = env_or("ZETTA_MYSQL_TEST_SSLMODE", "require");

    const auto typed = zetta::connect::run_connector_query(
        config,
        "SELECT 42 AS n, 3.5 AS d, CAST(2.25 AS DOUBLE) AS f, 'hi' AS s, NULL AS nothing", 10);
    ASSERT_TRUE(typed.ok()) << typed.error;
    ASSERT_EQ(typed.data.rows.size(), 1u);
    EXPECT_EQ(typed.data.rows[0].at("n").get<long long>(), 42);
    EXPECT_DOUBLE_EQ(typed.data.rows[0].at("d").get<double>(), 3.5);
    EXPECT_DOUBLE_EQ(typed.data.rows[0].at("f").get<double>(), 2.25);
    EXPECT_EQ(typed.data.rows[0].at("s").get<std::string>(), "hi");
    EXPECT_TRUE(typed.data.rows[0].at("nothing").is_null());

    // The row cap holds against a real server.
    const auto capped = zetta::connect::run_connector_query(
        config, "SELECT table_name FROM information_schema.columns", 3);
    ASSERT_TRUE(capped.ok()) << capped.error;
    EXPECT_LE(capped.data.rows.size(), 3u);

    // The read-only transaction refuses a write the guard cannot see (a CTE
    // feeding a DELETE is MySQL 8 syntax the guard admits as WITH).
    const auto write = zetta::connect::run_connector_query(
        config, "WITH x AS (SELECT 1) DELETE FROM mysql.user WHERE 1 = 0", 1);
    EXPECT_FALSE(write.ok());

    const auto shown = zetta::connect::run_connector_query(config, "SHOW DATABASES", 100);
    ASSERT_TRUE(shown.ok()) << shown.error;
    EXPECT_FALSE(shown.data.rows.empty());
}
