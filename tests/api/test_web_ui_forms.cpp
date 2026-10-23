// The web dashboard's capability dialogs (#827, docs/data/connector-query.md),
// and its consent approval surface on Governance -> Consent requests (#999).
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
#include <sys/time.h>
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
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/consent.hpp"
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
                        const std::string& form = {},
                        const std::string& headers = "Origin: http://zetta.example.test:4800\r\n",
                        bool use_session = true, bool keep_alive = false) {
    const int socket_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) return {};
    const timeval receive_timeout{5, 0};
    ::setsockopt(socket_descriptor, SOL_SOCKET, SO_RCVTIMEO,
                 &receive_timeout, sizeof(receive_timeout));
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
    request += "Host: zetta.example.test:4800\r\n";
    if (use_session) request += "Cookie: zetta_session=" + session + "\r\n";
    request += headers;
    if (!form.empty()) {
        request += "Content-Type: application/x-www-form-urlencoded\r\n";
    }
    request += "Content-Length: " + std::to_string(form.size()) + "\r\n";
    request += keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
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
                        const std::string& form = {},
                        const std::string& headers = "Origin: http://zetta.example.test:4800\r\n",
                        bool use_session = true, bool keep_alive = false) {
        return request_as(owner_secret, method, target, form, headers,
                          use_session, keep_alive);
    }

    WebResponse request_as(const std::string& session, const std::string& method,
                           const std::string& target, const std::string& form = {},
                           const std::string& headers = "Origin: http://zetta.example.test:4800\r\n",
                           bool use_session = true, bool keep_alive = false) {
        WebResponse response;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            response = web_request(server->port(), method, target, session,
                                   form, headers, use_session, keep_alive);
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

TEST_F(WebUiFormsFixture, BrowserLoginAndLogoutRequireTheirExactOrigin) {
    for (const std::string path : {"/login", "/session", "/logout", "/session/logout"}) {
        const std::string body = path == "/session"
            ? "{\"token\":\"" + owner_secret + "\"}" : "token=" + owner_secret;
        for (const std::string headers : {
                 "", "Origin: null\r\n", "Origin: http://sibling.example.test:4800\r\n",
                 "Origin: https://zetta.example.test:4800\r\n",
                 "Origin: http://zetta.example.test:4801\r\n",
                 "Origin: http://zetta.example.test:4800/\r\n",
                 "Origin: http://zetta.example.test:4800\\evil.test\r\n",
                 "Origin: http://zetta.example.test:4800%2F.evil.test\r\n",
                 "Origin: http://zetta.example.test:4800.evil.test\r\n",
                 "Origin: http://zetta.example.test:4800\r\nOrigin: http://zetta.example.test:4800\r\n",
                 "Referer: http://zetta.example.test:4800/\r\nSec-Fetch-Site: same-origin\r\n"}) {
            SCOPED_TRACE(path + " " + headers);
            // Login is protected even before a victim has a cookie.
            const auto refused = request("POST", path, body, headers, false);
            EXPECT_EQ(refused.status, 403);
            EXPECT_EQ(refused.headers.find("zetta_session="), std::string::npos);
        }
        const auto accepted = request("POST", path, body);
        EXPECT_EQ(accepted.status, path == "/session" || path == "/session/logout" ? 200 : 302);
        EXPECT_NE(accepted.headers.find("zetta_session="), std::string::npos);
        // Public GETs are allowed, but cannot replay another request's cookie
        // or redirect when a cached worker previously handled login/logout.
        const auto page = request("GET", "/", "", "Sec-Fetch-Site: same-site\r\n", false);
        EXPECT_EQ(page.status, 200);
        EXPECT_EQ(page.headers.find("zetta_session="), std::string::npos);
        EXPECT_EQ(page.headers.find("\r\nLocation:"), std::string::npos);
    }
    const auto secure = request("POST", "/login", "token=" + owner_secret,
        "Origin: https://zetta.example.test:4800\r\nX-Forwarded-Proto: https\r\n", false);
    EXPECT_EQ(secure.status, 302);
    EXPECT_NE(secure.headers.find("; Secure"), std::string::npos);
}

TEST_F(WebUiFormsFixture, OriginRefusalsCloseConnectionsWithUnreadBodies) {
    const std::string form = "token=" + owner_secret;
    const std::string forged =
        "POST /login HTTP/1.1\r\nHost: zetta.example.test:4800\r\n"
        "Origin: http://zetta.example.test:4800\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
        std::to_string(form.size()) + "\r\nConnection: close\r\n\r\n" + form;
    const auto refused = request("POST", "/login", forged,
        "Origin: http://sibling.example.test:4800\r\n", false, true);
    EXPECT_EQ(refused.status, 403);
    EXPECT_NE(refused.headers.find("\r\nConnection: close"), std::string::npos);
    EXPECT_EQ(refused.headers.find("zetta_session="), std::string::npos);
    EXPECT_EQ(refused.body.find("HTTP/1.1"), std::string::npos);
    EXPECT_EQ(refused.body.find("zetta_session="), std::string::npos);
}

TEST_F(WebUiFormsFixture, EveryBrowserActionRefusesSiblingAndMissingOrigins) {
    for (const std::string path : {"/run", "/assist", "/shell", "/graph-save", "/blob", "/blob/delete"}) {
        for (const std::string headers : {"", "Origin: http://sibling.example.test:4800\r\n"}) {
            SCOPED_TRACE(path + " " + headers);
            const auto refused = request("POST", path, "cap=create_project&name=forged", headers);
            EXPECT_EQ(refused.status, 403);
            EXPECT_NE(refused.body.find("same-origin browser request required"), std::string::npos);
        }
    }
    EXPECT_FALSE(node.find_project("forged"));
    const auto accepted = request("POST", "/run", "cap=create_project&name=trusted&_return=%2F%3Fview%3Dprojects");
    EXPECT_EQ(accepted.status, 302) << accepted.body;
    EXPECT_TRUE(node.find_project("trusted"));
}

TEST_F(WebUiFormsFixture, CookieRestAndMcpCannotDispatchCrossOriginActions) {
    const std::string action = R"({"name":"forged"})";
    const std::string rpc = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"create_project","arguments":{"name":"forged"}}})";
    for (const std::string headers : {"", "Origin: http://sibling.example.test:4800\r\n"}) {
        EXPECT_EQ(request("POST", "/v1/create_project", action, headers).status, 403);
        EXPECT_EQ(request("POST", "/mcp", rpc, headers).status, 403);
        EXPECT_EQ(request("PUT", "/v1/objects/bucket/key", "bytes", headers).status, 403);
        EXPECT_EQ(request("PATCH", "/v1/objects/bucket/key", "bytes", headers).status, 403);
        EXPECT_EQ(request("DELETE", "/v1/objects/bucket/key", "", headers).status, 403);
    }
    EXPECT_FALSE(node.find_project("forged"));
    EXPECT_EQ(request("POST", "/v1/create_project", R"({"name":"trusted-rest"})").status, 200);
    EXPECT_EQ(request("POST", "/mcp", rpc).status, 200);
    EXPECT_TRUE(node.find_project("trusted-rest"));
    EXPECT_TRUE(node.find_project("forged"));
}

