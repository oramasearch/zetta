// zetta#1394 (LSR-161): a URL a caller chose (an action connector's endpoint, a
// solution's report delivery endpoint) may reach only public addresses unless the
// node allows the class. The native HTTP client resolves the host, classifies each
// address, dials only one that passed, and checks every redirect hop again.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <string>
#include <thread>

#include "zetta/api/native_http_transport.hpp"
#include "zetta/connect/destination_policy.hpp"
#include "zetta/connect/egress_policy.hpp"
#include "zetta/connect/http_transport.hpp"

namespace {

using zetta::connect::AddressClass;
using zetta::connect::AllowedDestinations;
using zetta::connect::classify_address;
using zetta::connect::DestinationPolicy;

TEST(DestinationPolicy, ClassifiesIpv4Addresses) {
    EXPECT_EQ(classify_address("8.8.8.8"), AddressClass::Public);
    EXPECT_EQ(classify_address("1.1.1.1"), AddressClass::Public);
    EXPECT_EQ(classify_address("127.0.0.1"), AddressClass::Loopback);
    EXPECT_EQ(classify_address("127.255.0.9"), AddressClass::Loopback);
    EXPECT_EQ(classify_address("10.1.2.3"), AddressClass::Private);
    EXPECT_EQ(classify_address("172.16.0.1"), AddressClass::Private);
    EXPECT_EQ(classify_address("172.31.255.254"), AddressClass::Private);
    EXPECT_EQ(classify_address("172.32.0.1"), AddressClass::Public);
    EXPECT_EQ(classify_address("192.168.1.1"), AddressClass::Private);
    EXPECT_EQ(classify_address("100.64.0.1"), AddressClass::Private);
    EXPECT_EQ(classify_address("169.254.1.1"), AddressClass::LinkLocal);
    EXPECT_EQ(classify_address("169.254.169.254"), AddressClass::Metadata);
    EXPECT_EQ(classify_address("169.254.170.2"), AddressClass::Metadata);
    EXPECT_EQ(classify_address("100.100.100.200"), AddressClass::Metadata);
    EXPECT_EQ(classify_address("0.0.0.0"), AddressClass::Reserved);
    EXPECT_EQ(classify_address("224.0.0.1"), AddressClass::Reserved);
    EXPECT_EQ(classify_address("255.255.255.255"), AddressClass::Reserved);
    EXPECT_EQ(classify_address("192.0.2.10"), AddressClass::Reserved);
}

TEST(DestinationPolicy, ClassifiesIpv6AddressesAndTheIpv4TheyCarry) {
    EXPECT_EQ(classify_address("2606:4700:4700::1111"), AddressClass::Public);
    EXPECT_EQ(classify_address("::1"), AddressClass::Loopback);
    EXPECT_EQ(classify_address("::"), AddressClass::Reserved);
    EXPECT_EQ(classify_address("fe80::1"), AddressClass::LinkLocal);
    EXPECT_EQ(classify_address("fd12:3456::1"), AddressClass::Private);
    EXPECT_EQ(classify_address("fc00::1"), AddressClass::Private);
    EXPECT_EQ(classify_address("fd00:ec2::254"), AddressClass::Metadata);
    EXPECT_EQ(classify_address("ff02::1"), AddressClass::Reserved);
    EXPECT_EQ(classify_address("2001:db8::1"), AddressClass::Reserved);
    // An IPv6 form of a refused IPv4 address is refused the same way.
    EXPECT_EQ(classify_address("::ffff:127.0.0.1"), AddressClass::Loopback);
    EXPECT_EQ(classify_address("::ffff:169.254.169.254"), AddressClass::Metadata);
    EXPECT_EQ(classify_address("::ffff:10.0.0.1"), AddressClass::Private);
    EXPECT_EQ(classify_address("64:ff9b::a9fe:a9fe"), AddressClass::Metadata);
    EXPECT_EQ(classify_address("::ffff:8.8.8.8"), AddressClass::Public);
    // Not an address literal: never dialled.
    EXPECT_EQ(classify_address("example.com"), AddressClass::Reserved);
    EXPECT_EQ(classify_address(""), AddressClass::Reserved);
}

TEST(DestinationPolicy, AllowanceParsesClassNamesAndNeverAllowsReserved) {
    std::string error;
    const auto none = AllowedDestinations::parse("", &error);
    ASSERT_TRUE(none) << error;
    EXPECT_EQ(none->describe(), "public only");
    EXPECT_TRUE(zetta::connect::destination_allowed(AddressClass::Public, *none));
    EXPECT_FALSE(zetta::connect::destination_allowed(AddressClass::Loopback, *none));
    EXPECT_FALSE(zetta::connect::destination_allowed(AddressClass::Metadata, *none));

    const auto some = AllowedDestinations::parse("loopback, private", &error);
    ASSERT_TRUE(some) << error;
    EXPECT_TRUE(zetta::connect::destination_allowed(AddressClass::Loopback, *some));
    EXPECT_TRUE(zetta::connect::destination_allowed(AddressClass::Private, *some));
    EXPECT_FALSE(zetta::connect::destination_allowed(AddressClass::LinkLocal, *some));
    EXPECT_FALSE(zetta::connect::destination_allowed(AddressClass::Metadata, *some));
    EXPECT_EQ(some->describe(), "public + loopback,private");

    // Allowing link-local does not allow the metadata endpoints inside it.
    const auto link = AllowedDestinations::parse("link-local", &error);
    ASSERT_TRUE(link) << error;
    EXPECT_FALSE(zetta::connect::destination_allowed(AddressClass::Metadata, *link));

    const auto all = AllowedDestinations::parse("loopback private link-local metadata", &error);
    ASSERT_TRUE(all) << error;
    EXPECT_FALSE(zetta::connect::destination_allowed(AddressClass::Reserved, *all));

    EXPECT_FALSE(AllowedDestinations::parse("everything", &error));
    EXPECT_NE(error.find("everything"), std::string::npos);
    EXPECT_FALSE(AllowedDestinations::parse("reserved", &error));
}

// A loopback HTTP server that answers each connection with one canned response
// and counts the connections it accepted.
class CannedServer {
public:
    explicit CannedServer(std::string response) : response_(std::move(response)) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int enabled = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(socket_, 8) != 0 ||
            ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return;
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] {
            for (;;) {
                const int connection = ::accept(socket_, nullptr, nullptr);
                if (connection < 0) return;
                ++accepted_;
                std::array<char, 4096> buffer{};
                std::string request;
                // Read the whole request (header and Content-Length body) so closing
                // the socket does not reset the connection under unread bytes.
                std::size_t expected = std::string::npos;
                while (expected == std::string::npos || request.size() < expected) {
                    const ssize_t received = ::recv(connection, buffer.data(), buffer.size(), 0);
                    if (received <= 0) break;
                    request.append(buffer.data(), static_cast<std::size_t>(received));
                    const std::size_t end = request.find("\r\n\r\n");
                    if (expected == std::string::npos && end != std::string::npos) {
                        std::size_t length = 0;
                        const std::size_t field = request.find("Content-Length:");
                        if (field != std::string::npos && field < end) {
                            length = std::stoul(request.substr(field + 15, end - field - 15));
                        }
                        expected = end + 4 + length;
                    }
                }
                ::send(connection, response_.data(), response_.size(), 0);
                ::close(connection);
            }
        });
    }
    ~CannedServer() {
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        if (worker_.joinable()) worker_.join();
    }
    CannedServer(const CannedServer&) = delete;
    CannedServer& operator=(const CannedServer&) = delete;

    std::string url(const std::string& host = "127.0.0.1") const {
        return "http://" + host + ":" + std::to_string(port_) + "/hook";
    }
    int accepted() const { return accepted_.load(); }
    std::uint16_t port() const { return port_; }

