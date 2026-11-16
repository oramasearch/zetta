// SPDX-License-Identifier: MIT

// zetta#1389: the web UI's cookie blob routes check what the session
// user may do, not only that the cookie resolves to a user.
//
// POST /blob, POST /blob/delete and GET /blob/download are registered before the
// main gate, so they used to accept any valid native token (an app credential, a
// read-only user) and upload, read or delete large objects in any database on the
// node. Now upload and delete need sql:write on the database and download needs
// blob:read covering the object id (what the governed serve_blob path requires),
// and every decision is audited.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/server.hpp"
#include "zetta/data/database.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/node.hpp"

namespace {

struct WebResponse {
    int status = 0;
    std::string body;
};

// A blocking HTTP/1.1 request carrying `session` as the zetta_session cookie, with
// `body` sent raw (the dashboard posts a file's bytes as the request body).
WebResponse web_request(unsigned short port, const std::string& method, const std::string& target,
                        const std::string& session, const std::string& body) {
    const int socket_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(socket_descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_descriptor);
        return {};
    }
    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Cookie: zetta_session=" + session + "\r\n";
    request += "Origin: http://127.0.0.1\r\n";
    request += "Content-Type: application/octet-stream\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
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
    if (separator != std::string::npos) response.body = raw.substr(separator + 4);
    return response;
}

struct WebBlobAuthz : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    const std::string owner_secret = "web-blob-owner-secret";
    std::string alpha_object;  // a large object in "alpha"
    std::string beta_object;   // a large object in "beta"
    std::string app_token;     // sql:write ON DATABASE alpha + blob:read on alpha_object
    std::string reader_token;  // the built-in reader: sql:read, no blob or write right

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
        auto alpha = node.create_database("alpha");
        auto beta = node.create_database("beta");
        ASSERT_TRUE(alpha && beta);
        const auto stored_alpha = alpha->large_objects().import_bytes("a.txt", "alpha bytes");
        const auto stored_beta = beta->large_objects().import_bytes("b.txt", "beta secret bytes");
        ASSERT_TRUE(stored_alpha && stored_beta);
        alpha_object = stored_alpha->id;
        beta_object = stored_beta->id;

        // A narrowly scoped app credential: an ordinary user + token, as
        // network/app_credential.cpp makes one.
        ASSERT_TRUE(node.create_role("alpha_app"));
        zetta::governance::Resource alpha_database;
        alpha_database.kind = zetta::governance::ResourceKind::Database;
        alpha_database.name = "alpha";
        ASSERT_TRUE(node.grant_scoped_permissions("alpha_app", {"sql:write"}, alpha_database));
        ASSERT_TRUE(node.grant_scoped_permissions("alpha_app", {"blob:read"},
                                                  zetta::governance::Resource::blob(alpha_object)));
        app_token = token_for("app-connection:alpha", "alpha_app");
        node.ensure_builtin_roles();
        reader_token = token_for("read-only", "reader");

        zetta::api::ServerOptions options;
        options.port = 0;
        server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
        ASSERT_TRUE(server->start()) << server->reason();
    }
    void TearDown() override {
        server->stop();
        node.stop();
    }

    std::string token_for(const std::string& user_name, const std::string& role_name) {
        auto user = node.create_user(user_name);
        auto role = node.find_role(role_name);
        EXPECT_TRUE(user && role);
        if (!user || !role) return {};
        node.assign_role(user->id(), role->id());
        return node.create_token(user->id(), zetta::governance::TokenType::Api).secret;
    }

    WebResponse request(const std::string& method, const std::string& target,
                        const std::string& session, const std::string& body = {}) {
        WebResponse response;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            response = web_request(server->port(), method, target, session, body);
            finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!finished && std::chrono::steady_clock::now() < deadline) {
            server->drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        client.join();
        return response;
    }

    std::size_t objects_in(const std::string& db) {
        return node.find_database(db)->large_objects().list().size();
    }

    // Whether an audit event records a refused blob route for `target`.
    bool refusal_audited(const std::string& verb, const std::string& target) {
        const auto events = node.audit_events();
        return std::any_of(events.begin(), events.end(), [&](const auto& event) {
            const auto& value = event->value();
            return !value.allowed && value.target == "web:blob_" + verb + ":" + target;
        });
    }
};

