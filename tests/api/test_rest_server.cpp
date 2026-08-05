// Protocol-layer (loopback HTTP) coverage of the REST rendering (#618,
// docs/api/rest-openapi.md): real GETs and POSTs against /v1/<name> — the
// curl surface — with the test thread playing the owning loop (drain()), the
// same layer-2 discipline as test_mcp_server.cpp.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "index/term_frequency_backend.hpp"
#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/api/server.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/schemas/registry.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_step_executor.hpp"  // read_index_partitions
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/version.hpp"

namespace {

struct HttpResponse {
    int status = 0;
    std::string body;
};

// A deliberately dumb blocking HTTP/1.1 client: one request, Connection:
// close. Runs on a helper thread while the test thread drains the server's
// bridge (the owning-loop role).
HttpResponse http_request(unsigned short port, const std::string& method,
                          const std::string& target, const std::string& body,
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
    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    if (!body.empty()) request += "Content-Type: application/json\r\n";
    if (!bearer.empty()) request += "Authorization: Bearer " + bearer + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    ::send(socket_descriptor, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = ::recv(socket_descriptor, buffer, sizeof(buffer), 0)) >
           0) {
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(socket_descriptor);

    HttpResponse parsed;
    if (response.size() > 12) {
        parsed.status = std::atoi(response.substr(9, 3).c_str());
    }
    const auto separator = response.find("\r\n\r\n");
    if (separator != std::string::npos) {
        parsed.body = response.substr(separator + 4);
    }
    return parsed;
}

struct RestServerFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    // Auth is always on (#799): seed the owner (admin) so this secret
    // authenticates as it; request() sends it by default so every fixture call
    // is a successful, authorized admin call.
    std::string owner_secret = "rest-owner-secret";

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
    // An empty bearer defaults to the seeded owner's secret; an explicit bearer
    // (e.g. a lower-privilege user's token) overrides it.
    HttpResponse request(const std::string& method, const std::string& target,
                         const std::string& body = {},
                         const std::string& bearer = {}) {
        const std::string effective_bearer =
            bearer.empty() ? owner_secret : bearer;
        HttpResponse response;
        std::atomic<bool> finished{false};
        std::thread client([&] {
            response = http_request(server->port(), method, target, body,
                                    effective_bearer);
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

    nlohmann::json json_of(const HttpResponse& response) {
        return nlohmann::json::parse(response.body, nullptr,
                                     /*allow_exceptions=*/false);
    }
};

// The read journey over plain GETs — what a curl-speaking integration runs
// (#618 acceptance): orient with server_info, read a system.* view through
// sql (query-string input, URL-encoded), then the typed dataset reads — every
// call the same governed Registry::call path as MCP.
TEST_F(RestServerFixture, ReadJourneyOverPlainGets) {
    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("accounts");
    ASSERT_TRUE(table);
    table->add_row({{"user", "u1"}, {"plan", "pro"}});
    table->add_row({{"user", "u2"}, {"plan", "free"}});

    auto info = request("GET", "/v1/server_info");
    EXPECT_EQ(info.status, 200);
    auto info_body = json_of(info);
    ASSERT_TRUE(info_body.is_object()) << info.body;
    EXPECT_TRUE(info_body.at("ok").get<bool>());
    EXPECT_EQ(info_body.at("value").at("version"), zetta::kVersion);
    EXPECT_EQ(info_body.at("trace_id").get<std::string>().size(), 32u);

    auto rows = request(
        "GET", "/v1/sql?statement=SELECT%20*%20FROM%20system.datasets;"
               "&intent=rest%20read%20journey");
    EXPECT_EQ(rows.status, 200);
    auto rows_body = json_of(rows);
    ASSERT_TRUE(rows_body.at("ok").get<bool>()) << rows.body;
    ASSERT_EQ(rows_body.at("value").at("rows").size(), 1u);
    EXPECT_EQ(rows_body.at("value").at("rows")[0].at("dataset"), "accounts");

    auto dataset = request("GET", "/v1/get_dataset?dataset=accounts");
    EXPECT_EQ(dataset.status, 200);
    auto dataset_body = json_of(dataset);
    ASSERT_TRUE(dataset_body.at("ok").get<bool>()) << dataset.body;
    EXPECT_EQ(dataset_body.at("value").at("rows"), 2u);
    EXPECT_EQ(dataset_body.at("value").at("database"), "data");

    // list_traces with a coerced integer query parameter.
    auto traces = request("GET", "/v1/list_traces?limit=5");
    EXPECT_EQ(traces.status, 200);
    EXPECT_TRUE(json_of(traces).at("ok").get<bool>()) << traces.body;
}

// The action rendering: POST a JSON body, read the mutation back over GET —
// the journey discipline over the wire.
TEST_F(RestServerFixture, PostActionThenReadBack) {
    auto created = request(
        "POST", "/v1/create_project",
        R"({"name":"churn-rest","intent":"rest action journey"})");
    EXPECT_EQ(created.status, 200);
    auto created_body = json_of(created);
    ASSERT_TRUE(created_body.at("ok").get<bool>()) << created.body;
    EXPECT_EQ(created_body.at("value").at("name"), "churn-rest");

    auto listed = request("GET", "/v1/list_projects");
    EXPECT_EQ(listed.status, 200);
    const auto projects = json_of(listed).at("value").at("projects");
    EXPECT_NE(std::find(projects.begin(), projects.end(), "churn-rest"),
              projects.end());

    auto detail = request("GET", "/v1/get_project?name=churn-rest");
    EXPECT_EQ(detail.status, 200);
    EXPECT_EQ(json_of(detail).at("value").at("name"), "churn-rest");

    // The duplicate is a domain failure: 422 with the engine's reason.
    auto duplicate = request("POST", "/v1/create_project",
                             R"({"name":"churn-rest"})");
    EXPECT_EQ(duplicate.status, 422);
    auto duplicate_body = json_of(duplicate);
    EXPECT_FALSE(duplicate_body.at("ok").get<bool>());
    EXPECT_EQ(duplicate_body.at("error").at("code"), "engine_error");
}

// The error mapping over the wire: schema-invalid input -> 400 with the
// path-qualified reason; a miss -> 404; a malformed body -> 400.
TEST_F(RestServerFixture, ErrorsMapToStatuses) {
    auto invalid = request("GET", "/v1/list_traces?limit=not-a-number");
    EXPECT_EQ(invalid.status, 400);
    auto invalid_body = json_of(invalid);
    EXPECT_EQ(invalid_body.at("error").at("code"), "invalid_input");
    EXPECT_NE(invalid_body.at("error").at("message")
                  .get<std::string>().find("limit"),
              std::string::npos);

    auto missing = request("GET", "/v1/get_run?run_id=no-such-run");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(json_of(missing).at("error").at("code"), "not_found");

    auto malformed = request("POST", "/v1/create_project", "this is not json");
    EXPECT_EQ(malformed.status, 400);
    EXPECT_EQ(json_of(malformed).at("error").at("code"), "invalid_input");
}

// GET /v1/openapi.json serves exactly what the generator produces from the
// server's registry — the served document and the drift-checked artifact
// share one source of truth.
TEST_F(RestServerFixture, OpenApiDocumentIsServed) {
    auto served = request("GET", "/v1/openapi.json");
    EXPECT_EQ(served.status, 200);
    const nlohmann::json document = json_of(served);
    ASSERT_TRUE(document.is_object()) << served.body;
    EXPECT_EQ(document, zetta::api::openapi_document(server->registry()));
    EXPECT_TRUE(document.at("paths").contains("/v1/server_info"));
}

// The spec index over the wire (#646, docs/api/specs-index.md): GET /v1/specs
// serves the index, /v1/specs/<name> the raw document (a boundary schema
// straight from its accessor; "openapi" the #618 document), and an unknown
// name is a 404 that points back at the index.
TEST_F(RestServerFixture, SpecIndexIsServed) {
    auto index = request("GET", "/v1/specs");
    EXPECT_EQ(index.status, 200);
    const nlohmann::json index_document = json_of(index);
    ASSERT_TRUE(index_document.is_object()) << index.body;
    EXPECT_EQ(index_document,
              zetta::api::spec_index(server->registry()));
    EXPECT_GE(index_document.at("count").get<std::size_t>(), 55u);

    auto run_schema = request("GET", "/v1/specs/run");
    EXPECT_EQ(run_schema.status, 200);
    EXPECT_EQ(json_of(run_schema),
              zetta::schemas::find_boundary_schema("run")->schema());

    auto input_schema = request("GET", "/v1/specs/sql.input");
    EXPECT_EQ(input_schema.status, 200);
    EXPECT_EQ(json_of(input_schema),
              server->registry().find("sql")->input_schema);

    auto openapi = request("GET", "/v1/specs/openapi");
    EXPECT_EQ(openapi.status, 200);
    EXPECT_EQ(json_of(openapi).at("openapi"), "3.1.0");

    auto missing = request("GET", "/v1/specs/no-such-spec");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(json_of(missing).at("error").at("code"), "not_found");
}

TEST(RestServerAuth, OpenApiIsAPublicDiscoveryDoc) {
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

    HttpResponse denied;
    HttpResponse wrong;
    HttpResponse allowed;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        denied = http_request(server.port(), "GET", "/v1/openapi.json", "");
        wrong = http_request(server.port(), "GET", "/v1/openapi.json", "",
                             "wrong-token");
        allowed = http_request(server.port(), "GET", "/v1/openapi.json", "",
                               "secret-key-id");
        finished = true;
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        server.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    client.join();

    // /v1/openapi.json is a PUBLIC discovery doc — registered before the auth
    // gate (like /llms.txt and /mcp.md): it serves with no token, and a wrong
    // token doesn't matter on a public route. The gated /v1/* data routes keep
    // their 401s (test_auth_gate.cpp).
    EXPECT_EQ(denied.status, 200);
    EXPECT_EQ(wrong.status, 200);
    EXPECT_EQ(allowed.status, 200);

    server.stop();
    node.stop();
}

// The #648 acceptance over the wire — the curl surface: expose a trained
// model, POST /v1/models/{name}/predict with a key -> 200 with the ranked
// list; a key without data:predict -> 403; un-expose -> the route answers
// 404. Same for an index over GET /v1/indexes/{name}/search. (The full
// allow/deny/read-back journeys live in test_serving.cpp; this is the
// protocol layer.)
TEST_F(RestServerFixture, ServedModelAndIndexEndpointsOverHttp) {
    // A trained model, registered + exposed.
    std::vector<std::vector<double>> columns(2);
    std::vector<double> labels;
    for (std::size_t i = 0; i < 30; ++i) {
        const bool positive = i >= 15;
        columns[0].push_back(positive ? 0.9 : 0.1);
        columns[1].push_back(0.5);
        labels.push_back(positive ? 1.0 : 0.0);
    }
    zetta::ml::GbdtParams params;
    params.num_trees = 5;
    params.max_depth = 2;
    const auto trained =
        zetta::ml::train_gbdt(columns, {"tenure", "plan"}, labels, params);
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    zetta::ml::ModelValue model_value;
    model_value.kind = "classifier";
    model_value.name = "churn";
    auto model = node.create_model(std::move(model_value));
    ASSERT_TRUE(model);
    ASSERT_TRUE(model->set_weights(trained.model.to_json().dump()));

    // Under always-on auth (#799) the HTTP gate resolves the bearer to a USER
    // before the serving layer runs, so the serve:* authorize runs against that
    // user's effective permissions (a raw AccessKey bearer never reaches here —
    // it is 401 at the gate). The allow half is the seeded owner (admin holds
    // data:predict + data:search); the deny half is a second user with NO roles.
    // (The AccessKey-scoped allow/deny journeys live in test_serving.cpp.)
    auto denied_user = node.create_user("no-serving");
    ASSERT_TRUE(denied_user);
    const auto denied_token =
        node.create_token(denied_user->id(), zetta::governance::TokenType::Api);
    ASSERT_TRUE(denied_token.token);
    ASSERT_FALSE(denied_token.secret.empty());

    const std::string predict_target = "/v1/models/churn/predict";
    const std::string predict_body =
        nlohmann::json{{"rows",
                        {{{"tenure", 0.95}, {"plan", 0.5}},
                         {{"tenure", 0.05}, {"plan", 0.5}}}}}
            .dump();

    // Not exposed yet: the route answers 404 (owner is authorized to predict,
    // so this is the exposure check, not the capability check).
    auto before = request("POST", predict_target, predict_body);
    EXPECT_EQ(before.status, 404);

    // Expose over the same wire (the control plane), then predict.
    auto exposed = request("POST", "/v1/expose_model",
                           nlohmann::json{{"model", "churn"}}.dump());
    ASSERT_EQ(exposed.status, 200) << exposed.body;
    auto served = request("POST", predict_target, predict_body);
    ASSERT_EQ(served.status, 200) << served.body;
    auto served_body = json_of(served);
    ASSERT_TRUE(served_body.at("ok").get<bool>()) << served.body;
    ASSERT_EQ(served_body.at("value").at("ranked").size(), 2u);
    EXPECT_EQ(served_body.at("value").at("ranked")[0].at("entity"), 0);
    EXPECT_EQ(served_body.at("trace_id").get<std::string>().size(), 32u);

    // Deny: a user without data:predict gets 403 (unauthorized).
    auto denied = request("POST", predict_target, predict_body,
                          denied_token.secret);
    EXPECT_EQ(denied.status, 403) << denied.body;

    // Un-expose kills the route.
    auto unexposed = request("POST", "/v1/unexpose_model",
                             nlohmann::json{{"model", "churn"}}.dump());
    ASSERT_EQ(unexposed.status, 200) << unexposed.body;
    auto after = request("POST", predict_target, predict_body);
    EXPECT_EQ(after.status, 404) << after.body;

    // Same for an index: seed a searchable table, expose, GET the served
    // search route, then un-expose.
    node.index_registry().add(
        std::make_shared<zetta::test::TermFrequencyBackend>());
    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("articles");
    ASSERT_TRUE(table);
    table->add_row({{"title", "Postgres deep dive"},
                    {"body", "postgres postgres tuning"}});
    table->add_row({{"title", "Cooking pasta"}, {"body", "boil water"}});
    zetta::index::IndexValue index_value;
    index_value.table = "articles";
    index_value.kind = zetta::index::IndexKind::Search;
    index_value.backend = "tf_reference";
    index_value.fields = {"title", "body"};
    auto index = node.create_index(std::move(index_value));
    ASSERT_TRUE(index);
    const zetta::index::IndexSpec spec = index->spec();
    ASSERT_TRUE(node.index_registry()
                    .resolve(spec)
                    ->build(spec, zetta::index::read_index_partitions(
                                      table->store()))
                    .ok());

    const std::string search_target =
        "/v1/indexes/articles/search?query=postgres";
    auto search_before = request("GET", search_target, "");
    EXPECT_EQ(search_before.status, 404);

    auto index_exposed = request("POST", "/v1/expose_index",
                                 nlohmann::json{{"index", "articles"}}.dump());
    ASSERT_EQ(index_exposed.status, 200) << index_exposed.body;
    auto search_served = request("GET", search_target, "");
    ASSERT_EQ(search_served.status, 200) << search_served.body;
    auto search_body = json_of(search_served);
    ASSERT_TRUE(search_body.at("ok").get<bool>()) << search_served.body;
    ASSERT_GE(search_body.at("value").at("hits").size(), 1u);

    auto search_denied = request("GET", search_target, "", denied_token.secret);
    EXPECT_EQ(search_denied.status, 403) << search_denied.body;

    auto index_unexposed =
        request("POST", "/v1/unexpose_index",
                nlohmann::json{{"index", "articles"}}.dump());
    ASSERT_EQ(index_unexposed.status, 200) << index_unexposed.body;
    auto search_after = request("GET", search_target, "");
    EXPECT_EQ(search_after.status, 404) << search_after.body;
}

}  // namespace
