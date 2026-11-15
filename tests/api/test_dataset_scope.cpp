// The per-call dataset scope (#1515, the zetta side of lake#1055;
// docs/governance/dataset-scope.md): a call confined to named datasets runs only
// when its capability declares what it reads and every dataset is in the scope.
// The REST half (the header, its parsing, and that only capability routes answer
// under a scope) is the DatasetScopeRest fixture at the end.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/dataset_scope.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/server.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::DatasetName;
using zetta::api::DatasetScope;
using zetta::api::Registry;

DatasetScope scope_of(const std::string& value) {
    std::string error;
    const auto scope = DatasetScope::parse(value, &error);
    EXPECT_TRUE(scope) << error;
    return scope.value_or(DatasetScope{});
}

TEST(DatasetScope, ParsesBothSpellingsAndDeduplicates) {
    const DatasetScope scope = scope_of("sales.accounts, sales/orders  sales.accounts");
    ASSERT_EQ(scope.datasets().size(), 2U);
    EXPECT_TRUE(scope.contains({"sales", "accounts"}));
    EXPECT_TRUE(scope.contains({"sales", "orders"}));
    EXPECT_FALSE(scope.contains({"sales", "refunds"}));
    EXPECT_FALSE(scope.contains({"crm", "accounts"}));
    EXPECT_EQ(scope.describe(), "sales.accounts,sales.orders");
}

TEST(DatasetScope, AnEmptyValueConfinesToNoDataset) {
    const DatasetScope scope = scope_of("");
    EXPECT_TRUE(scope.datasets().empty());
    EXPECT_FALSE(scope.contains({"sales", "accounts"}));
    EXPECT_EQ(scope.describe(), "no dataset");
}

TEST(DatasetScope, RefusesMalformedEntries) {
    for (const char* value : {"sales", "sales.", ".accounts", "sales.accounts.extra",
                              "sales/accounts/x", "sales.acc'ounts", "sales.*", "*"}) {
        std::string error;
        EXPECT_FALSE(DatasetScope::parse(value, &error)) << value;
        EXPECT_NE(error.find("expected <database>.<table>"), std::string::npos) << value;
    }
}

struct DatasetScopeFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::api::BackgroundActions background;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        for (const char* statement :
             {"CREATE DATABASE sales", "CREATE TABLE sales.accounts (id, owner)",
              "CREATE TABLE sales.refunds (id, amount)",
              "INSERT INTO sales.accounts (id, owner) VALUES (1, 'a')",
              "INSERT INTO sales.refunds (id, amount) VALUES (1, '9')"}) {
            ASSERT_TRUE(call("sql", {{"statement", statement}}).ok) << statement;
        }
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input,
                   std::optional<DatasetScope> scope = std::nullopt) {
        ApiContext context{node, nullptr, {}, trace, &node.span_recorder(), &background};
        context.dataset_scope = std::move(scope);
        return registry.call(name, input, context);
    }
};

TEST_F(DatasetScopeFixture, ASelectInsideTheScopeRuns) {
    const ApiResult result = call("sql", {{"statement", "SELECT * FROM sales.accounts"}},
                                  scope_of("sales.accounts"));
    ASSERT_TRUE(result.ok) << result.error.message;
    EXPECT_EQ(result.value.at("rows").size(), 1U);

    // The `database` default places an unqualified table.
    const ApiResult unqualified = call(
        "sql", {{"statement", "SELECT id FROM accounts"}, {"database", "sales"}},
        scope_of("sales/accounts"));
    EXPECT_TRUE(unqualified.ok) << unqualified.error.message;
}

TEST_F(DatasetScopeFixture, ASelectOutsideTheScopeIsRefusedAndSaysWhatItReads) {
    const ApiResult result = call("sql", {{"statement", "SELECT * FROM sales.refunds"}},
                                  scope_of("sales.accounts"));
    ASSERT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "unauthorized");
    EXPECT_NE(result.error.message.find("sales.refunds"), std::string::npos)
        << result.error.message;
    EXPECT_EQ(result.error.details.at("datasets_read"), nlohmann::json::array({"sales.refunds"}));
    EXPECT_EQ(result.error.details.at("outside"), nlohmann::json::array({"sales.refunds"}));
    EXPECT_EQ(result.error.details.at("dataset_scope"), "sales.accounts");
}

