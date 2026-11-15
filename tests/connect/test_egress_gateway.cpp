// The workspace egress gateway route (#1331, docs/governance/egress-policy.md,
// "The workspace egress gateway"): which destinations go through the gateway,
// how its address and routes are parsed, and the explicit-routing preface. The
// dialler side (SNI, preface, redirects, the allowlist first) is covered in
// tests/connect/test_native_http_transport.cpp.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "temp_dir.hpp"
#include "zetta/connect/egress_gateway.hpp"

namespace {

using zetta::connect::EgressGateway;

EgressGateway gateway_of(const std::string& address, const std::string& routes) {
    std::string error;
    const auto gateway = EgressGateway::parse(address, routes, &error);
    EXPECT_TRUE(gateway) << error;
    return gateway.value_or(EgressGateway{});
}

bool refused(const std::string& address, const std::string& routes, std::string& error) {
    return !EgressGateway::parse(address, routes, &error).has_value();
}

TEST(EgressGateway, NothingConfiguredMeansNoGateway) {
    const EgressGateway gateway = gateway_of("", "");
    EXPECT_FALSE(gateway.configured());
    EXPECT_FALSE(gateway.routes_destination("api.example.com", 443));
    EXPECT_EQ(gateway.describe(), "none");
    EXPECT_FALSE(zetta::connect::egress_gateway().configured());
}

TEST(EgressGateway, ParsesTheAddressWithTheDefaultPort) {
    const EgressGateway named =
        gateway_of("zetta-tunnel-egress.ws-1.svc.cluster.local", "api.example.com");
    EXPECT_TRUE(named.configured());
    EXPECT_EQ(named.host(), "zetta-tunnel-egress.ws-1.svc.cluster.local");
    EXPECT_EQ(named.port(), zetta::connect::kEgressGatewayDefaultPort);

    const EgressGateway with_port = gateway_of("10.0.0.9:443", "api.example.com");
    EXPECT_EQ(with_port.host(), "10.0.0.9");
    EXPECT_EQ(with_port.port(), 443);

    const EgressGateway ipv6 = gateway_of("[fd00::9]:7445", "api.example.com");
    EXPECT_EQ(ipv6.host(), "fd00::9");
    EXPECT_EQ(ipv6.describe(), "[fd00::9]:7445 for api.example.com");
}

TEST(EgressGateway, RefusesMalformedOrHalfConfiguredSettings) {
    std::string error;
    EXPECT_TRUE(refused("", "api.example.com", error));
    EXPECT_NE(error.find("without a gateway address"), std::string::npos) << error;
    EXPECT_TRUE(refused("gateway.local", "", error));
    EXPECT_NE(error.find("without routes"), std::string::npos) << error;
    EXPECT_TRUE(refused("gateway.local:0", "api.example.com", error));
    EXPECT_TRUE(refused("gateway.local:", "api.example.com", error));
    EXPECT_TRUE(refused("gateway.local:70000", "api.example.com", error));
    EXPECT_TRUE(refused("fd00::9", "api.example.com", error));  // unbracketed IPv6
    EXPECT_TRUE(refused("[fd00::9", "api.example.com", error));
    EXPECT_TRUE(refused("bad host!", "api.example.com", error));
    EXPECT_TRUE(refused("gateway.local", "api.example.com:99999", error));
}

// Only names go through the gateway. Addresses, ranges and `*` are enforced by
// the network directly, so a route of that kind is a configuration error.
TEST(EgressGateway, RoutesAreNamesOnly) {
    std::string error;
    EXPECT_TRUE(refused("gateway.local", "*", error));
    EXPECT_NE(error.find("not a DNS name"), std::string::npos) << error;
    EXPECT_TRUE(refused("gateway.local", "*:443", error));
    EXPECT_TRUE(refused("gateway.local", "203.0.113.7", error));
    EXPECT_TRUE(refused("gateway.local", "[2001:db8::1]:443", error));
    EXPECT_TRUE(refused("gateway.local", "10.0.0.0/8", error));
    EXPECT_TRUE(refused("gateway.local", "localhost", error));
}

TEST(EgressGateway, RoutesMatchNamesAndPorts) {
    const EgressGateway gateway =
        gateway_of("gateway.local", "api.example.com:443, *.data.example.org");
    EXPECT_TRUE(gateway.routes_destination("api.example.com", 443));
    EXPECT_TRUE(gateway.routes_destination("API.Example.com.", 443));
    EXPECT_FALSE(gateway.routes_destination("api.example.com", 8443));
    EXPECT_TRUE(gateway.routes_destination("eu.data.example.org", 5432));
    EXPECT_FALSE(gateway.routes_destination("data.example.org", 443));  // not the apex
    EXPECT_FALSE(gateway.routes_destination("other.example.com", 443));
}

// An in-cluster Service, an address and loopback never go through the gateway,
// even when a route could be read as covering them.
TEST(EgressGateway, AddressesAndLoopbackAreNeverRouted) {
    const EgressGateway gateway = gateway_of("gateway.local", "*.example.com");
    EXPECT_FALSE(gateway.routes_destination("203.0.113.7", 443));
    EXPECT_FALSE(gateway.routes_destination("2001:db8::1", 443));
    EXPECT_FALSE(gateway.routes_destination("localhost", 443));
    EXPECT_FALSE(gateway.routes_destination("127.0.0.1", 443));
    EXPECT_FALSE(gateway.routes_destination("lake-sqlite.ws-1.svc.cluster.local", 8080));
}

TEST(EgressGateway, ThePrefaceNamesTheDestinationExactly) {
    EXPECT_EQ(zetta::connect::egress_gateway_preface("API.Example.com.", 5432),
              "ZTEG1 api.example.com 5432\n");
    EXPECT_TRUE(zetta::connect::egress_gateway_uses_sni(/*secure=*/true, 443));
    EXPECT_FALSE(zetta::connect::egress_gateway_uses_sni(/*secure=*/true, 8443));
    EXPECT_FALSE(zetta::connect::egress_gateway_uses_sni(/*secure=*/false, 443));
    EXPECT_FALSE(zetta::connect::egress_gateway_uses_sni(/*secure=*/false, 80));
}

TEST(EgressGateway, TheProcessGatewayIsInstalledAndRemoved) {
    zetta::connect::set_egress_gateway(gateway_of("gateway.local:7445", "api.example.com"));
    EXPECT_TRUE(zetta::connect::egress_gateway().routes_destination("api.example.com", 443));
    zetta::connect::set_egress_gateway({});
    EXPECT_FALSE(zetta::connect::egress_gateway().configured());
}

// --- The gateway files beside the allowlist file (#1517) ---------------------

using zetta::connect::EgressGatewayFiles;
using Outcome = zetta::connect::EgressGatewayFiles::Outcome;

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

TEST(EgressGatewayFiles, TheDirectoryIsTheAllowFilesDirectory) {
    EXPECT_EQ(EgressGatewayFiles::directory_of("/etc/zetta/egress/allow"), "/etc/zetta/egress");
    EXPECT_EQ(EgressGatewayFiles::directory_of("allow"), ".");
}

TEST(EgressGatewayFiles, WithoutTheFilesTheEnvironmentApplies) {
    zetta::test::TempDir dir;
    const EgressGateway fallback = gateway_of("env-gateway.local", "env.example.com");
    EgressGatewayFiles files(dir.path().string(), fallback);
    const auto loaded = files.load();
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->from_files);
    EXPECT_EQ(loaded->gateway.host(), "env-gateway.local");
    EXPECT_EQ(files.check().outcome, Outcome::Unchanged);
}

