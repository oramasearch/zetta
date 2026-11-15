// SPDX-License-Identifier: MIT

#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <thread>
#include <vector>

#include "zetta/api/native_http_transport.hpp"
#include "zetta/api/mysql_protocol.hpp"
#include "zetta/api/postgres_protocol.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/connector_destinations.hpp"
#include "zetta/connect/destination_policy.hpp"
#include "zetta/connect/egress_policy.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/connect/mysql_client.hpp"
#include "zetta/connect/postgres_runner.hpp"
#include "zetta/connect/source_catalog.hpp"
#include "zetta/node.hpp"
#include "temp_dir.hpp"

namespace {
using zetta::connect::ConnectorDestinations;

TEST(ConnectorDestinations, OnlyExactApprovedAuthoritiesReachPrivateAddresses) {
    const auto policy =
        ConnectorDestinations::parse("db.workspace.svc.cluster.local:5432, [::1]:5433");
    ASSERT_TRUE(policy);
    EXPECT_TRUE(policy->allows("DB.Workspace.svc.cluster.local.", "05432", "10.0.1.2"));
    EXPECT_FALSE(policy->allows("db.other.svc.cluster.local", "5432", "10.0.1.2"));
    EXPECT_FALSE(policy->allows("db.workspace.svc.cluster.local", "5433", "10.0.1.2"));
    EXPECT_FALSE(policy->allows("db.workspace.svc.cluster.local.evil", "5432", "10.0.1.2"));
    EXPECT_TRUE(policy->allows("0:0:0:0:0:0:0:1", "5433", "::1"));
    EXPECT_TRUE(policy->allows("warehouse.example", "443", "8.8.8.8"));
    EXPECT_FALSE(ConnectorDestinations{}.allows("localhost", "5432", "127.0.0.1"));
    EXPECT_FALSE(ConnectorDestinations{}.allows("db.workspace.svc", "5432", "10.0.1.2"));
    EXPECT_FALSE(policy->allows("warehouse.example", "443", std::string("8.8.8.8\0suffix", 14)));
    for (const char* address :
         {"169.254.169.254", "169.254.170.2", "100.100.100.200", "169.254.1.2", "fe80::1",
          "fd00:ec2::254", "0.0.0.0", "::ffff:169.254.169.254", "64:ff9b::a9fe:a9fe", "ff02::1",
          "64:ff9b:1::a9fe:a9fe", "3fff::1", "100::1"}) {
        EXPECT_FALSE(policy->allows("db.workspace.svc.cluster.local", "5432", address)) << address;
    }
}

TEST(ConnectorDestinations, InvalidOperatorSettingsFailClosed) {
    for (const char* list : {"*", "private", "loopback", "*.svc:5432", "db", "db:0", "db:65536",
                             "db:postgres", "db:5432/path", "user@db:5432", "/tmp:5432", "::1:5432",
                             "good:5432,bad:bad", "db:5432\nhttps://other:443"}) {
        EXPECT_FALSE(ConnectorDestinations::parse(list)) << list;
    }
}

// The listener records TCP handshakes, reads the first protocol bytes and closes,
// so HTTP and Postgres probes need neither a database nor a TLS certificate.
class ProbeListener {
   public:
    ProbeListener() {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (socket_ < 0 ||
            ::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            ::listen(socket_, 8))
            return;
        const int flags = ::fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) return;
        socklen_t length = sizeof(address);
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length)) return;
        port = std::to_string(ntohs(address.sin_port));
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                pollfd descriptor{socket_, POLLIN, 0};
                if (::poll(&descriptor, 1, 20) <= 0 || !(descriptor.revents & POLLIN)) continue;
                const int client = ::accept(socket_, nullptr, nullptr);
                if (client >= 0) {
                    ++accepted;
                    const int client_flags = ::fcntl(client, F_GETFL, 0);
                    if (client_flags >= 0) ::fcntl(client, F_SETFL, client_flags & ~O_NONBLOCK);
                    // Wait for the first protocol bytes before closing, so a
                    // TLS writer does not race an early close and SIGPIPE.
                    timeval timeout{1, 0};
                    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    std::array<char, 4096> bytes{};
                    [[maybe_unused]] const auto received = ::recv(client, bytes.data(), bytes.size(), 0);
                    ::close(client);
                }
            }
        });
    }
    ~ProbeListener() {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        if (socket_ >= 0) ::close(socket_);
    }
    std::string port;
    std::atomic<int> accepted{0};

   private:
    int socket_ = -1;
    std::jthread worker_;
};