TEST_F(DatasetScopeFixture, AnEmptyScopeRefusesEveryRead) {
    const ApiResult result =
        call("sql", {{"statement", "SELECT * FROM sales.accounts"}}, scope_of(""));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.code, "unauthorized");
}

// Writes, DDL, credential statements, system views and an unplaceable table all
// read or change something that is not a dataset in the scope.
TEST_F(DatasetScopeFixture, AnythingButASelectOfAScopedDatasetIsRefused) {
    const DatasetScope scope = scope_of("sales.accounts");
    for (const char* statement :
         {"INSERT INTO sales.accounts (id, owner) VALUES (2, 'b')",
          "UPDATE sales.accounts SET owner = 'x'", "DELETE FROM sales.accounts WHERE id = 1",
          "DROP TABLE sales.accounts", "CREATE TABLE sales.extra (x)", "CREATE USER e2e_scope",
          "SELECT * FROM system.audit", "SELECT * FROM accounts", "SELECT FROM"}) {
        const ApiResult result = call("sql", {{"statement", statement}}, scope);
        EXPECT_FALSE(result.ok) << statement;
        EXPECT_EQ(result.error.code, "unauthorized") << statement << ": " << result.error.message;
    }
    // Nothing was written.
    const ApiResult rows = call("sql", {{"statement", "SELECT * FROM sales.accounts"}});
    ASSERT_TRUE(rows.ok);
    EXPECT_EQ(rows.value.at("rows").size(), 1U);
}

// A capability that does not declare what it reads cannot run under a scope.
TEST_F(DatasetScopeFixture, ACapabilityThatDeclaresNoReadsIsRefused) {
    const ApiResult unconfined = call("list_projects", nlohmann::json::object());
    EXPECT_TRUE(unconfined.ok) << unconfined.error.message;
    const ApiResult confined =
        call("list_projects", nlohmann::json::object(), scope_of("sales.accounts"));
    ASSERT_FALSE(confined.ok);
    EXPECT_EQ(confined.error.code, "unauthorized");
    EXPECT_NE(confined.error.message.find("does not declare"), std::string::npos);
}

TEST_F(DatasetScopeFixture, ARefusalIsAudited) {
    const std::size_t before = node.audit_events().size();
    const ApiResult result = call("sql", {{"statement", "SELECT * FROM sales.refunds"}},
                                  scope_of("sales.accounts"));
    ASSERT_FALSE(result.ok);
    bool refused = false;
    const auto events = node.audit_events();
    for (std::size_t index = before; index < events.size(); ++index) {
        const auto& value = events[index]->value();
        if (!value.allowed && value.detail.find("outside the dataset scope") != std::string::npos) {
            refused = true;
        }
    }
    EXPECT_TRUE(refused);
}

// #1519: list_datasets lists only the scope's datasets.
TEST_F(DatasetScopeFixture, ListDatasetsListsOnlyTheScope) {
    const auto names = [](const ApiResult& result) {
        std::vector<std::string> out;
        for (const auto& dataset : result.value.at("datasets")) {
            out.push_back(dataset.value("database", std::string{}) + "." +
                          dataset.value("name", std::string{}));
        }
        return out;
    };
    const ApiResult unconfined = call("list_datasets", {{"detail", "summary"}});
    ASSERT_TRUE(unconfined.ok) << unconfined.error.message;
    const auto all = names(unconfined);
    ASSERT_NE(std::ranges::find(all, "sales.refunds"), all.end()) << unconfined.value.dump();

    for (const char* detail : {"summary", "full"}) {
        const ApiResult confined =
            call("list_datasets", {{"detail", detail}}, scope_of("sales.accounts"));
        ASSERT_TRUE(confined.ok) << confined.error.message;
        for (const std::string& name : names(confined)) {
            EXPECT_EQ(name, "sales.accounts") << detail;
        }
        EXPECT_EQ(confined.value.at("datasets").size(), 1U) << detail;
    }
    const ApiResult none = call("list_datasets", nlohmann::json::object(), scope_of(""));
    ASSERT_TRUE(none.ok);
    EXPECT_TRUE(none.value.at("datasets").empty());
}

