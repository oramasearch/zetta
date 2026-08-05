// The llms.txt progressive-disclosure endpoints (#645,
// docs/api/llms-endpoints.md). Two layers: the zetta-core renderers (content
// is generated from the registry + live node state — the no-drift rule), and
// plain GET over a real socket beside /mcp (auth, content type, 404).
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/llms.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/server.hpp"
#include "zetta/index/index.hpp"
#include "zetta/node.hpp"

namespace {

// --- layer 1: the renderers ---------------------------------------------------

struct LlmsRenderFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    zetta::api::Registry registry;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }
};

TEST_F(LlmsRenderFixture, IndexTeachesTheFirstThreeReadsInOrder) {
    const std::string page = zetta::api::render_llms_index(node, registry);

    // Live node identity is embedded.
    EXPECT_NE(page.find(node.node_id()), std::string::npos);

    // The orientation sequence: server_info -> sql -> list_traces, each with
    // its registry description verbatim (no second prose source).
    const auto server_info = page.find("`server_info`");
    const auto sql = page.find("`sql`", server_info + 1);
    const auto list_traces = page.find("`list_traces`", sql + 1);
    ASSERT_NE(server_info, std::string::npos);
    ASSERT_NE(sql, std::string::npos);
    ASSERT_NE(list_traces, std::string::npos);
    EXPECT_NE(page.find(registry.find("server_info")->description),
              std::string::npos);
    EXPECT_NE(page.find(registry.find("sql")->description), std::string::npos);
    EXPECT_NE(page.find(registry.find("list_traces")->description),
              std::string::npos);

    // A no-MCP agent learns the call shape and where everything lives.
    EXPECT_NE(page.find("POST /mcp"), std::string::npos);
    EXPECT_NE(page.find("tools/call"), std::string::npos);
    for (const auto& topic : zetta::api::llms_topics()) {
        EXPECT_NE(page.find("/llms/" + topic.name + ".txt"), std::string::npos)
            << topic.name;
    }
    EXPECT_NE(page.find("/llms-full.txt"), std::string::npos);
}

TEST_F(LlmsRenderFixture, CapabilitiesTopicRendersEveryRegistryEntry) {
    const auto page =
        zetta::api::render_llms_topic(node, registry, "capabilities");
    ASSERT_TRUE(page.has_value());
    for (const auto& capability : registry.list()) {
        EXPECT_NE(page->find("`" + capability->name + "`"), std::string::npos)
            << capability->name;
        EXPECT_NE(page->find(capability->description), std::string::npos)
            << capability->name;
    }
}

TEST_F(LlmsRenderFixture, ContentUpdatesWhenCapabilitiesAndStateChange) {
    // A newly registered capability appears on the next render — the content
    // is generated from the registry, not a snapshot.
    ASSERT_TRUE(registry.add(zetta::api::Capability{
        "frobnicate_widgets",
        "Frobnicate widgets",
        "Frobnicate every widget on this node. No side effects.",
        nlohmann::json{{"type", "object"}, {"additionalProperties", false}},
        nlohmann::json{{"type", "object"}},
        zetta::api::CapabilityKind::Read,
        zetta::governance::Operation{zetta::governance::CapabilityDomain::Api,
                                     "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        [](const nlohmann::json&, zetta::api::ApiContext&) {
            return zetta::api::ApiResult::success({});
        }}));
    const auto capabilities =
        zetta::api::render_llms_topic(node, registry, "capabilities");
    ASSERT_TRUE(capabilities.has_value());
    EXPECT_NE(capabilities->find("frobnicate_widgets"), std::string::npos);

    // Live state changes show up the same way: a new index, database, project.
    ASSERT_TRUE(node.create_index(zetta::index::IndexValue{
        "customers", zetta::index::IndexKind::Search, "memory_search",
        {"name"}, ""}));
    const auto indexes = zetta::api::render_llms_topic(node, registry, "indexes");
    ASSERT_TRUE(indexes.has_value());
    EXPECT_NE(indexes->find("`customers`"), std::string::npos);
    EXPECT_NE(indexes->find("memory_search"), std::string::npos);

    ASSERT_TRUE(node.create_database("warehouse"));
    const auto data = zetta::api::render_llms_topic(node, registry, "data");
    ASSERT_TRUE(data.has_value());
    EXPECT_NE(data->find("`warehouse`"), std::string::npos);

    ASSERT_TRUE(node.create_project("churn-2026"));
    const auto projects =
        zetta::api::render_llms_topic(node, registry, "projects");
    ASSERT_TRUE(projects.has_value());
    EXPECT_NE(projects->find("`churn-2026`"), std::string::npos);
}

TEST_F(LlmsRenderFixture, UnknownTopicIsRejectedAndFullConcatenatesAll) {
    EXPECT_FALSE(
        zetta::api::render_llms_topic(node, registry, "no-such-topic"));

    const std::string full = zetta::api::render_llms_full(node, registry);
    EXPECT_NE(full.find(node.node_id()), std::string::npos);
    EXPECT_NE(full.find("# Capabilities"), std::string::npos);
    EXPECT_NE(full.find("# Cluster"), std::string::npos);
    EXPECT_NE(full.find("# Data"), std::string::npos);
    EXPECT_NE(full.find("# Indexes"), std::string::npos);
    EXPECT_NE(full.find("# Projects"), std::string::npos);
    EXPECT_NE(full.find("# Observability"), std::string::npos);
}

// --- layer 2: plain GET over a real socket -------------------------------------