struct ConnectorDestinationTransport : ::testing::Test {
    void SetUp() override {
        zetta::connect::set_connector_destinations({});
        // Lake's Zettas run with ZETTA_EGRESS_ALLOW=* (#1321): the connector
        // policy must refuse on its own, whatever general egress allows.
        const auto everything = zetta::connect::EgressPolicy::parse("*");
        ASSERT_TRUE(everything);
        zetta::connect::set_egress_policy(*everything);
        zetta::api::install_native_http_transport();
    }
    void TearDown() override {
        zetta::api::shutdown_native_http_transport();
        zetta::connect::set_connector_destinations({});
        zetta::connect::set_allowed_destinations({});
        zetta::connect::set_egress_policy({});
    }
};

TEST_F(ConnectorDestinationTransport, HttpChecksResolvedNamesIndependentlyOfActionAllowances) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    zetta::connect::AllowedDestinations action;
    action.loopback = true;
    action.metadata = true;
    zetta::connect::set_allowed_destinations(action);
    for (const char* host :
         {"127.0.0.1", "localhost", "127.1", "2130706433", "[::ffff:127.0.0.1]"}) {
        zetta::connect::HttpRequest request;
        request.url = "https://" + std::string(host) + ":" + server.port + "/";
        request.destinations = zetta::connect::DestinationPolicy::Connector;
        request.timeout_seconds = 1;
        const auto response = zetta::connect::default_http_transport()(request);
        EXPECT_TRUE(response.error.starts_with("destination refused:"))
            << host << ": " << response.error;
    }
    EXPECT_EQ(server.accepted.load(), 0);

    const auto allowed = ConnectorDestinations::parse("localhost:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    zetta::connect::HttpRequest request;
    request.url = "https://localhost:" + server.port + "/";
    request.destinations = zetta::connect::DestinationPolicy::Connector;
    request.timeout_seconds = 1;
    const auto response = zetta::connect::default_http_transport()(request);
    EXPECT_FALSE(response.error.starts_with("destination refused:")) << response.error;
    EXPECT_GT(server.accepted.load(), 0);
    request.url = "http://localhost:" + server.port + "/";
    EXPECT_NE(zetta::connect::default_http_transport()(request).error.find("HTTPS"),
              std::string::npos);
}

// --- MySQL and TiDB (#1414): the MySQL protocol driver dials checked addresses --

zetta::connect::ConnectorConfig mysql_config(const std::string& type, const std::string& host,
                                             const std::string& port) {
    zetta::connect::ConnectorConfig config;
    config.type = type;
    config.host = host;
    config.port = port;
    config.user = "reader";
    config.token = "test";
    // Plaintext so the probe needs no certificate; the destination check runs
    // before the protocol either way.
    config.sslmode = "disable";
    return config;
}

TEST_F(ConnectorDestinationTransport, MysqlAndTidbRefuseUnapprovedPrivateTargetsBeforeConnect) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    zetta::connect::install_mysql_protocol();
    for (const char* type : {"mysql", "tidb"}) {
        for (const char* host :
             {"127.0.0.1", "localhost", "127.1", "2130706433", "::ffff:127.0.0.1"}) {
            const auto result = zetta::connect::run_connector_query(
                mysql_config(type, host, server.port), "SELECT 1", 1);
            EXPECT_TRUE(result.error.starts_with("destination refused:"))
                << type << " " << host << ": " << result.error;
        }
    }
    EXPECT_EQ(server.accepted.load(), 0);
}

TEST_F(ConnectorDestinationTransport, MysqlAndTidbConnectToAnExactApprovedAuthorityOnly) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    zetta::connect::install_mysql_protocol();
    const auto allowed = ConnectorDestinations::parse("localhost:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    int expected = 0;
    for (const char* type : {"mysql", "tidb"}) {
        // The approval names localhost, so the same address under another
        // name, or another port, is still refused.
        auto refused = zetta::connect::run_connector_query(
            mysql_config(type, "127.0.0.1", server.port), "SELECT 1", 1);
        EXPECT_TRUE(refused.error.starts_with("destination refused:")) << refused.error;
        EXPECT_EQ(server.accepted.load(), expected) << type;
        const auto result = zetta::connect::run_connector_query(
            mysql_config(type, "localhost", server.port), "SELECT 1", 1);
        EXPECT_FALSE(result.error.starts_with("destination refused:")) << result.error;
        EXPECT_FALSE(result.ok());  // the probe closes without a greeting
        EXPECT_GT(server.accepted.load(), expected) << type;
        expected = server.accepted.load();
    }
}