TEST_F(WebUiFormsFixture, CookieReadsNeedUnambiguousOriginEvidence) {
    for (const std::string headers : {
             "", "Sec-Fetch-Site: same-site\r\n", "Sec-Fetch-Site: cross-site\r\n",
             "Referer: http://sibling.example.test:4800/page\r\n",
             "Referer: http://zetta.example.test:4800@evil.test/page\r\n",
             "Referer: http://zetta.example.test:4800.evil.test/page\r\n",
             "Referer: http://zetta.example.test:4800/\r\nReferer: http://zetta.example.test:4800/\r\n",
             "Sec-Fetch-Site: \r\nReferer: http://zetta.example.test:4800/\r\n",
             "Sec-Fetch-Site: same-origin\r\nSec-Fetch-Site: same-origin\r\n",
             "Origin: null\r\nSec-Fetch-Site: same-origin\r\n",
             "Origin: http://sibling.example.test:4800\r\nReferer: http://zetta.example.test:4800/\r\n",
             "Referer: http://zetta.example.test:4800/\r\nSec-Fetch-Site: cross-site\r\n"}) {
        SCOPED_TRACE(headers);
        EXPECT_EQ(request("GET", "/v1/server_info", "", headers).status, 403);
    }
    for (const std::string headers : {
             "Sec-Fetch-Site: same-origin\r\n", "Sec-Fetch-Site: none\r\n",
             "Referer: http://zetta.example.test:4800/studio/?view=data\r\n",
             "Origin: http://zetta.example.test:4800\r\n"}) {
        SCOPED_TRACE(headers);
        EXPECT_EQ(request("GET", "/v1/server_info", "", headers).status, 200);
    }
    EXPECT_EQ(request("GET", "/v1/server_info", "",
        "oRiGiN: http://zetta.example.test:4800\r\nX-Forwarded-Host: sibling.example.test\r\n").status, 200);
}