private:
    std::string response_;
    int socket_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<int> accepted_{0};
    std::thread worker_;
};

const std::string kOk =
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";

struct NativeTransportDestinations : ::testing::Test {
    // The egress allowlist (#1321) runs first in the same dial loop and denies
    // non-loopback by default. Allow everything, as Lake's Zettas do
    // (ZETTA_EGRESS_ALLOW=*), so these tests exercise the destination policy
    // itself rather than an earlier egress refusal.
    void SetUp() override {
        const auto everything = zetta::connect::EgressPolicy::parse("*");
        ASSERT_TRUE(everything);
        zetta::connect::set_egress_policy(*everything);
        zetta::api::install_native_http_transport();
    }
    void TearDown() override {
        zetta::connect::set_allowed_destinations({});
        zetta::connect::set_egress_policy({});
        zetta::api::shutdown_native_http_transport();
    }

    static zetta::connect::HttpResponse send(const std::string& url, DestinationPolicy policy,
                                             int redirects = 0) {
        zetta::connect::HttpRequest request;
        request.method = "POST";
        request.url = url;
        request.body = "{}";
        request.timeout_seconds = 3;
        request.max_redirects = redirects;
        request.destinations = policy;
        return zetta::connect::default_http_transport()(request);
    }
};

TEST_F(NativeTransportDestinations, PublicOnlyRefusesLoopbackBeforeConnecting) {
    CannedServer server(kOk);
    ASSERT_NE(server.port(), 0);
    for (const std::string host : {"127.0.0.1", "localhost"}) {
        const auto response = send(server.url(host), DestinationPolicy::PublicOnly);
        EXPECT_FALSE(response.ok()) << host;
        EXPECT_EQ(response.error.rfind("destination refused:", 0), 0u)
            << host << ": " << response.error;
    }
    EXPECT_EQ(server.accepted(), 0);
}

TEST_F(NativeTransportDestinations, AnyAndAnAllowedClassStillConnect) {
    CannedServer server(kOk);
    ASSERT_NE(server.port(), 0);
    // A request that did not opt in (an operator-configured destination) is unchanged.
    const auto before = send(server.url(), DestinationPolicy::Any);
    EXPECT_TRUE(before.ok()) << before.error;
    // The node may allow loopback (a sidecar, a local fixture).
    AllowedDestinations allowed;
    allowed.loopback = true;
    zetta::connect::set_allowed_destinations(allowed);
    const auto response = send(server.url(), DestinationPolicy::PublicOnly);
    EXPECT_TRUE(response.ok()) << response.error;
    EXPECT_EQ(server.accepted(), 2);
}

// Each redirect hop is checked again: an allowed first hop cannot bounce the
// request to the metadata endpoint.
TEST_F(NativeTransportDestinations, EveryRedirectHopIsChecked) {
    CannedServer server(
        "HTTP/1.1 302 Found\r\nLocation: http://169.254.169.254/latest/meta-data/\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n");
    ASSERT_NE(server.port(), 0);
    AllowedDestinations allowed;
    allowed.loopback = true;
    zetta::connect::set_allowed_destinations(allowed);
    const auto response = send(server.url(), DestinationPolicy::PublicOnly, /*redirects=*/2);
    EXPECT_FALSE(response.ok());
    EXPECT_EQ(response.error.rfind("destination refused:", 0), 0u) << response.error;
    EXPECT_NE(response.error.find("metadata"), std::string::npos) << response.error;
    EXPECT_EQ(server.accepted(), 1);
}

}  // namespace
