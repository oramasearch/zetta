// The node's outbound egress policy (CTL-4, #75,
// docs/governance/egress-policy.md): denied by default, an explicit allowlist,
// loopback never counted as egress. The dialler-level enforcement is covered in
// tests/connect/test_native_http_transport.cpp.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "zetta/api/native_http_transport.hpp"
#include "zetta/connect/egress_policy.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/governance/audit_event.hpp"

namespace {

using zetta::connect::EgressPolicy;
using zetta::connect::EgressRule;

EgressPolicy policy_of(const std::string& list) {
    std::string error;
    const auto policy = EgressPolicy::parse(list, &error);
    EXPECT_TRUE(policy) << error;
    return policy.value_or(EgressPolicy{});
}

TEST(EgressPolicy, TheDefaultDeniesEveryOutsideDestination) {
    const EgressPolicy policy;
    EXPECT_FALSE(policy.allows_name("api.example.com", 443));
    EXPECT_FALSE(policy.allows_name("203.0.113.7", 80));
    EXPECT_FALSE(policy.allows_name("2001:db8::1", 443));
    EXPECT_FALSE(policy.allows_address("203.0.113.7", 443));
    EXPECT_FALSE(policy.has_network_rule_for(443));
    EXPECT_FALSE(policy.allows_everything());
    EXPECT_EQ(policy.describe(), "deny (loopback only)");
}

TEST(EgressPolicy, TheProcessPolicyStartsAsDeny) {
    EXPECT_TRUE(zetta::connect::egress_policy().rules().empty());
    EXPECT_FALSE(zetta::connect::egress_policy().allows_name("example.com", 443));
}

TEST(EgressPolicy, LoopbackIsNeverEgress) {
    const EgressPolicy policy;
    for (const char* host : {"localhost", "LOCALHOST", "db.localhost", "127.0.0.1",
                             "127.8.9.10", "::1", "[::1]", "::ffff:127.0.0.1"}) {
        EXPECT_TRUE(policy.allows_name(host, 5432)) << host;
        EXPECT_TRUE(zetta::connect::is_loopback_destination(host)) << host;
    }
    EXPECT_TRUE(policy.allows_address("127.0.0.1", 80));
    EXPECT_TRUE(policy.allows_address("::1", 80));
    EXPECT_FALSE(zetta::connect::is_loopback_destination("localhost.example.com"));
    EXPECT_FALSE(zetta::connect::is_loopback_destination("128.0.0.1"));
    EXPECT_FALSE(zetta::connect::is_loopback_destination("::2"));
}

TEST(EgressPolicy, AnExactHostMatchesOnlyThatHost) {
    const EgressPolicy policy = policy_of("API.Example.com.");
    EXPECT_TRUE(policy.allows_name("api.example.com", 443));
    EXPECT_TRUE(policy.allows_name("API.EXAMPLE.COM", 8443));  // no port: any port
    EXPECT_TRUE(policy.allows_name("api.example.com.", 443));
    EXPECT_FALSE(policy.allows_name("example.com", 443));
    EXPECT_FALSE(policy.allows_name("evil.api.example.com", 443));
    EXPECT_FALSE(policy.allows_name("api.example.com.evil.net", 443));
}

TEST(EgressPolicy, APortLimitsTheEntryToThatPort) {
    const EgressPolicy policy = policy_of("api.example.com:443");
    EXPECT_TRUE(policy.allows_name("api.example.com", 443));
    EXPECT_FALSE(policy.allows_name("api.example.com", 80));
}

TEST(EgressPolicy, AWildcardMatchesNamesUnderTheDomainButNotTheApex) {
    const EgressPolicy policy = policy_of("*.snowflakecomputing.com:443");
    EXPECT_TRUE(policy.allows_name("acme.snowflakecomputing.com", 443));
    EXPECT_TRUE(policy.allows_name("a.b.snowflakecomputing.com", 443));
    EXPECT_FALSE(policy.allows_name("snowflakecomputing.com", 443));
    EXPECT_FALSE(policy.allows_name("evilsnowflakecomputing.com", 443));
    EXPECT_FALSE(policy.allows_name("acme.snowflakecomputing.com", 80));
    // A suffix rule never admits an address.
    EXPECT_FALSE(policy.allows_address("203.0.113.7", 443));
}

TEST(EgressPolicy, AnAddressEntryMatchesThatAddressInAnySpelling) {
    const EgressPolicy policy = policy_of("203.0.113.7:5432,[2001:db8::1]:443");
    EXPECT_TRUE(policy.allows_name("203.0.113.7", 5432));
    EXPECT_FALSE(policy.allows_name("203.0.113.8", 5432));
    EXPECT_TRUE(policy.allows_address("203.0.113.7", 5432));
    EXPECT_TRUE(policy.allows_name("2001:0db8:0000::0001", 443));
    EXPECT_TRUE(policy.allows_name("[2001:db8::1]", 443));
    EXPECT_FALSE(policy.allows_name("2001:db8::1", 80));
    EXPECT_TRUE(policy.allows_address("::ffff:203.0.113.7", 5432));
}

TEST(EgressPolicy, ARangeAdmitsResolvedAddressesInsideIt) {
    const EgressPolicy policy = policy_of("10.1.2.3/8:5432 [2001:db8::/32]");
    ASSERT_EQ(policy.rules().size(), 2U);
    EXPECT_EQ(policy.rules()[0].kind, EgressRule::Kind::Network);
    EXPECT_TRUE(policy.has_network_rule_for(5432));
    EXPECT_TRUE(policy.has_network_rule_for(443));  // the IPv6 range has no port

    // A name is not matched by a range until it resolves...
    EXPECT_FALSE(policy.allows_name("db.internal.example", 5432));
    // ...and then only its addresses inside the range are admitted.
    EXPECT_TRUE(policy.allows_address("10.200.0.4", 5432));
    EXPECT_FALSE(policy.allows_address("11.0.0.1", 5432));
    EXPECT_FALSE(policy.allows_address("10.200.0.4", 5433));
    EXPECT_TRUE(policy.allows_address("2001:db8:ffff::9", 443));
    EXPECT_FALSE(policy.allows_address("2001:db9::9", 443));
    EXPECT_TRUE(policy.allows_address("::ffff:10.0.0.1", 5432));
    // An IP literal in a URL is checked against the range directly.
    EXPECT_TRUE(policy.allows_name("10.9.9.9", 5432));
}

TEST(EgressPolicy, PrefixLengthZeroAndFullAreValid) {
    const EgressPolicy everything = policy_of("0.0.0.0/0:443");
    EXPECT_TRUE(everything.allows_address("198.51.100.1", 443));
    EXPECT_FALSE(everything.allows_address("198.51.100.1", 80));
    EXPECT_FALSE(everything.allows_address("2001:db8::1", 443));  // IPv4 only
    const EgressPolicy one = policy_of("198.51.100.1/32");
    EXPECT_TRUE(one.allows_address("198.51.100.1", 1));
    EXPECT_FALSE(one.allows_address("198.51.100.2", 1));
}

TEST(EgressPolicy, StarTurnsTheControlOff) {
    const EgressPolicy policy = policy_of("*");
    EXPECT_TRUE(policy.allows_everything());
    EXPECT_TRUE(policy.allows_name("anything.example", 1));
    EXPECT_TRUE(policy.allows_address("198.51.100.1", 1));

    const EgressPolicy https_only = policy_of("*:443");
    EXPECT_FALSE(https_only.allows_everything());
    EXPECT_TRUE(https_only.allows_name("anything.example", 443));
    EXPECT_FALSE(https_only.allows_name("anything.example", 80));
}

TEST(EgressPolicy, EntriesAreSeparatedByCommasOrWhitespace) {
    const EgressPolicy policy = policy_of(" a.example.com, b.example.com\tc.example.com ,, ");
    EXPECT_EQ(policy.rules().size(), 3U);
    EXPECT_EQ(policy.describe(), "allow a.example.com,b.example.com,c.example.com");
    EXPECT_TRUE(policy_of("").rules().empty());
}

TEST(EgressPolicy, AMalformedEntryIsRefusedWithItsReason) {
    for (const char* bad : {"host:0", "host:65536", "host:abc", "host:", "[::1",
                            "[::1]x", "*.", "*.-bad", "10.0.0.0/33", "10.0.0.0/x",
                            "nope/8", "[2001:db8::/129]", "bad host!", ":443",
                            "-leading.example.com"}) {
        std::string error;
        EXPECT_FALSE(EgressPolicy::parse(bad, &error)) << bad;
        EXPECT_NE(error.find("egress allowlist entry"), std::string::npos) << bad;
    }
}

TEST(EgressPolicy, MergeAppendsTheOtherAllowlist) {
    EgressPolicy policy = policy_of("a.example.com");
    policy.merge(policy_of("b.example.com:443"));
    EXPECT_TRUE(policy.allows_name("a.example.com", 1));
    EXPECT_TRUE(policy.allows_name("b.example.com", 443));
    EXPECT_FALSE(policy.allows_name("b.example.com", 80));
}

TEST(EgressPolicy, TheObserverHearsEachDenial) {
    std::vector<zetta::connect::EgressDenial> heard;
    zetta::connect::set_egress_denial_observer(
        [&heard](const zetta::connect::EgressDenial& denial) { heard.push_back(denial); });
    zetta::connect::report_egress_denial(
        {"x.example", 443, zetta::connect::egress_denied_reason("x.example", 443)});
    zetta::connect::set_egress_denial_observer({});
    zetta::connect::report_egress_denial({"y.example", 80, "ignored"});
    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(heard[0].host, "x.example");
    EXPECT_TRUE(heard[0].reason.starts_with("egress denied: x.example:443"));
}

// #1323: a refused connection names who it was for. A graph step runs inside an
// OutboundCallerScope (the run's principal and the run); a capability call has
// its audit actor; the node's own work has neither.
TEST(EgressPolicy, ADenialNamesThePrincipalAndRunItWasFor) {
    zetta::connect::set_egress_policy({});  // deny everything but loopback
    zetta::api::install_native_http_transport();
    std::vector<zetta::connect::EgressDenial> heard;
    zetta::connect::set_egress_denial_observer(
        [&heard](const zetta::connect::EgressDenial& denial) { heard.push_back(denial); });
    const auto send = [] {
        zetta::connect::HttpRequest request;
        request.method = "GET";
        request.url = "https://denied.example.test/";
        request.timeout_seconds = 3;
        return zetta::connect::default_http_transport()(request);
    };
    {
        const zetta::connect::OutboundCallerScope scope{{"principal-alice", "run-7"}};
        const auto response = send();
        EXPECT_TRUE(response.error.starts_with("egress denied:")) << response.error;
    }
    {
        const zetta::governance::AuditActorScope actor{std::string("principal-bob")};
        EXPECT_TRUE(send().error.starts_with("egress denied:"));
    }
    EXPECT_TRUE(send().error.starts_with("egress denied:"));
    zetta::connect::set_egress_denial_observer({});
    zetta::api::shutdown_native_http_transport();

    ASSERT_EQ(heard.size(), 3U);
    EXPECT_EQ(heard[0].principal, "principal-alice");
    EXPECT_EQ(heard[0].run_id, "run-7");
    EXPECT_EQ(heard[1].principal, "principal-bob");
    EXPECT_TRUE(heard[1].run_id.empty());
    EXPECT_TRUE(heard[2].principal.empty());  // the node's own work
    EXPECT_FALSE(zetta::connect::current_outbound_caller());
}

}  // namespace
