// The node's outbound egress policy (CTL-4, #75,
// docs/governance/egress-policy.md): denied by default, an explicit allowlist,
// loopback never counted as egress. The dialler-level enforcement is covered in
// tests/connect/test_native_http_transport.cpp.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/native_http_transport.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/connect/egress_policy.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/identity_seal.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/runtime.hpp"

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

// #1328: the denial carries the surface the caller came through, so its audit
// row (recorded on the client's thread, where no scope is open) names it.
TEST(EgressPolicy, ADenialCarriesTheCallersSurface) {
    zetta::connect::set_egress_policy({});
    zetta::api::install_native_http_transport();
    std::vector<zetta::connect::EgressDenial> heard;
    zetta::connect::set_egress_denial_observer(
        [&heard](const zetta::connect::EgressDenial& denial) { heard.push_back(denial); });
    zetta::connect::HttpRequest request;
    request.method = "GET";
    request.url = "https://denied.example.test/";
    request.timeout_seconds = 3;
    {
        const zetta::governance::AuditActorScope actor{std::string("principal-bob")};
        const zetta::governance::AuditSurfaceScope surface{std::string("mcp")};
        EXPECT_TRUE(zetta::connect::default_http_transport()(request).error.starts_with(
            "egress denied:"));
    }
    EXPECT_TRUE(
        zetta::connect::default_http_transport()(request).error.starts_with("egress denied:"));
    zetta::connect::set_egress_denial_observer({});
    zetta::api::shutdown_native_http_transport();

    ASSERT_EQ(heard.size(), 2U);
    EXPECT_EQ(heard[0].surface, "mcp");
    EXPECT_TRUE(heard[1].surface.empty());  // none declared: the row says internal
}

// ---- per-principal egress scopes (#1323) ------------------------------------

TEST(EgressScope, TheValueIsValidatedAtTheBoundary) {
    using zetta::connect::parse_egress_scope_value;
    const nlohmann::json good = {
        {"name", "analysts"}, {"principals", {"user-1", "role-2"}}, {"allow", "*.example.com:443"}};
    const auto parsed = parse_egress_scope_value(good);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->principals.size(), 2U);
    EXPECT_EQ(zetta::connect::to_json(*parsed), good);

    std::string error;
    EXPECT_FALSE(parse_egress_scope_value({{"name", "a"}, {"principals", nlohmann::json::array()},
                                           {"allow", ""}}, &error));
    EXPECT_FALSE(parse_egress_scope_value({{"name", "a b"}, {"principals", {"u"}}, {"allow", ""}}));
    EXPECT_FALSE(parse_egress_scope_value({{"name", "a"}, {"principals", {"u"}}, {"allow", "bad host!"}},
                                          &error));
    EXPECT_NE(error.find("egress allowlist entry"), std::string::npos) << error;
    EXPECT_FALSE(parse_egress_scope_value(
        {{"name", "a"}, {"principals", {"u"}}, {"allow", ""}, {"extra", 1}}));
    // An empty allowlist is valid: loopback only.
    EXPECT_TRUE(parse_egress_scope_value({{"name", "a"}, {"principals", {"u"}}, {"allow", ""}}));
    // The record is sealed like identity records.
    EXPECT_TRUE(zetta::governance::is_identity_key("egress_scope:4f1c"));
}

// The design's test: a step run by a principal whose scope lacks a host is
// refused, the denial names that principal and says the scope refused it; the
// same host is admitted for a principal with no scope. Loopback stays exempt.
TEST(EgressScope, APrincipalsScopeNarrowsTheNodeAllowlist) {
    std::string error;
    const auto node_policy =
        zetta::connect::EgressPolicy::parse("scoped.example.test,other.example.test", &error);
    ASSERT_TRUE(node_policy) << error;
    zetta::connect::set_egress_policy(*node_policy);
    std::map<std::string, zetta::connect::PrincipalEgress> scopes;
    scopes["principal-carol"] = {*zetta::connect::EgressPolicy::parse("other.example.test"),
                                 {"carol-only-other"}};
    zetta::connect::set_egress_scopes(std::move(scopes));
    zetta::api::install_native_http_transport();
    std::vector<zetta::connect::EgressDenial> heard;
    zetta::connect::set_egress_denial_observer(
        [&heard](const zetta::connect::EgressDenial& denial) { heard.push_back(denial); });
    const auto send = [](const std::string& url) {
        zetta::connect::HttpRequest request;
        request.method = "GET";
        request.url = url;
        request.timeout_seconds = 3;
        return zetta::connect::default_http_transport()(request);
    };
    {
        const zetta::connect::OutboundCallerScope scope{{"principal-carol", "run-9"}};
        const auto refused = send("https://scoped.example.test/");
        EXPECT_TRUE(refused.error.starts_with("egress denied:")) << refused.error;
        EXPECT_NE(refused.error.find("egress scope of principal-carol"), std::string::npos)
            << refused.error;
        // Loopback is never egress, whatever the scope.
        EXPECT_FALSE(send("http://127.0.0.1:9/").error.starts_with("egress denied:"));
    }
    {
        const zetta::connect::OutboundCallerScope scope{{"principal-dave", "run-10"}};
        // No scope: the node allowlist alone admits it (it then fails to
        // resolve, which is not an egress refusal).
        EXPECT_FALSE(send("https://scoped.example.test/").error.starts_with("egress denied:"));
    }
    zetta::connect::set_egress_denial_observer({});
    zetta::api::shutdown_native_http_transport();
    zetta::connect::set_egress_scopes({});
    zetta::connect::set_egress_policy({});

    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(heard[0].principal, "principal-carol");
    EXPECT_EQ(heard[0].run_id, "run-9");
    EXPECT_EQ(heard[0].refused_by, "scope");
    EXPECT_EQ(heard[0].host, "scoped.example.test");
}

