// The default-deny auth gate (#745, epic #743): the ONE router-seam middleware
// that resolves a bearer -> user on EVERY path under --require-auth, and the
// per-user effective-permission check it feeds. Two layers here:
//   1. a unit layer over Node::authorize_user (deterministic, no HTTP), proving
//      the capability decision runs against the union of a user's roles; and
//   2. a protocol layer (real loopback HTTP, the test thread playing the owning
//      loop via drain()) proving: with --require-auth every route is 401 without
//      a valid token and reachable with the bootstrap owner's token, a token
//      resolves to its user so the capability check runs against that user, and
//      with the gate off the unauthenticated-local session still works.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/api/server.hpp"
#include "zetta/api/clerk_auth.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/governance/failed_auth_limiter.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/node.hpp"

namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

// ---------------------------------------------------------------------------
// Unit layer: Node::authorize_user resolves a user's EFFECTIVE permissions
// (the union of its assigned roles' capabilities), deny-by-default.
// ---------------------------------------------------------------------------
TEST(AuthorizeUser, ChecksEffectivePermissionsOfAssignedRoles) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();

    auto admin_user = node.create_user("admin_user");
    auto reader_user = node.create_user("reader_user");
    auto no_role_user = node.create_user("no_role_user");
    ASSERT_TRUE(admin_user && reader_user && no_role_user);
    node.assign_role(admin_user->id(), node.find_role("admin")->id());
    node.assign_role(reader_user->id(), node.find_role("reader")->id());

    const Operation query{CapabilityDomain::Api, "query"};
    const Operation write{CapabilityDomain::Data, "write"};
    const Operation create_project{CapabilityDomain::Orchestration,
                                   "create_project"};
    // A right in no vocabulary atom — the deny-by-default probe (no wildcard).
    const Operation phantom{CapabilityDomain::Data, "teleport"};

    // admin: reaches query + write, and — since #799 — the Orchestration control
    // surface too (create_project), so the owner can actually administer. But a
    // right no atom grants is still denied (deny-by-default — no wildcard).
    EXPECT_TRUE(node.authorize_user(admin_user->id(), query).allowed);
    EXPECT_TRUE(node.authorize_user(admin_user->id(), write).allowed);
    EXPECT_TRUE(node.authorize_user(admin_user->id(), create_project).allowed);
    EXPECT_FALSE(node.authorize_user(admin_user->id(), phantom).allowed);

    // reader: query yes, write no.
    EXPECT_TRUE(node.authorize_user(reader_user->id(), query).allowed);
    EXPECT_FALSE(node.authorize_user(reader_user->id(), write).allowed);

    // a user with no roles has no effective rights at all.
    EXPECT_FALSE(node.authorize_user(no_role_user->id(), query).allowed);

    // unknown + disabled users are denied outright.
    EXPECT_FALSE(node.authorize_user("no-such-user", query).allowed);
    ASSERT_TRUE(node.set_user_status("reader_user",
                                     zetta::governance::UserStatus::Disabled));
    EXPECT_FALSE(node.authorize_user(reader_user->id(), query).allowed);

    node.stop();
}