TEST_F(ConnectorDestinationTransport, MysqlNeverReachesMetadataOrLinkLocalEvenWhenApproved) {
    zetta::connect::install_mysql_protocol();
    const auto allowed = ConnectorDestinations::parse(
        "169.254.169.254:3306, [fe80::1]:3306, 100.100.100.200:4000");
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    for (const auto& [type, host, port] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"mysql", "169.254.169.254", "3306"},
             {"mysql", "fe80::1", "3306"},
             {"tidb", "100.100.100.200", "4000"},
             {"tidb", "::ffff:169.254.169.254", "4000"}}) {
        const auto result =
            zetta::connect::run_connector_query(mysql_config(type, host, port), "SELECT 1", 1);
        EXPECT_TRUE(result.error.starts_with("destination refused:"))
            << type << " " << host << ": " << result.error;
    }
}

TEST_F(ConnectorDestinationTransport, MysqlDriverRejectsOtherConnectorTypesBeforeConnect) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    const auto allowed = ConnectorDestinations::parse("127.0.0.1:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    zetta::connect::install_mysql_protocol();
    for (const char* type : {"postgres", "lake_sqlite", "snowflake"}) {
        auto config = mysql_config(type, "127.0.0.1", server.port);
        const auto result = zetta::connect::mysql_query_runner()(config, "SELECT 1", 1);
        EXPECT_NE(result.error.find("connector.type"), std::string::npos)
            << type << ": " << result.error;
    }
    // A socket path or a host list is refused for a direct C++ caller too.
    for (const char* host : {"/tmp/mysql.sock", "a,b", "host:3306"}) {
        auto config = mysql_config("mysql", host, server.port);
        const auto result = zetta::connect::mysql_query_runner()(config, "SELECT 1", 1);
        EXPECT_NE(result.error.find("connector.host"), std::string::npos)
            << host << ": " << result.error;
    }
    EXPECT_EQ(server.accepted.load(), 0);
}

// --- lake's SQL services (#1414): the HTTP transport's connector policy ---------

zetta::connect::ConnectorConfig lake_config(const std::string& type, const std::string& endpoint) {
    zetta::connect::ConnectorConfig config;
    config.type = type;
    config.endpoint = endpoint;
    config.token = "test";
    return config;
}

const std::vector<std::string> lake_types = {"lake_sqlite", "lake_duckdb", "lake_polars"};

TEST_F(ConnectorDestinationTransport, LakeServicesRefuseUnapprovedPrivateTargetsBeforeConnect) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    for (const auto& type : lake_types) {
        for (const char* host : {"127.0.0.1", "localhost", "127.1", "[::ffff:127.0.0.1]"}) {
            const auto config =
                lake_config(type, "http://" + std::string(host) + ":" + server.port);
            const auto result = zetta::connect::run_connector_query(config, "SELECT 1", 1);
            EXPECT_NE(result.error.find("destination refused:"), std::string::npos)
                << type << " " << host << ": " << result.error;
        }
        // The Polars catalog lists datasets without a query: the same check.
        zetta::connect::SourceCatalogRequest request;
        const auto catalog = zetta::connect::list_source_catalog(
            lake_config(type, "http://127.0.0.1:" + server.port), request);
        EXPECT_NE(catalog.error.find("destination refused:"), std::string::npos)
            << type << ": " << catalog.error;
    }
    EXPECT_EQ(server.accepted.load(), 0);
}

TEST_F(ConnectorDestinationTransport, LakeServicesConnectToAnExactApprovedAuthorityOnly) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    const auto allowed = ConnectorDestinations::parse("localhost:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    int expected = 0;
    for (const auto& type : lake_types) {
        const auto refused = zetta::connect::run_connector_query(
            lake_config(type, "http://127.0.0.1:" + server.port), "SELECT 1", 1);
        EXPECT_NE(refused.error.find("destination refused:"), std::string::npos) << refused.error;
        EXPECT_EQ(server.accepted.load(), expected) << type;
        // The protocol is the one the endpoint names: plain http to an
        // in-cluster service is allowed when written out.
        const auto result = zetta::connect::run_connector_query(
            lake_config(type, "http://localhost:" + server.port + "/base"), "SELECT 1", 1);
        EXPECT_EQ(result.error.find("destination refused:"), std::string::npos) << result.error;
        EXPECT_EQ(result.error.find("HTTPS"), std::string::npos) << result.error;
        EXPECT_GT(server.accepted.load(), expected) << type;
        expected = server.accepted.load();
    }
}