TEST_F(WebUiFormsFixture, CookieGetSqlChecksTheEntireParsedBatchBeforeExecution) {
    for (const std::string sql : {
             "CREATE+DATABASE+forged", "SELECT+name+FROM+system.users%3BCREATE+DATABASE+forged",
             "%2F*comment*%2FCREATE+DATABASE+forged", "CREATE+USER+forged"}) {
        const auto refused = request("GET", "/v1/sql?statement=" + sql, "", "Sec-Fetch-Site: same-origin\r\n");
        EXPECT_EQ(refused.status, 403) << refused.body;
    }
    EXPECT_FALSE(node.find_database("forged"));
    EXPECT_FALSE(node.find_user("forged"));
    const auto read = request("GET", "/v1/sql?statement=SELECT+name+FROM+system.users", "",
                              "Sec-Fetch-Site: same-origin\r\n");
    EXPECT_EQ(read.status, 200) << read.body;
    EXPECT_EQ(request("POST", "/v1/sql", R"({"statement":"CREATE DATABASE trusted_post"})").status, 200);
    EXPECT_TRUE(node.find_database("trusted_post"));
}

TEST_F(WebUiFormsFixture, ExplicitBearersKeepTheirProgrammaticContract) {
    const std::string bearer = "Authorization: Bearer " + owner_secret + "\r\n";
    EXPECT_EQ(request("POST", "/v1/create_project", R"({"name":"bearer-project"})", bearer, false).status, 200);
    EXPECT_EQ(request("GET", "/v1/sql?statement=CREATE+DATABASE+bearer_db", "", bearer, false).status, 200);
    EXPECT_TRUE(node.find_database("bearer_db"));
    EXPECT_EQ(request("POST", "/v1/create_project", R"({"name":"invalid-bearer"})",
                      "Authorization: Bearer invalid\r\n").status, 401);
    EXPECT_FALSE(node.find_project("invalid-bearer"));
    // A bearer header cannot bypass origin binding on the browser login path.
    EXPECT_EQ(request("POST", "/login", "token=" + owner_secret, bearer, false).status, 403);
}

// --- Governance -> Consent requests: the web approval surface (#999) ----------
//
// The page lists each pending challenge with Approve / Deny forms that POST
// /run as approve_consent / deny_consent, so the engine authorizes the signed-in
// user, refuses the requester's own approval, and audits the decision.
struct WebConsentFixture : WebUiFormsFixture {
    std::string owner_id;
    std::string approver_id;
    std::string approver_secret;
    std::string viewer_secret;

    void SetUp() override {
        WebUiFormsFixture::SetUp();
        owner_id = node.find_user("owner")->id();
        const auto user_with = [&](const std::string& name, const std::string& role,
                                   std::string& secret) {
            auto user = node.create_user(name);
            EXPECT_TRUE(user);
            node.assign_role(user->id(), node.find_role(role)->id());
            secret = node.create_token(user->id(), zetta::governance::TokenType::Access)
                         .secret;
            return user->id();
        };
        approver_id = user_with("approver", "admin", approver_secret);
        user_with("viewer", "reader", viewer_secret);
    }

    // A pending challenge whose frozen action is harmless to execute
    // (server_info), requested by `requester` as a user.
    std::string park_challenge(const std::string& requester) {
        return node.consent_ledger()
            .create("server_info", nlohmann::json::object(),
                    {"Read this node's identity (a test action)", {}, {"server_info"}},
                    requester, "", /*requested_is_user=*/true)
            ->id;
    }
};

