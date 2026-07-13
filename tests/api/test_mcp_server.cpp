// Protocol-layer (loopback HTTP) coverage of the embedded MCP server (#615) —
// the layer-2 journeys of docs/testing/mcp-journeys.md: real JSON-RPC over a
// real socket, with the test thread playing the owning loop (drain()).
// The canonical journey TABLES (journeys_canonical.hpp, #619) replay here
// unchanged over the wire — same data, different transport.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "journey.hpp"
#include "journeys_canonical.hpp"
#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/server.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/version.hpp"

namespace {

// Drift guard for the always-on auth gate (#799): the bootstrap owner's built-in
// `admin` role must authorize EVERY registry op. The capability model is
// exact-match (no wildcards), so a registry op whose (domain, action) is missing
// from the permission vocabulary (permission.cpp) is unreachable for every role —
// which, with auth mandatory, would make that control surface dead in production.
// This most often bites the Orchestration domain. If this fails, add the op's
// (domain, action) to permission_vocabulary().
TEST(AdminRoleCoversRegistry, EveryRegistryOpIsAuthorizedForTheOwner) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.bootstrap_owner("admin-secret").performed);

    auto admin = node.find_role("admin");
    ASSERT_TRUE(admin);
    const zetta::governance::CapabilitySet& caps =
        admin->permissions().capabilities;

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    // admin capabilities carry no TTL, so the reference time is immaterial.
    const std::string now = "2099-01-01T00:00:00.000Z";
    for (const auto& capability : registry.list()) {
        EXPECT_TRUE(caps.permits(capability->required_operation, now))
            << "admin cannot authorize registry op '" << capability->name
            << "' (" << zetta::governance::to_string(
                            capability->required_operation.domain)
            << ":" << capability->required_operation.action
            << ") — add it to permission_vocabulary()";
    }
    node.stop();
}

// A deliberately dumb blocking HTTP/1.1 client: one request, Connection:
// close, returns the response body. Runs on a helper thread while the test
// thread drains the server's bridge (the owning-loop role).
std::string http_post(unsigned short port, const std::string& target,
                      const std::string& body,
                      const std::string& bearer = {}) {
    int socket_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(socket_descriptor,
                  reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
        ::close(socket_descriptor);
        return {};
    }
    std::string request = "POST " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Content-Type: application/json\r\n";
    if (!bearer.empty()) request += "Authorization: Bearer " + bearer + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    ::send(socket_descriptor, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(socket_descriptor, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket_descriptor);
    const auto separator = response.find("\r\n\r\n");
    return separator == std::string::npos ? std::string()
                                          : response.substr(separator + 4);
}

struct McpServerFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    // Auth is always on (#799): seed the owner (admin) so this secret
    // authenticates as it, and every fixture request carries it as the bearer.
    std::string owner_secret = "mcp-owner-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        zetta::api::ServerOptions options;
        options.port = 0;  // ephemeral
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
        ASSERT_NE(server->port(), 0);
    }
    void TearDown() override {
        server->stop();
        node.stop();
    }

    // Issue a request from a helper thread while this (owning) thread drains.
    nlohmann::json rpc(const std::string& payload) {
        std::string body;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            body = http_post(server->port(), "/mcp", payload, owner_secret);
            finished = true;
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            server->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client.join();
        return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    }

    // The layer-2 transport for the shared journey driver: each step becomes
    // a real tools/call over the wire, and the response's structuredContent
    // maps back to the ApiResult shape the driver asserts on — so the SAME
    // journey tables replay here unchanged.
    zetta::test::journey::Caller journey_caller() {
        return [this](const std::string& tool, const nlohmann::json& input) {
            static std::atomic<int> next_id{100};
            const nlohmann::json request = {
                {"jsonrpc", "2.0"},
                {"id", next_id++},
                {"method", "tools/call"},
                {"params", {{"name", tool}, {"arguments", input}}}};
            const nlohmann::json response = rpc(request.dump());
            zetta::api::ApiResult result;
            if (!response.is_object() || !response.contains("result")) {
                result.error = {"protocol_error",
                                response.is_discarded() ? "unparseable response"
                                                        : response.dump(),
                                ""};
                return result;
            }
            const nlohmann::json& structured =
                response.at("result").at("structuredContent");
            result.ok = structured.value("ok", false);
            if (result.ok) {
                result.value = structured.value("value", nlohmann::json::object());
            } else {
                const nlohmann::json error =
                    structured.value("error", nlohmann::json::object());
                result.error = {error.value("code", std::string{}),
                                error.value("message", std::string{}),
                                error.value("path", std::string{})};
            }
            result.trace_id = structured.value("trace_id", std::string{});
            result.span_id = structured.value("span_id", std::string{});
            return result;
        };
    }
};