TEST_F(ConnectorDestinationTransport, LakeServicesNeverReachMetadataOrLinkLocalEvenWhenApproved) {
    const auto allowed = ConnectorDestinations::parse(
        "169.254.169.254:80, [fe80::1]:8080, 100.100.100.200:8080, 169.254.170.2:80");
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    for (const auto& type : lake_types) {
        for (const char* endpoint :
             {"http://169.254.169.254", "http://[fe80::1]:8080", "http://100.100.100.200:8080",
              "http://169.254.170.2:80", "http://[::ffff:169.254.169.254]"}) {
            const auto result =
                zetta::connect::run_connector_query(lake_config(type, endpoint), "SELECT 1", 1);
            EXPECT_NE(result.error.find("destination refused:"), std::string::npos)
                << type << " " << endpoint << ": " << result.error;
        }
    }
}

TEST(LakeServiceConnector, StatesItsProtocolAndCarriesTheConnectorPolicy) {
    for (const auto& type : lake_types) {
        // The protocol must be written out, and the authority must be one host.
        for (const char* endpoint :
             {"orders.ws-1.svc.cluster.local:8080", "ftp://orders:21", "http://a,b:8080",
              "http://user@orders:8080", "http://orders:0", "http://fe80::1%lo0:8080",
              "http://[fe80::1%25lo0]:8080", "http:///path"}) {
            EXPECT_FALSE(zetta::connect::ConnectorConfig::from_json(
                {{"type", type}, {"endpoint", endpoint}, {"token", "t"}}))
                << type << " " << endpoint;
            std::vector<zetta::connect::HttpRequest> seen;
            const zetta::connect::HttpTransport fake =
                [&](const zetta::connect::HttpRequest& request) {
                    seen.push_back(request);
                    return zetta::connect::HttpResponse{};
                };
            const auto result = zetta::connect::run_connector_query(
                lake_config(type, endpoint), "SELECT 1", 1, fake);
            EXPECT_FALSE(result.ok()) << type << " " << endpoint;
            EXPECT_TRUE(seen.empty()) << type << " " << endpoint;
        }
        for (const auto& [endpoint, plaintext] :
             std::vector<std::pair<std::string, bool>>{
                 {"http://orders.ws-1.svc.cluster.local:8080", true},
                 {"HTTPS://orders.ws-1.svc.cluster.local", false}}) {
            std::vector<zetta::connect::HttpRequest> seen;
            const zetta::connect::HttpTransport fake =
                [&](const zetta::connect::HttpRequest& request) {
                    seen.push_back(request);
                    zetta::connect::HttpResponse response;
                    response.status = 200;
                    response.body = R"({"columns":["one"],"rows":[[1]]})";
                    return response;
                };
            ASSERT_TRUE(zetta::connect::ConnectorConfig::from_json(
                {{"type", type}, {"endpoint", endpoint}, {"token", "t"}}))
                << endpoint;
            const auto result = zetta::connect::run_connector_query(
                lake_config(type, endpoint), "SELECT 1 AS one", 1, fake);
            EXPECT_TRUE(result.ok()) << result.error;
            ASSERT_EQ(seen.size(), 1u);
            EXPECT_EQ(seen.front().destinations, zetta::connect::DestinationPolicy::Connector);
            // An https:// service is never followed to plaintext.
            EXPECT_EQ(seen.front().connector_plaintext, plaintext) << endpoint;
        }
    }
}

TEST_F(ConnectorDestinationTransport, HttpsConnectorRequestsRefusePlaintextHops) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    const auto allowed = ConnectorDestinations::parse("localhost:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    zetta::connect::HttpRequest request;
    request.url = "http://localhost:" + server.port + "/";
    request.destinations = zetta::connect::DestinationPolicy::Connector;
    request.timeout_seconds = 1;
    EXPECT_NE(zetta::connect::default_http_transport()(request).error.find("HTTPS"),
              std::string::npos);
    EXPECT_EQ(server.accepted.load(), 0);
    request.connector_plaintext = true;
    EXPECT_EQ(zetta::connect::default_http_transport()(request).error.find("destination refused"),
              std::string::npos);
    EXPECT_GT(server.accepted.load(), 0);
}

// --- a saved connection whose type changes is validated again as its new type ---

struct SavedConnectorDestinations : ConnectorDestinationTransport {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    void SetUp() override {
        ConnectorDestinationTransport::SetUp();
        ASSERT_TRUE(node.start());
        zetta::connect::install_mysql_protocol();
    }
    void TearDown() override {
        node.stop();
        ConnectorDestinationTransport::TearDown();
    }
    zetta::connect::ConnectorResult run_saved(const std::string& name) {
        const auto saved = zetta::connect::find_connection(node, name);
        if (!saved) {
            zetta::connect::ConnectorResult missing;
            missing.error = "no saved connection";
            return missing;
        }
        return zetta::connect::run_connector_query(saved->config, "SELECT 1", 1);
    }
};