// ---------------------------------------------------------------------------
// Protocol layer: a dumb blocking HTTP/1.1 client (one request, Connection:
// close), run on a helper thread while the test thread drains the server.
// ---------------------------------------------------------------------------
struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_request(unsigned short port, const std::string& method,
                          const std::string& target, const std::string& body,
                          const std::string& bearer) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(fd);
        return {};
    }
    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    if (!body.empty()) request += "Content-Type: application/json\r\n";
    if (!bearer.empty()) request += "Authorization: Bearer " + bearer + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    ::send(fd, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(fd, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(fd);

    HttpResponse parsed;
    if (response.size() > 12) parsed.status = std::atoi(response.substr(9, 3).c_str());
    const auto separator = response.find("\r\n\r\n");
    if (separator != std::string::npos) parsed.body = response.substr(separator + 4);
    return parsed;
}

// Drive one request from a helper thread while the calling (owning) thread
// drains the server's bridge — the gate AND the handler each marshal onto it.
HttpResponse drive(zetta::api::ApiServer& server, const std::string& method,
                   const std::string& target, const std::string& body = {},
                   const std::string& bearer = {}) {
    HttpResponse response;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        response = http_request(server.port(), method, target, body, bearer);
        finished = true;
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();
    return response;
}

struct RequireAuthFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    std::string owner_secret = "owner-access-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        // Seed the owner (admin) + its access token from a known secret so a
        // client can authenticate as the bootstrap owner (#733/#744).
        const auto boot = node.bootstrap_owner(owner_secret);
        ASSERT_TRUE(boot.performed) << boot.reason;

        zetta::api::ServerOptions options;
        options.port = 0;  // ephemeral — the default-deny gate is always on (#799)
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
        ASSERT_NE(server->port(), 0);
    }
    void TearDown() override {
        server->stop();
        node.stop();
    }
};

// Acceptance: with --require-auth, NO path is reachable without a valid token.
TEST_F(RequireAuthFixture, EveryRouteDeniesWithoutToken) {
    EXPECT_EQ(drive(*server, "POST", "/mcp",
                    R"({"jsonrpc":"2.0","id":1,"method":"ping"})")
                  .status,
              401);
    EXPECT_EQ(drive(*server, "POST", "/v1/sql", R"({"sql":"SELECT 1"})").status,
              401);
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info").status, 401);
    EXPECT_EQ(drive(*server, "GET", "/v1/specs").status, 401);
    // /llms.txt is a PUBLIC discovery doc — registered before the gate, like
    // /mcp.md (the web-ui public-discovery-docs change): 200 without a token
    // by design.
    EXPECT_EQ(drive(*server, "GET", "/llms.txt").status, 200);
    // '/' serves the PUBLIC web-UI shell (an SPA login page — no secrets; its JS
    // fetches the gated /v1/* data endpoints under a bearer), so it is 200 by
    // design: it is the one deliberate public route (see the web_ui_page route in
    // http_server.cpp). The gate still fronts any UNMATCHED path (401, not 404).
    EXPECT_EQ(drive(*server, "GET", "/").status, 200);
    EXPECT_EQ(drive(*server, "GET", "/no/such/path").status, 401);
    // A wrong token is no better than none.
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, "not-the-secret").status,
              401);
}

// #1417 (LSR-244): invalid bearers from one address spend a failed-
// authentication budget. Once it is spent, the address's bearers get only the
// bounded lookups and a failure answers 429: a never-seen unprefixed bearer is
// not scanned, while an issued token and the already verified owner secret
// keep working. A request with no credential never touches the budget.
TEST_F(RequireAuthFixture, RepeatedInvalidBearersFromOneAddressAreRateLimited) {
    // The owner's secret is unprefixed: its first check scans, after which it
    // is found by its digest.
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, owner_secret).status, 200);
    for (int request = 0; request < 30; ++request) {
        EXPECT_EQ(drive(*server, "GET", "/v1/server_info").status, 401) << request;
    }
    for (std::uint32_t failure = 0; failure < zetta::governance::FailedAuthLimiter::kBurst;
         ++failure) {
        EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {},
                        "wrong-" + std::to_string(failure))
                      .status,
                  401)
            << failure;
    }
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, "wrong-again").status, 429);
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, owner_secret).status, 200);
    auto reader = node.create_user("limited-reader");
    ASSERT_TRUE(reader);
    const auto issued = node.create_token(reader->id(), zetta::governance::TokenType::Api);
    ASSERT_TRUE(issued.token);
    // Authenticated (the gate passes) and denied the capability: 403, not 429.
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, issued.secret).status, 403);
    std::this_thread::sleep_for(zetta::governance::FailedAuthLimiter::kRefill +
                                std::chrono::milliseconds(200));
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, "wrong-after-refill").status, 401);
}

