// The web dashboard's capability dialogs (#827, docs/data/connector-query.md).
//
// The dialog renderer only ever handled FLAT scalar properties, so a capability
// whose input carries a structured object — `create_dataset_from_query`'s
// connector config — had no usable form at all. These cover the nested-object
// rendering and, more importantly, the round trip: a nested form must decode
// back into a nested JSON input the capability actually accepts.
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

#include "temp_dir.hpp"
#include "zetta/api/server.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/node.hpp"

namespace {

struct WebResponse {
    int status = 0;
    std::string headers;
    std::string body;
};

// A blocking HTTP/1.1 client that authenticates the way the dashboard does: the
// `zetta_session` cookie's value IS a bearer token, so a test can present one
// without going through the login form.
WebResponse web_request(unsigned short port, const std::string& method,
                        const std::string& target, const std::string& session,
                        const std::string& form = {}) {
    const int socket_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
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
    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Cookie: zetta_session=" + session + "\r\n";
    if (!form.empty()) {
        request += "Content-Type: application/x-www-form-urlencoded\r\n";
    }
    request += "Content-Length: " + std::to_string(form.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += form;
    ::send(socket_descriptor, request.data(), request.size(), 0);

    std::string raw;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(socket_descriptor, buffer, sizeof(buffer), 0)) > 0) {
        raw.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket_descriptor);

    WebResponse response;
    if (raw.size() > 12) response.status = std::atoi(raw.substr(9, 3).c_str());
    const auto separator = raw.find("\r\n\r\n");
    if (separator != std::string::npos) {
        response.headers = raw.substr(0, separator);
        response.body = raw.substr(separator + 4);
    }
    return response;
}

struct WebUiFormsFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    std::string owner_secret = "web-ui-owner-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        zetta::api::ServerOptions options;
        options.port = 0;
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
    }
    void TearDown() override {
        server->stop();
        node.stop();
    }

    WebResponse request(const std::string& method, const std::string& target,
                        const std::string& form = {}) {
        WebResponse response;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            response = web_request(server->port(), method, target, owner_secret,
                                   form);
            finished = true;
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            server->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client.join();
        return response;
    }
};