TEST(EgressGatewayFiles, TheFilesWinAndAreReReadWithoutARestart) {
    zetta::test::TempDir dir;
    write_file(dir.path() / "gateway", "zetta-tunnel-egress.ws-1.svc.cluster.local:7445\n");
    write_file(dir.path() / "gatewayRoutes", "# name rules\napi.example.com:443\n");
    EgressGatewayFiles files(dir.path().string(), gateway_of("env-gateway.local", "env.example.com"));
    const auto loaded = files.load();
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->from_files);
    EXPECT_TRUE(loaded->gateway.routes_destination("api.example.com", 443));
    EXPECT_FALSE(loaded->gateway.routes_destination("env.example.com", 443));
    EXPECT_EQ(files.check().outcome, Outcome::Unchanged);

    // A name rule is added: the next read applies it.
    write_file(dir.path() / "gatewayRoutes", "api.example.com:443,files.example.org:8443\n");
    const auto added = files.check();
    EXPECT_EQ(added.outcome, Outcome::Applied);
    EXPECT_TRUE(added.gateway.routes_destination("files.example.org", 8443));

    // The last name rule goes: the operator removes both keys, and the
    // environment applies again.
    std::filesystem::remove(dir.path() / "gateway");
    std::filesystem::remove(dir.path() / "gatewayRoutes");
    const auto removed = files.check();
    EXPECT_EQ(removed.outcome, Outcome::Applied);
    EXPECT_FALSE(removed.from_files);
    EXPECT_EQ(removed.gateway.host(), "env-gateway.local");
}

TEST(EgressGatewayFiles, AMalformedReplacementKeepsTheLastGoodGateway) {
    zetta::test::TempDir dir;
    write_file(dir.path() / "gateway", "gateway.local\n");
    write_file(dir.path() / "gatewayRoutes", "api.example.com\n");
    EgressGatewayFiles files(dir.path().string(), EgressGateway{});
    ASSERT_TRUE(files.load());

    write_file(dir.path() / "gatewayRoutes", "10.0.0.0/8\n");  // not a name
    const auto refused = files.check();
    EXPECT_EQ(refused.outcome, Outcome::Refused);
    EXPECT_NE(refused.error.find("not a DNS name"), std::string::npos) << refused.error;
    EXPECT_TRUE(refused.gateway.routes_destination("api.example.com", 443));
    // Reported once: the same bad state reads as unchanged.
    EXPECT_EQ(files.check().outcome, Outcome::Unchanged);

    // Routes without a gateway are refused too.
    std::filesystem::remove(dir.path() / "gateway");
    write_file(dir.path() / "gatewayRoutes", "api.example.com\n");
    EXPECT_EQ(files.check().outcome, Outcome::Refused);
}

TEST(EgressGatewayFiles, AMalformedFileAtStartupStopsTheNode) {
    zetta::test::TempDir dir;
    write_file(dir.path() / "gateway", "gateway.local:0\n");
    write_file(dir.path() / "gatewayRoutes", "api.example.com\n");
    EgressGatewayFiles files(dir.path().string(), EgressGateway{});
    std::string error;
    EXPECT_FALSE(files.load(&error));
    EXPECT_NE(error.find("egress gateway files"), std::string::npos) << error;
}

}  // namespace