// #1519: get_dataset reads the one dataset it names, resolved the way the scope
// check resolved it.
TEST_F(DatasetScopeFixture, GetDatasetReadsOnlyTheNamedDataset) {
    const DatasetScope scope = scope_of("sales.accounts");
    const ApiResult dotted = call("get_dataset", {{"dataset", "sales.accounts"}}, scope);
    EXPECT_TRUE(dotted.ok) << dotted.error.message;
    const ApiResult with_database =
        call("get_dataset", {{"dataset", "accounts"}, {"database", "sales"}}, scope);
    EXPECT_TRUE(with_database.ok) << with_database.error.message;

    const ApiResult outside = call("get_dataset", {{"dataset", "sales.refunds"}}, scope);
    ASSERT_FALSE(outside.ok);
    EXPECT_EQ(outside.error.code, "unauthorized");
    EXPECT_EQ(outside.error.details.at("outside"), nlohmann::json::array({"sales.refunds"}));

    // A bare name is looked up across every database, so it cannot be placed.
    const ApiResult bare = call("get_dataset", {{"dataset", "accounts"}}, scope);
    ASSERT_FALSE(bare.ok);
    EXPECT_EQ(bare.error.code, "unauthorized");
}

// Capabilities that read something they cannot bound stay refused (#1519):
// score and explain read the model's entity-key table, analyze_dataset and
// plan_features read same-database joins, and the dataset actions write a dataset.
TEST_F(DatasetScopeFixture, WhatCannotBeBoundedStaysRefused) {
    const DatasetScope scope = scope_of("sales.accounts");
    for (const auto& [name, input] : std::vector<std::pair<std::string, nlohmann::json>>{
             {"score", {{"model", "m"}, {"dataset_path", "sales.accounts"}}},
             {"explain", {{"model", "m"}, {"dataset_path", "sales.accounts"}, {"entity", 1}}},
             {"analyze_dataset", {{"dataset_path", "sales.accounts"}}},
             {"server_info", nlohmann::json::object()}}) {
        const ApiResult result = call(name, input, scope);
        EXPECT_FALSE(result.ok) << name;
        EXPECT_EQ(result.error.code, "unauthorized") << name << ": " << result.error.message;
    }
}

TEST(DatasetScopeRefusal, NamesWhatTheCapabilityReads) {
    zetta::api::Capability capability;
    capability.name = "fixture";
    capability.dataset_reads = [](const nlohmann::json&) {
        return std::optional<std::vector<DatasetName>>(
            std::vector<DatasetName>{{"sales", "accounts"}, {"crm", "leads"}});
    };
    const auto refused = zetta::api::dataset_scope_refusal(capability, nlohmann::json::object(),
                                                           scope_of("sales.accounts"));
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->error.details.at("datasets_read"),
              nlohmann::json::array({"sales.accounts", "crm.leads"}));
    EXPECT_EQ(refused->error.details.at("outside"), nlohmann::json::array({"crm.leads"}));
    EXPECT_FALSE(zetta::api::dataset_scope_refusal(capability, nlohmann::json::object(),
                                                   scope_of("sales.accounts,crm.leads")));
}

// --- Over REST -----------------------------------------------------------------

struct Reply {
    int status = 0;
    std::string body;
};

// One blocking HTTP/1.1 exchange on loopback with the given extra header lines.
Reply exchange(unsigned short port, const std::string& method, const std::string& target,
               const std::string& bearer, const std::string& extra_headers,
               const std::string& body = {}) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    timeval timeout{};
    timeout.tv_sec = 10;
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        return {};
    }
    std::string request = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    request += "Authorization: Bearer " + bearer + "\r\n" + extra_headers;
    if (!body.empty()) request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    [[maybe_unused]] const auto sent = ::send(descriptor, request.data(), request.size(), 0);
    std::string response;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(descriptor, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(descriptor);
    Reply reply;
    if (response.size() > 12) reply.status = std::atoi(response.substr(9, 3).c_str());
    if (const auto separator = response.find("\r\n\r\n"); separator != std::string::npos) {
        reply.body = response.substr(separator + 4);
    }
    return reply;
}