TEST_F(WebConsentFixture, TheConsentTabListsAPendingChallengeWithApproveAndDeny) {
    const std::string id = park_challenge(approver_id);
    const WebResponse page = request("GET", "/?view=governance&tab=consent");
    ASSERT_EQ(page.status, 200) << page.body.substr(0, 300);
    EXPECT_NE(page.body.find("data-challenge=\"" + id + "\""), std::string::npos);
    EXPECT_NE(page.body.find("Read this node&#39;s identity"), std::string::npos)
        << "the card names what would run";
    EXPECT_NE(page.body.find("name=\"cap\" value=\"approve_consent\""), std::string::npos);
    EXPECT_NE(page.body.find("name=\"cap\" value=\"deny_consent\""), std::string::npos);
    EXPECT_NE(page.body.find("name=\"challenge_id\" value=\"" + id + "\""),
              std::string::npos);
}

TEST_F(WebConsentFixture, TheRequesterIsOfferedWithdrawNotApprove) {
    park_challenge(owner_id);
    const WebResponse page = request("GET", "/?view=governance&tab=consent");
    ASSERT_EQ(page.status, 200);
    EXPECT_NE(page.body.find("You requested this"), std::string::npos);
    EXPECT_EQ(page.body.find("name=\"cap\" value=\"approve_consent\""), std::string::npos);
    EXPECT_NE(page.body.find("Withdraw"), std::string::npos);
}

TEST_F(WebConsentFixture, TheRequesterCannotApproveItsOwnRequestByPostingTheForm) {
    const std::string id = park_challenge(owner_id);
    const WebResponse posted = request(
        "POST", "/run",
        "cap=approve_consent&_return=%2F%3Fview%3Dgovernance%26tab%3Dconsent"
        "&challenge_id=" + id);
    EXPECT_EQ(posted.status, 200) << "a refusal renders, it does not redirect";
    EXPECT_NE(posted.body.find("unauthorized"), std::string::npos);
    EXPECT_EQ(node.consent_ledger().find(id)->status,
              zetta::governance::ConsentStatus::Pending);
}

TEST_F(WebConsentFixture, AUserWithoutTheApproverRightCannotApprove) {
    const std::string id = park_challenge(owner_id);
    const WebResponse posted = request_as(
        viewer_secret, "POST", "/run",
        "cap=approve_consent&_return=%2F%3Fview%3Dgovernance%26tab%3Dconsent"
        "&challenge_id=" + id);
    EXPECT_NE(posted.body.find("unauthorized"), std::string::npos);
    EXPECT_EQ(node.consent_ledger().find(id)->status,
              zetta::governance::ConsentStatus::Pending);
}

TEST_F(WebConsentFixture, ADifferentApproverApprovesAndTheDecisionIsAudited) {
    const std::string id = park_challenge(owner_id);
    const WebResponse posted = request_as(
        approver_secret, "POST", "/run",
        "cap=approve_consent&_return=%2F%3Fview%3Dgovernance%26tab%3Dconsent"
        "&challenge_id=" + id);
    ASSERT_EQ(posted.status, 302) << posted.body.substr(0, 600);
    EXPECT_NE(posted.headers.find("/?view=governance&tab=consent"), std::string::npos);
    const auto challenge = node.consent_ledger().find(id);
    ASSERT_TRUE(challenge);
    EXPECT_EQ(challenge->status, zetta::governance::ConsentStatus::Executed)
        << challenge->detail;
    EXPECT_EQ(challenge->decided_by, approver_id);
    bool audited = false;
    for (const auto& event : node.audit_events()) {
        if (event->value().action == zetta::governance::AuditAction::ConsentDecided &&
            event->value().actor == approver_id && event->value().allowed) {
            audited = true;
        }
    }
    EXPECT_TRUE(audited);

    // Decided, it leaves the pending list.
    const WebResponse page = request("GET", "/?view=governance&tab=consent");
    EXPECT_EQ(page.body.find("data-challenge=\"" + id + "\""), std::string::npos);
}

TEST_F(WebConsentFixture, DenyRefusesTheChallengeAndTheActionNeverRuns) {
    const std::string id = park_challenge(owner_id);
    const WebResponse posted = request_as(
        approver_secret, "POST", "/run",
        "cap=deny_consent&_return=%2F%3Fview%3Dgovernance%26tab%3Dconsent"
        "&challenge_id=" + id + "&reason=not+today");
    ASSERT_EQ(posted.status, 302) << posted.body.substr(0, 600);
    const auto challenge = node.consent_ledger().find(id);
    EXPECT_EQ(challenge->status, zetta::governance::ConsentStatus::Denied);
    EXPECT_EQ(challenge->detail, "not today");
}

}  // namespace