TEST_F(McpServerFixture, InitializeAndListTools) {
    auto initialized = rpc(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    ASSERT_TRUE(initialized.is_object());
    EXPECT_EQ(initialized["result"]["serverInfo"]["name"], "zetta");
    EXPECT_EQ(initialized["result"]["serverInfo"]["version"], zetta::kVersion);

    auto listed = rpc(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    ASSERT_TRUE(listed["result"]["tools"].is_array());
    EXPECT_GE(listed["result"]["tools"].size(), 6u);
}

TEST_F(McpServerFixture, ToolsCallRunsOnTheOwningLoopAndCarriesTrace) {
    auto called = rpc(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
        R"("params":{"name":"server_info","arguments":{}}})");
    ASSERT_TRUE(called.is_object()) << called.dump();
    const auto& structured = called["result"]["structuredContent"];
    EXPECT_TRUE(structured["ok"].get<bool>());
    EXPECT_EQ(structured["value"]["version"], zetta::kVersion);
    EXPECT_EQ(structured["trace_id"].get<std::string>().size(), 32u);
    EXPECT_FALSE(called["result"]["isError"].get<bool>());
}

TEST_F(McpServerFixture, SqlOverTheWireReadsSystemViews) {
    auto called = rpc(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call",)"
        R"("params":{"name":"sql","arguments":)"
        R"({"statement":"SELECT * FROM system.datasets;",)"
        R"("intent":"protocol-layer journey"}}})");
    const auto& structured = called["result"]["structuredContent"];
    ASSERT_TRUE(structured["ok"].get<bool>()) << called.dump();
    EXPECT_TRUE(structured["value"].contains("rows"));
}

// The canonical dataset-ingest journey (#619) replayed over real JSON-RPC:
// the same table layer 1 runs in-process, here over the wire.
TEST_F(McpServerFixture, JourneyDatasetIngestReplaysOverTheWire) {
    namespace fs = std::filesystem;
    const fs::path file = fs::path(dir.path()) / "accounts.jsonl";
    {
        std::ofstream out(file);
        for (int i = 0; i < 24; ++i) {
            out << R"({"account":")" << i << R"(","mrr":)" << (100 + i) << "}\n";
        }
    }
    const zetta::test::journey::JourneyReport report =
        zetta::test::journey::run_journey(
            zetta::test::journey::canonical::dataset_ingest(
                file.string(), (fs::path(dir.path()) / "nope.xlsx").string()),
            journey_caller());
    EXPECT_TRUE(report.ok) << report.summary();
}

// The canonical gate-blocked journey (#619) over the wire: the refusal and
// its reason ride the JSON-RPC error mapping intact.
TEST_F(McpServerFixture, JourneyGateBlockedReplaysOverTheWire) {
    ASSERT_TRUE(node.create_project("leaky"));
    auto project = node.find_project("leaky");
    ASSERT_TRUE(project);
    ASSERT_TRUE(project->create_graph(
        "leaky graph", zetta::test::journey::canonical::leakage_graph()));

    const zetta::test::journey::JourneyReport report =
        zetta::test::journey::run_journey(
            zetta::test::journey::canonical::gate_blocked(), journey_caller());
    EXPECT_TRUE(report.ok) << report.summary();
}

TEST_F(McpServerFixture, MalformedAndUnknownRequestsFailCleanly) {
    auto parse_error = rpc("this is not json");
    EXPECT_EQ(parse_error["error"]["code"].get<int>(), -32700);

    auto unknown = rpc(R"({"jsonrpc":"2.0","id":5,"method":"no/such"})");
    EXPECT_EQ(unknown["error"]["code"].get<int>(), -32601);
}

TEST(McpServerAuth, ConfiguredTokenIsRequired) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    // Auth is always on (#799): seed the owner from a known secret so that secret
    // authenticates as the owner — there is no legacy shared-secret mode.
    ASSERT_TRUE(node.bootstrap_owner("secret-key-id").performed);
    zetta::api::ServerOptions options;
    options.port = 0;
    zetta::api::ApiServer server(node, nullptr, options);
    ASSERT_TRUE(server.start()) << server.reason();

    std::string denied_body;
    std::string allowed_body;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        denied_body = http_post(
            server.port(), "/mcp",
            R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
        allowed_body = http_post(
            server.port(), "/mcp",
            R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "secret-key-id");
        finished = true;
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();

    EXPECT_NE(denied_body.find("bearer"), std::string::npos);
    auto allowed = nlohmann::json::parse(allowed_body, nullptr, false);
    EXPECT_TRUE(allowed["result"].is_object());

    server.stop();
    node.stop();
}

}  // namespace