// Work runtime::offload hands to a pool worker keeps who it dials out for.
TEST(EgressScope, OffloadedWorkKeepsItsOutboundCaller) {
    const zetta::connect::OutboundCallerScope scope{{"principal-erin", "run-11"}};
    const auto seen = zetta::runtime::block_on(
        zetta::runtime::offload([] { return zetta::connect::current_outbound_caller(); }));
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->principal, "principal-erin");
    EXPECT_EQ(seen->run_id, "run-11");
}

// The node compiles its scopes into the dialler's snapshot: a role names every
// user that holds it, scopes naming one principal are unioned, and a drop
// removes the restriction. Only cluster:admin may create one.
TEST(EgressScope, TheNodePublishesItsScopesForTheDialler) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();
    auto writer = node.create_user("wren");
    auto admin = node.create_user("ada");
    ASSERT_TRUE(writer && admin);
    const std::string writer_role = node.find_role("writer")->id();
    node.assign_role(writer->id(), writer_role);
    node.assign_role(admin->id(), node.find_role("admin")->id());

    std::string error;
    ASSERT_TRUE(node.create_egress_scope({"writers", {writer_role}, "a.example.com"}, &error))
        << error;
    ASSERT_TRUE(node.create_egress_scope({"wren", {writer->id()}, "b.example.com:443"}, &error))
        << error;
    auto scope = zetta::connect::egress_scope_for(writer->id());
    ASSERT_TRUE(scope);
    EXPECT_TRUE(scope->policy.allows_name("a.example.com", 443));
    EXPECT_TRUE(scope->policy.allows_name("b.example.com", 443));
    EXPECT_FALSE(scope->policy.allows_name("c.example.com", 443));
    EXPECT_EQ(scope->scopes.size(), 2U);
    EXPECT_FALSE(zetta::connect::egress_scope_for(admin->id()));
    EXPECT_FALSE(zetta::connect::egress_scope_for("local"));
    EXPECT_FALSE(node.create_egress_scope({"bad", {writer->id()}, "bad host!"}, &error));

    // Over the registry: a writer may not manage scopes; an admin may.
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    const auto trace = zetta::observability::make_root_trace();
    const auto call = [&](const std::string& user, const std::string& name,
                          nlohmann::json input) {
        zetta::api::ApiContext context{node, nullptr, user, trace, &node.span_recorder()};
        context.acting_is_user = true;
        return registry.call(name, input, context);
    };
    EXPECT_EQ(call(writer->id(), "drop_egress_scope", {{"name", "wren"}}).error.code,
              "unauthorized");
    const auto dropped = call(admin->id(), "drop_egress_scope", {{"name", "wren"}});
    ASSERT_TRUE(dropped.ok) << dropped.error.message;
    EXPECT_TRUE(dropped.value.at("removed").get<bool>());
    scope = zetta::connect::egress_scope_for(writer->id());
    ASSERT_TRUE(scope);
    EXPECT_FALSE(scope->policy.allows_name("b.example.com", 443));

    // A scope survives a restart of a standalone node.
    node.stop();
    zetta::Node reloaded{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(reloaded.start());
    ASSERT_EQ(reloaded.egress_scopes().size(), 1U);
    EXPECT_EQ(reloaded.egress_scopes().front().name, "writers");
    reloaded.publish_egress_scopes();
    EXPECT_TRUE(zetta::connect::egress_scope_for(writer->id()));
    reloaded.stop();
    EXPECT_FALSE(zetta::connect::egress_scope_for(writer->id()));
}

}  // namespace