TEST_F(WebBlobAuthz, AScopedAppTokenCannotUploadOutsideItsDatabase) {
    const WebResponse refused = request("POST", "/blob?db=beta&name=planted.txt", app_token, "x");
    EXPECT_NE(refused.body.find("permission denied"), std::string::npos) << refused.body;
    EXPECT_EQ(objects_in("beta"), 1u);
    EXPECT_TRUE(refusal_audited("upload", "beta/planted.txt"));

    const WebResponse stored = request("POST", "/blob?db=alpha&name=new.txt", app_token, "new");
    EXPECT_NE(stored.body.find("\"ok\":true"), std::string::npos) << stored.body;
    EXPECT_EQ(objects_in("alpha"), 2u);
}

TEST_F(WebBlobAuthz, AScopedAppTokenCannotDownloadAnObjectOutsideItsGrant) {
    const WebResponse refused =
        request("GET", "/blob/download?db=beta&id=" + beta_object, app_token);
    EXPECT_EQ(refused.status, 403) << refused.body;
    EXPECT_EQ(refused.body.find("beta secret bytes"), std::string::npos);
    EXPECT_TRUE(refusal_audited("download", "beta/" + beta_object));

    const WebResponse served =
        request("GET", "/blob/download?db=alpha&id=" + alpha_object, app_token);
    EXPECT_EQ(served.status, 200) << served.body;
    EXPECT_EQ(served.body, "alpha bytes");
}

TEST_F(WebBlobAuthz, AScopedAppTokenCannotDeleteOutsideItsDatabase) {
    const WebResponse refused =
        request("POST", "/blob/delete?db=beta&id=" + beta_object, app_token);
    EXPECT_NE(refused.body.find("permission denied"), std::string::npos) << refused.body;
    EXPECT_TRUE(node.find_database("beta")->large_objects().read(beta_object).has_value());
    EXPECT_TRUE(refusal_audited("delete", "beta/" + beta_object));

    const WebResponse deleted =
        request("POST", "/blob/delete?db=alpha&id=" + alpha_object, app_token);
    EXPECT_NE(deleted.body.find("\"ok\":true"), std::string::npos) << deleted.body;
    EXPECT_EQ(objects_in("alpha"), 0u);
}

TEST_F(WebBlobAuthz, AReadOnlyUserCanNeitherWriteNorReadBlobs) {
    EXPECT_NE(request("POST", "/blob?db=alpha&name=r.txt", reader_token, "x")
                  .body.find("permission denied"),
              std::string::npos);
    EXPECT_EQ(request("GET", "/blob/download?db=alpha&id=" + alpha_object, reader_token).status,
              403);
    EXPECT_NE(request("POST", "/blob/delete?db=alpha&id=" + alpha_object, reader_token)
                  .body.find("permission denied"),
              std::string::npos);
    EXPECT_EQ(objects_in("alpha"), 1u);
}

TEST_F(WebBlobAuthz, TheOwnerStillUploadsDownloadsAndDeletes) {
    EXPECT_NE(request("POST", "/blob?db=beta&name=o.txt", owner_secret, "owner")
                  .body.find("\"ok\":true"),
              std::string::npos);
    const WebResponse served =
        request("GET", "/blob/download?db=beta&id=" + beta_object, owner_secret);
    EXPECT_EQ(served.status, 200);
    EXPECT_EQ(served.body, "beta secret bytes");
    EXPECT_NE(request("POST", "/blob/delete?db=beta&id=" + beta_object, owner_secret)
                  .body.find("\"ok\":true"),
              std::string::npos);
}

TEST_F(WebBlobAuthz, NoSessionIsStillUnauthorized) {
    EXPECT_EQ(request("GET", "/blob/download?db=alpha&id=" + alpha_object, "not-a-token").status,
              401);
    EXPECT_NE(request("POST", "/blob?db=alpha&name=n.txt", "not-a-token", "x")
                  .body.find("unauthorized"),
              std::string::npos);
}

}  // namespace