// Acceptance: the bootstrap owner's token opens every route (the gate passes).
TEST_F(RequireAuthFixture, OwnerTokenPassesGateOnEveryRoute) {
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, owner_secret).status,
              200);
    EXPECT_EQ(drive(*server, "GET", "/v1/specs", {}, owner_secret).status, 200);
    EXPECT_EQ(drive(*server, "GET", "/llms.txt", {}, owner_secret).status, 200);

    const HttpResponse ping = drive(*server, "POST", "/mcp",
                                    R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                                    owner_secret);
    EXPECT_EQ(ping.status, 200);
    const auto ping_json = nlohmann::json::parse(ping.body, nullptr, false);
    EXPECT_TRUE(ping_json.contains("result"));
}

// Acceptance: a valid token resolves to its user, and the capability check runs
// against THAT user's effective permissions — same route, different verdicts.
TEST_F(RequireAuthFixture, CapabilityCheckRunsAgainstResolvedUser) {
    // The owner is admin -> holds api:query -> server_info (Api:query) is 200.
    EXPECT_EQ(drive(*server, "GET", "/v1/server_info", {}, owner_secret).status,
              200);

    // A second user with NO roles authenticates (valid token, gate passes) but
    // is denied the capability: server_info needs api:query, which no role
    // grants it -> 403 (unauthorized), not 401. Proves the check is per-user.
    auto nobody = node.create_user("nobody");
    ASSERT_TRUE(nobody);
    const auto issued =
        node.create_token(nobody->id(), zetta::governance::TokenType::Api);
    ASSERT_TRUE(issued.token);
    ASSERT_FALSE(issued.secret.empty());

    const HttpResponse denied =
        drive(*server, "GET", "/v1/server_info", {}, issued.secret);
    EXPECT_EQ(denied.status, 403);
    EXPECT_NE(denied.body.find("unauthorized"), std::string::npos);
}

// #1408: the same diagnostic grant protects REST and MCP with real native tokens.
TEST_F(RequireAuthFixture, TraceReadsRequireDiagnosticRightsOnRestAndMcp) {
    const auto victim = drive(*server, "POST", "/v1/create_project",
                              R"({"name":"victim","intent":"private request content"})",
                              owner_secret);
    ASSERT_EQ(victim.status, 200);
    const auto trace_id = nlohmann::json::parse(victim.body).at("trace_id").get<std::string>();
    const auto reader = node.create_user("trace-reader");
    ASSERT_TRUE(reader);
    ASSERT_TRUE(node.assign_role(reader->id(), node.find_role("reader")->id()));
    const auto issued = node.create_token(reader->id(), zetta::governance::TokenType::Api);
    ASSERT_TRUE(issued.token);

    for (const char* name : {"list_traces", "get_trace"}) {
        SCOPED_TRACE(name);
        const std::string route = std::string("/v1/") + name +
            (std::string_view(name) == "get_trace" ? "?trace_id=" + trace_id : "?limit=50");
        const auto denied = drive(*server, "GET", route, {}, issued.secret);
        EXPECT_EQ(denied.status, 403);
        EXPECT_EQ(denied.body.find("private request content"), std::string::npos);
        const auto admin_read = drive(*server, "GET", route, {}, owner_secret);
        EXPECT_EQ(admin_read.status, 200);
        EXPECT_EQ(admin_read.body.find(owner_secret), std::string::npos);
        EXPECT_EQ(admin_read.body.find(issued.secret), std::string::npos);

        const nlohmann::json arguments = std::string_view(name) == "get_trace"
            ? nlohmann::json{{"trace_id", trace_id}} : nlohmann::json{{"limit", 50}};
        const nlohmann::json request{{"jsonrpc", "2.0"}, {"id", 1},
                                      {"method", "tools/call"},
                                      {"params", {{"name", name}, {"arguments", arguments}}}};
        const auto mcp = drive(*server, "POST", "/mcp", request.dump(), issued.secret);
        ASSERT_EQ(mcp.status, 200);
        const auto result = nlohmann::json::parse(mcp.body).at("result");
        EXPECT_TRUE(result.at("isError").get<bool>());
        EXPECT_EQ(result.at("structuredContent").at("error").at("code"), "unauthorized");
        EXPECT_EQ(mcp.body.find("private request content"), std::string::npos);
    }
}