TEST_F(WebUiFormsFixture, StoredGraphJsonStaysInsideItsDataElement) {
    const std::string sensitive = "</ScRiPt ><script>inert</script><!--<script>\"&\\u003c";
    const std::string graph_name = "Pipeline " + sensitive;
    const std::string project_name = "Project " + sensitive;
    const auto owner = node.authenticate_token(owner_secret);
    ASSERT_TRUE(owner);
    zetta::api::ApiContext context{node};
    context.acting_key = owner->id();
    context.acting_is_user = true;
    auto& registry = server->registry();
    ASSERT_TRUE(registry.call("create_project", {{"name", project_name}}, context).ok);
    const nlohmann::json nodes = nlohmann::json::array({
        {{"id", "step " + sensitive}, {"kind", "connect"}, {"name", "Step " + sensitive},
         {"attributes", {{sensitive, nlohmann::json::array({sensitive})}}}}});
    const auto saved = registry.call("save_graph",
        {{"project", project_name}, {"graph", graph_name}, {"nodes", nodes},
         {"edges", nlohmann::json::array()}}, context);
    ASSERT_TRUE(saved.ok) << saved.error.message;
    const auto read = registry.call("get_graph",
        {{"project", project_name}, {"graph", graph_name}}, context);
    ASSERT_TRUE(read.ok) << read.error.message;

    const auto page = request("GET", "/?view=pipeline");
    ASSERT_EQ(page.status, 200);
    const std::string marker = "<script id=\"ge-data\" type=\"application/json\">";
    const auto start = page.body.find(marker);
    ASSERT_NE(start, std::string::npos);
    const auto value_start = start + marker.size();
    const auto end = page.body.find("</script>", value_start);
    ASSERT_NE(end, std::string::npos);
    const std::string encoded = page.body.substr(value_start, end - value_start);
    EXPECT_EQ(encoded.find('<'), std::string::npos);
    const auto parsed = nlohmann::json::parse(encoded, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_EQ(parsed.at("graphs").size(), 1);
    EXPECT_EQ(parsed.at("graphs").at(0), read.value);
    EXPECT_EQ(parsed.at("graphs").at(0).at("nodes"), nodes);
    EXPECT_EQ(page.body.find(sensitive), std::string::npos);
}

TEST_F(WebUiFormsFixture, FilePanelEmbedsItsDatabaseAsScriptJson) {
    const auto page = request("GET", "/?view=data&db=example");
    ASSERT_EQ(page.status, 200);
    EXPECT_NE(page.body.find("<script>(function(){var db=\"example\""), std::string::npos);
    const auto invalid = request("GET", "/?view=data&db=%3C%2Fscript%3E");
    ASSERT_EQ(invalid.status, 200);
    EXPECT_EQ(invalid.body.find("<script>(function(){var db="), std::string::npos);
}

TEST_F(WebUiFormsFixture, InstalledWebappDoesNotEmbedStoredGraphData) {
    const std::string sensitive = "</script><script>inert</script>";
    ASSERT_TRUE(node.create_project(sensitive));
    const auto owner = node.authenticate_token(owner_secret);
    ASSERT_TRUE(owner);
    zetta::api::ApiContext context{node};
    context.acting_key = owner->id();
    context.acting_is_user = true;
    const auto saved = server->registry().call("save_graph",
        {{"project", sensitive}, {"graph", sensitive},
         {"nodes", nlohmann::json::array({
             {{"id", "step"}, {"kind", "connect"}, {"name", sensitive}}})},
         {"edges", nlohmann::json::array()}}, context);
    ASSERT_TRUE(saved.ok) << saved.error.message;
    const auto bundle = dir.path() / "webapp";
    std::filesystem::create_directory(bundle);
    const std::string index = "<!doctype html><html><body>Studio fixture</body></html>";
    { std::ofstream output(bundle / "index.html"); output << index; }
    server->stop();
    zetta::api::ServerOptions options;
    options.port = 0;
    options.webapp_dir = bundle.string();
    server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
    ASSERT_TRUE(server->start()) << server->reason();
    const auto root = request("GET", "/?view=pipeline");
    EXPECT_EQ(root.status, 302);
    EXPECT_NE(root.headers.find("/studio/"), std::string::npos);
    EXPECT_EQ(root.body.find(sensitive), std::string::npos);
    const auto app = request("GET", "/studio/pipeline");
    EXPECT_EQ(app.status, 200);
    EXPECT_EQ(app.body, index);
}

TEST_F(WebUiFormsFixture, RendersANestedConnectorGroupWithAMaskedToken) {
    const WebResponse page = request("GET", "/?view=data");
    ASSERT_EQ(page.status, 200) << page.body.substr(0, 300);

    // The nested object became its own labelled group of parent.child controls…
    EXPECT_NE(page.body.find("<legend>connector</legend>"), std::string::npos);
    EXPECT_NE(page.body.find("name=\"connector.type\""), std::string::npos);
    EXPECT_NE(page.body.find("name=\"connector.account\""), std::string::npos);
    EXPECT_NE(page.body.find("name=\"connector.warehouse\""), std::string::npos);
    // …the enum sub-property became a picker, not a free-text box…
    EXPECT_NE(page.body.find("<select name=\"connector.type\">"),
              std::string::npos);
    // …and the credential is masked.
    EXPECT_NE(page.body.find("<input type=\"password\" name=\"connector.token\""),
              std::string::npos);
    // The saved-connection picker is the alternative to filling it in at all.
    EXPECT_NE(page.body.find("<select name=\"connection\">"), std::string::npos);
}

TEST_F(WebUiFormsFixture, ANestedFormDecodesBackIntoANestedInput) {
    const WebResponse posted = request(
        "POST", "/run",
        "cap=create_connection&_return=%2F%3Fview%3Ddata&name=warehouse"
        "&connector.type=snowflake&connector.account=ab12345.us-east-1"
        "&connector.token=the-secret-token&connector.warehouse=COMPUTE_WH");
    // A successful dialog submission redirects back to the view it came from.
    EXPECT_EQ(posted.status, 302) << posted.body.substr(0, 400);

    // The capability got a real nested object, not a flat string.
    const auto saved = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(saved.has_value());
    EXPECT_EQ(saved->config.type, "snowflake");
    EXPECT_EQ(saved->config.account, "ab12345.us-east-1");
    EXPECT_EQ(saved->config.warehouse, "COMPUTE_WH");
    EXPECT_EQ(saved->config.token, "the-secret-token");
}

TEST_F(WebUiFormsFixture, TheSavedConnectionPickerListsNamesAndNeverTheToken) {
    ASSERT_EQ(request("POST", "/run",
                      "cap=create_connection&_return=%2F%3Fview%3Ddata"
                      "&name=warehouse&connector.type=snowflake"
                      "&connector.account=ab12345.us-east-1"
                      "&connector.token=the-secret-token")
                  .status,
              302);

    const WebResponse page = request("GET", "/?view=data");
    ASSERT_EQ(page.status, 200);
    // The picker offers it by name…
    EXPECT_NE(page.body.find("<option value=\"warehouse\">"), std::string::npos);
    // …and nothing on the page carries the credential (the system.connections
    // panel renders the same redacted source).
    EXPECT_EQ(page.body.find("the-secret-token"), std::string::npos);
}

}  // namespace
