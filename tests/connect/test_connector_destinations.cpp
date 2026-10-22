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
#include <thread>

#include "zetta/api/native_http_transport.hpp"
#include "zetta/api/postgres_protocol.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/connector_destinations.hpp"
#include "zetta/connect/destination_policy.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/connect/postgres_runner.hpp"

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
        zetta::api::install_native_http_transport();
    }
    void TearDown() override {
        zetta::api::shutdown_native_http_transport();
        zetta::connect::set_connector_destinations({});
        zetta::connect::set_allowed_destinations({});
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