// A deliberately dumb blocking HTTP/1.1 GET client: one request, Connection:
// close, returns the whole response (status line + headers + body) so tests
// can assert on the status and content type too.
std::string http_get(unsigned short port, const std::string& target,
                     const std::string& bearer = {}) {
    int socket_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(socket_descriptor, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
        ::close(socket_descriptor);
        return {};
    }
    std::string request = "GET " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    if (!bearer.empty()) request += "Authorization: Bearer " + bearer + "\r\n";
    request += "Connection: close\r\n\r\n";
    ::send(socket_descriptor, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(socket_descriptor, buffer, sizeof(buffer), 0)) >
           0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket_descriptor);
    return response;
}

struct LlmsHttpFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    // Auth is always on (#799): seed the owner (admin) so this secret
    // authenticates as it, and every fixture GET carries it as the bearer.
    std::string owner_secret = "llms-owner-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        zetta::api::ServerOptions options;
        options.port = 0;  // ephemeral
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
    }
    void TearDown() override {
        server->stop();
        node.stop();
    }

    // Issue a GET from a helper thread while this (owning) thread drains. An
    // empty bearer defaults to the seeded owner's secret.
    std::string get(const std::string& target, const std::string& bearer = {}) {
        const std::string effective_bearer =
            bearer.empty() ? owner_secret : bearer;
        std::string response;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            response = http_get(server->port(), target, effective_bearer);
            finished = true;
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            server->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client.join();
        return response;
    }
};

TEST_F(LlmsHttpFixture, OneGetOrientsANoMcpAgent) {
    const std::string response = get("/llms.txt");
    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("text/plain"), std::string::npos);
    EXPECT_NE(response.find(node.node_id()), std::string::npos);
    EXPECT_NE(response.find("`server_info`"), std::string::npos);
    EXPECT_NE(response.find("`sql`"), std::string::npos);
    EXPECT_NE(response.find("`list_traces`"), std::string::npos);
    EXPECT_NE(response.find("POST /mcp"), std::string::npos);
}

TEST_F(LlmsHttpFixture, TopicAndFullPagesAreServed) {
    const std::string capabilities = get("/llms/capabilities.txt");
    EXPECT_NE(capabilities.find("200"), std::string::npos);
    EXPECT_NE(capabilities.find("`create_project`"), std::string::npos);

    const std::string full = get("/llms-full.txt");
    EXPECT_NE(full.find("200"), std::string::npos);
    EXPECT_NE(full.find("# Observability"), std::string::npos);

    const std::string missing = get("/llms/no-such-topic.txt");
    EXPECT_NE(missing.find("404"), std::string::npos);
    EXPECT_NE(missing.find("unknown topic"), std::string::npos);
    EXPECT_NE(missing.find("capabilities"), std::string::npos);
}

// Regression for #700: the beast2 worker reuses one response object across
// requests without clearing its headers, so without the adapter's explicit
// stamping, the FIRST response's Content-Length/ETag would stick to the
// worker and truncate every later, longer body. One worker coroutine forces
// every request through the same reused response object.
TEST(LlmsHttpHeaders, ContentLengthIsCorrectOnEveryResponseOfOneWorker) {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    // Auth is always on (#799): seed the owner so its secret authenticates the
    // three GETs — the header regression is about successful (200) bodies.
    const std::string owner_secret = "llms-headers-owner-secret";
    ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
    zetta::api::ServerOptions options;
    options.port = 0;
    options.worker_coroutines = 1;
    zetta::api::ApiServer server(node, nullptr, options);
    ASSERT_TRUE(server.start()) << server.reason();

    std::vector<std::string> responses;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        responses.push_back(http_get(server.port(), "/llms.txt", owner_secret));
        responses.push_back(
            http_get(server.port(), "/llms-full.txt", owner_secret));
        responses.push_back(http_get(server.port(), "/llms.txt", owner_secret));
        finished = true;
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();

    ASSERT_EQ(responses.size(), 3u);
    for (const std::string& response : responses) {
        const auto header_end = response.find("\r\n\r\n");
        ASSERT_NE(header_end, std::string::npos);
        const auto length_at = response.find("Content-Length: ");
        ASSERT_NE(length_at, std::string::npos);
        ASSERT_LT(length_at, header_end);
        const std::size_t declared = std::stoul(
            response.substr(length_at + 16,
                            response.find("\r\n", length_at) - length_at - 16));
        EXPECT_EQ(declared, response.size() - header_end - 4);
    }
    // The full page really is the longer document, not a truncated copy of
    // the first response.
    EXPECT_GT(responses[1].size(), responses[0].size());
    EXPECT_NE(responses[1].find("# Observability"), std::string::npos);

    server.stop();
    node.stop();
}

TEST(LlmsHttpAuth, LlmsPagesArePublicDiscoveryDocs) {
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

    std::string denied;
    std::string allowed;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        denied = http_get(server.port(), "/llms.txt");
        allowed = http_get(server.port(), "/llms.txt", "secret-key-id");
        finished = true;
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();

    // /llms.txt is a PUBLIC discovery doc — registered before the auth gate so
    // a no-MCP agent can orient without credentials; it serves identically with
    // and without a token.
    EXPECT_NE(denied.find("200"), std::string::npos);
    EXPECT_NE(allowed.find("200"), std::string::npos);
    EXPECT_NE(allowed.find("`server_info`"), std::string::npos);

    server.stop();
    node.stop();
}

}  // namespace