struct DatasetScopeRest : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    std::string owner_secret = "dataset-scope-owner-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        zetta::api::ServerOptions options;
        options.port = 0;
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
        for (const char* statement :
             {"CREATE DATABASE sales", "CREATE TABLE sales.accounts (id, owner)",
              "CREATE TABLE sales.refunds (id, amount)"}) {
            const Reply created = request("POST", "/v1/sql", "",
                                          nlohmann::json{{"statement", statement}}.dump());
            ASSERT_EQ(created.status, 200) << statement << ": " << created.body;
        }
    }
    void TearDown() override {
        server->stop();
        node.stop();
    }

    // Run the exchange on a helper thread while this thread drains the server.
    Reply request(const std::string& method, const std::string& target,
                  const std::string& extra_headers, const std::string& body = {}) {
        Reply reply;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            reply = exchange(server->port(), method, target, owner_secret, extra_headers, body);
            finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            server->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client.join();
        return reply;
    }

    static std::string select_of(const std::string& table) {
        return nlohmann::json{{"statement", "SELECT * FROM " + table}}.dump();
    }
};

TEST_F(DatasetScopeRest, TheHeaderConfinesACapabilityCall) {
    const std::string header = "Zetta-Dataset-Scope: sales/accounts\r\n";
    const Reply inside = request("POST", "/v1/sql", header, select_of("sales.accounts"));
    EXPECT_EQ(inside.status, 200) << inside.body;
    const Reply outside = request("POST", "/v1/sql", header, select_of("sales.refunds"));
    EXPECT_EQ(outside.status, 403) << outside.body;
    const nlohmann::json refused = nlohmann::json::parse(outside.body, nullptr, false);
    ASSERT_TRUE(refused.is_object()) << outside.body;
    EXPECT_EQ(refused["error"]["details"]["outside"], nlohmann::json::array({"sales.refunds"}));
    // The same bearer without the header is not confined: the header only narrows.
    const Reply unconfined = request("POST", "/v1/sql", "", select_of("sales.refunds"));
    EXPECT_EQ(unconfined.status, 200) << unconfined.body;
}

TEST_F(DatasetScopeRest, AMalformedOrRepeatedHeaderIsABadRequest) {
    const Reply malformed = request("POST", "/v1/sql", "Zetta-Dataset-Scope: sales\r\n",
                                    select_of("sales.accounts"));
    EXPECT_EQ(malformed.status, 400) << malformed.body;
    const Reply repeated = request("POST", "/v1/sql",
                                   "Zetta-Dataset-Scope: sales.accounts\r\n"
                                   "Zetta-Dataset-Scope: sales.refunds\r\n",
                                   select_of("sales.accounts"));
    EXPECT_EQ(repeated.status, 400) << repeated.body;
}

// Only registry capability routes answer under a scope: /mcp, the object byte
// routes and the serving routes do not say what they read.
TEST_F(DatasetScopeRest, OnlyCapabilityRoutesAnswerUnderAScope) {
    const std::string header = "Zetta-Dataset-Scope: sales.accounts\r\n";
    const Reply mcp = request(
        "POST", "/mcp", header,
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"sql","arguments":{"statement":"SELECT * FROM sales.refunds"}}})");
    EXPECT_EQ(mcp.status, 403) << mcp.body;
    const Reply object = request("GET", "/v1/objects/bucket/key", header);
    EXPECT_EQ(object.status, 403) << object.body;
    const Reply specs = request("GET", "/v1/specs", header);
    EXPECT_EQ(specs.status, 403) << specs.body;
    // A capability that does not declare its reads answers 403 from the registry.
    const Reply listed = request("GET", "/v1/list_projects", header);
    EXPECT_EQ(listed.status, 403) << listed.body;
}

// The header is read only after authentication: without a valid bearer the
// answer is still 401.
TEST_F(DatasetScopeRest, AnUnauthenticatedScopedRequestIsStill401) {
    Reply reply;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        reply = exchange(server->port(), "POST", "/v1/sql", "not-a-token",
                         "Zetta-Dataset-Scope: sales\r\n", select_of("sales.accounts"));
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        server->drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();
    EXPECT_EQ(reply.status, 401) << reply.body;
}

}  // namespace