// Auth is unconditional (#799): there is NO unauthenticated-local HTTP mode and no
// toggle to reach one. Even on a loopback bind with an empty token store, a gated
// route is 401 — the only way in is a bearer that resolves to a user.
TEST(AuthGateAlwaysOn, NoUnauthenticatedLocalHttpMode) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());

    zetta::api::ServerOptions options;
    options.port = 0;
    zetta::api::ApiServer server(node, nullptr, options);
    ASSERT_TRUE(server.start()) << server.reason();

    EXPECT_EQ(drive(server, "GET", "/v1/server_info").status, 401);
    const HttpResponse ping = drive(
        server, "POST", "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    EXPECT_EQ(ping.status, 401);

    server.stop();
    node.stop();
}

// A cold Clerk fetch must suspend the gate so the same HTTP executor can serve
// native tokens. Both transport shapes are installed to catch a sync regression.
TEST(AuthGateClerk, AnIssuerOutageLeavesNativeAuthenticationResponsive) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner("clerk-outage-owner").performed);
    zetta::api::ServerOptions options;
    options.port = 0;
    options.clerk_issuer = "https://clerk.example.dev";
    zetta::api::ApiServer server(node, nullptr, options);
    ASSERT_TRUE(server.start()) << server.reason();
    zetta::api::clear_clerk_jwks_cache();
    std::atomic<bool> fetching{false};
    zetta::runtime::async_event release;
    zetta::connect::set_default_http_transport(
        [&](const auto&) {
            fetching = true;
            [[maybe_unused]] const auto waited = zetta::runtime::block_on(release.wait());
            return zetta::connect::HttpResponse{503, {}, {}};
        },
        [&](const auto&) -> zetta::runtime::io_task<zetta::connect::HttpResponse> {
            fetching = true;
            const auto [error] = co_await release.wait();
            co_return {error, zetta::connect::HttpResponse{503, {}, {}}};
        });
    // Correct issuer/algorithm, arbitrary signature: no trusted identity is
    // needed to reach the key fetch.
    const std::string jwt =
        "eyJhbGciOiJSUzI1NiIsImtpZCI6InVua25vd24ifQ."
        "eyJpc3MiOiJodHRwczovL2NsZXJrLmV4YW1wbGUuZGV2Iiwic3ViIjoidXNlciJ9.AA";
    HttpResponse clerk_response, native_response;
    std::atomic<bool> clerk_done{false}, native_done{false};
    std::thread clerk_client([&] {
        clerk_response = http_request(server.port(), "GET", "/v1/server_info", {}, jwt);
        clerk_done = true;
    });
    const auto fetch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fetching && std::chrono::steady_clock::now() < fetch_deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(fetching);
    std::thread native_client([&] {
        native_response = http_request(server.port(), "GET", "/v1/server_info", {},
                                       "clerk-outage-owner");
        native_done = true;
    });
    const auto native_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!native_done && std::chrono::steady_clock::now() < native_deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(native_done) << "native authentication waited for the Clerk fetch";
    EXPECT_FALSE(clerk_done);
    release.set();
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((!clerk_done || !native_done) &&
           std::chrono::steady_clock::now() < completion_deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    clerk_client.join();
    native_client.join();
    EXPECT_EQ(clerk_response.status, 401);
    EXPECT_EQ(native_response.status, 200);
    server.stop();
    node.stop();
    zetta::connect::set_default_http_transport({}, {});
    zetta::api::clear_clerk_jwks_cache();
}

}  // namespace