TEST_F(SavedConnectorDestinations, ATypeChangeIsRevalidatedBeforeAnyConnection) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    // Saved as an approved lake service.
    const auto allowed = ConnectorDestinations::parse("localhost:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    ASSERT_TRUE(zetta::connect::save_connection(
                    node, "source", lake_config("lake_sqlite", "http://localhost:" + server.port))
                    .empty());
    EXPECT_EQ(run_saved("source").error.find("destination refused"), std::string::npos);
    const int after_lake = server.accepted.load();
    EXPECT_GT(after_lake, 0);

    // Re-saved as MySQL at an address the approval does not name: refused.
    ASSERT_TRUE(zetta::connect::save_connection(
                    node, "source", mysql_config("mysql", "127.0.0.1", server.port))
                    .empty());
    EXPECT_TRUE(run_saved("source").error.starts_with("destination refused:"));
    // Re-saved as TiDB carrying a Postgres-only TLS mode: the MySQL rules apply.
    auto tidb = mysql_config("tidb", "localhost", server.port);
    tidb.sslmode = "prefer";
    zetta::connect::save_connection(node, "source", tidb);
    EXPECT_FALSE(run_saved("source").ok());
    // Re-saved as a lake service whose endpoint does not name its protocol.
    zetta::connect::save_connection(
        node, "source", lake_config("lake_duckdb", "localhost:" + server.port));
    EXPECT_FALSE(run_saved("source").ok());
    EXPECT_EQ(server.accepted.load(), after_lake);
    // A stale config whose type is changed in place is refused by the adapter.
    auto stale = lake_config("lake_polars", "http://localhost:" + server.port);
    stale.type = "mysql";
    EXPECT_FALSE(zetta::connect::run_connector_query(stale, "SELECT 1", 1).ok());
    EXPECT_EQ(server.accepted.load(), after_lake);
}

#if defined(ZETTA_WITH_POSTGRES)
TEST_F(ConnectorDestinationTransport, PostgresRejectsResolvedLoopbackBeforeProtocolConnect) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    zetta::connect::install_postgres_protocol();
    zetta::connect::ConnectorConfig config;
    config.type = "postgres";
    config.port = server.port;
    config.user = "test";
    config.database = "test";
    for (const char* host : {"127.0.0.1", "localhost", "127.1", "2130706433", "::ffff:127.0.0.1"}) {
        config.host = host;
        const auto result = zetta::connect::run_connector_query(config, "SELECT 1", 1);
        EXPECT_TRUE(result.error.starts_with("destination refused:"))
            << host << ": " << result.error;
    }
    EXPECT_EQ(server.accepted.load(), 0);
    const auto allowed = ConnectorDestinations::parse("localhost:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    config.host = "localhost";
    // A libpq environment default must not replace the checked address.
    struct HostaddrScope {
        std::optional<std::string> previous;
        HostaddrScope() {
            if (const char* value = std::getenv("PGHOSTADDR")) previous = value;
            ::setenv("PGHOSTADDR", "127.0.0.2", 1);
        }
        ~HostaddrScope() {
            if (previous) ::setenv("PGHOSTADDR", previous->c_str(), 1);
            else ::unsetenv("PGHOSTADDR");
        }
    } hostaddr;
    const auto result = zetta::connect::run_connector_query(config, "SELECT 1", 1);
    EXPECT_FALSE(result.error.starts_with("destination refused:")) << result.error;
    EXPECT_GT(server.accepted.load(), 0);
}
TEST_F(ConnectorDestinationTransport, PostgresRejectsOtherConnectorProtocolsBeforeConnect) {
    ProbeListener server;
    ASSERT_FALSE(server.port.empty());
    const auto allowed = ConnectorDestinations::parse("127.0.0.1:" + server.port);
    ASSERT_TRUE(allowed);
    zetta::connect::set_connector_destinations(*allowed);
    zetta::connect::install_postgres_protocol();
    zetta::connect::ConnectorConfig config;
    config.type = "snowflake";
    config.account = "account";
    config.token = "test";
    config.host = "127.0.0.1";
    config.port = server.port;
    config.user = "test";
    config.database = "test";
    const auto result = zetta::connect::postgres_query_runner()(config, "SELECT 1", 1);
    EXPECT_NE(result.error.find("connector.type"), std::string::npos) << result.error;
    EXPECT_EQ(server.accepted.load(), 0);
}
#endif
}  // namespace
