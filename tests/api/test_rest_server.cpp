// Protocol-layer (loopback HTTP) coverage of the REST rendering (#618,
// docs/api/rest-openapi.md): real GETs and POSTs against /v1/<name> — the
// curl surface — with the test thread playing the owning loop (drain()), the
// same layer-2 discipline as test_mcp_server.cpp.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "index/term_frequency_backend.hpp"
#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/api/server.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/cluster/cluster.hpp"
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
#include "zetta/pipeline/run.hpp"
#include "zetta/version.hpp"

namespace {

struct HttpResponse {
    int status = 0;
    std::string body;
};

int open_http_request(unsigned short port, const std::string& method,
                      const std::string& target, const std::string& body,
                      const std::string& bearer = {}) {
    int socket_descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_descriptor < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const int flags = ::fcntl(socket_descriptor, F_GETFL, 0);
    ::fcntl(socket_descriptor, F_SETFL, flags | O_NONBLOCK);
    const int connected = ::connect(
        socket_descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    pollfd pending{socket_descriptor, POLLOUT, 0};
    int connect_error = 0;
    socklen_t error_size = sizeof(connect_error);
    if ((connected != 0 && errno != EINPROGRESS) ||
        (connected != 0 && ::poll(&pending, 1, 1000) != 1) ||
        ::getsockopt(socket_descriptor, SOL_SOCKET, SO_ERROR,
                     &connect_error, &error_size) != 0 || connect_error != 0) {
        ::close(socket_descriptor);
        return -1;
    }
    ::fcntl(socket_descriptor, F_SETFL, flags);
    // A bounded wait (#1007): a server that never answers must FAIL the test
    // (status 0, empty body), not park the suite in recv() forever — the hang is
    // exactly the failure some of these tests exist to catch.
    timeval receive_timeout{};
    receive_timeout.tv_sec = 10;
    ::setsockopt(socket_descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                 sizeof(receive_timeout));
    std::string request = method + " " + target + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    if (!body.empty()) request += "Content-Type: application/json\r\n";
    if (!bearer.empty()) request += "Authorization: Bearer " + bearer + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t written =
            ::send(socket_descriptor, request.data() + sent, request.size() - sent, 0);
        if (written <= 0) {
            ::close(socket_descriptor);
            return -1;
        }
        sent += static_cast<std::size_t>(written);
    }
    return socket_descriptor;
}

HttpResponse read_http_response(int socket_descriptor) {
    if (socket_descriptor < 0) return {};
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

// A deliberately dumb blocking HTTP/1.1 client: one request, Connection:
// close. Runs on a helper thread while the test thread drains the server's
// bridge (the owning-loop role).
HttpResponse http_request(unsigned short port, const std::string& method,
                          const std::string& target, const std::string& body,
                          const std::string& bearer = {}) {
    return read_http_response(
        open_http_request(port, method, target, body, bearer));
}

struct OpenSockets {
    std::vector<int> descriptors;

    ~OpenSockets() {
        for (const int descriptor : descriptors) {
            if (descriptor >= 0) ::close(descriptor);
        }
    }
};

struct RestServerFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    std::unique_ptr<zetta::api::ApiServer> server;
    zetta::api::ServerOptions options;
    // Auth is always on (#799): seed the owner (admin) so this secret
    // authenticates as it; request() sends it by default so every fixture call
    // is a successful, authorized admin call.
    std::string owner_secret = "rest-owner-secret";

    void SetUp() override {
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(node.bootstrap_owner(owner_secret).performed);
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

struct SmallConnectionCacheFixture : RestServerFixture {
    void SetUp() override {
        options.idle_connection_cache = 1;
        RestServerFixture::SetUp();
    }
};

struct ConnectionLimitFixture : SmallConnectionCacheFixture {
    void SetUp() override {
        options.max_connections = 2;
        SmallConnectionCacheFixture::SetUp();
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

// #1007: a request no route matches used to get NO response — the router fell
// through, the worker wrote nothing, and the client waited until its own timeout
// (0 bytes), or saw "Empty reply from server" through a proxy. That is what a node
// older than a capability shows for that capability's route, and what calling a
// read with POST shows (#954). Every one of these must now be answered.
TEST_F(RestServerFixture, UnroutedRequestsAreAnsweredInsteadOfHanging) {
    auto unknown = request("GET", "/v1/no_such_capability?run_id=x&limit=10");
    EXPECT_EQ(unknown.status, 404) << unknown.body;
    auto unknown_body = json_of(unknown);
    ASSERT_TRUE(unknown_body.is_object()) << unknown.body;
    EXPECT_FALSE(unknown_body.at("ok").get<bool>());
    EXPECT_EQ(unknown_body.at("error").at("code"), "unknown_capability");

    // A read called as an action says which method it takes.
    auto read_as_post = request("POST", "/v1/get_run_report", R"({"run_id":"x"})");
    EXPECT_EQ(read_as_post.status, 405) << read_as_post.body;
    auto read_as_post_body = json_of(read_as_post);
    ASSERT_TRUE(read_as_post_body.is_object()) << read_as_post.body;
    EXPECT_EQ(read_as_post_body.at("error").at("code"), "method_not_allowed");
    EXPECT_NE(read_as_post_body.at("error").at("message").get<std::string>().find("GET"),
              std::string::npos);

    // ...and an action called as a read.
    auto action_as_get = request("GET", "/v1/create_project?name=x");
    EXPECT_EQ(action_as_get.status, 405) << action_as_get.body;
    EXPECT_NE(json_of(action_as_get).at("error").at("message").get<std::string>().find("POST"),
              std::string::npos);

    auto elsewhere = request("GET", "/no/such/route");
    EXPECT_EQ(elsewhere.status, 404) << elsewhere.body;
    EXPECT_EQ(json_of(elsewhere).at("error").at("code"), "not_found");

    // The node is still serving afterwards.
    EXPECT_EQ(request("GET", "/v1/server_info").status, 200);
}

// #1007, the reported path end to end over REST: create_project -> capture_intent
// -> compile_intent (with input_id) -> approve_plan -> gate -> run, with NO dataset
// attached, then the report. get_run answered `succeeded` while get_run_report
// never did. Both, with and without `limit`, must answer — and a run that trained
// no model must answer with an error envelope, not silence.
// A churn run executes over the live compute plane, so this one needs the node's
// cluster attached (a single-member JobCoordinator) — the AttachedNode shape. The
// cluster outlives TearDown's node.stop() (members are destroyed after it).
struct RestClusterFixture : RestServerFixture {
    zetta::cluster::Cluster cluster{"rest-report-node", [] {
                                        zetta::cluster::ClusterConfig config;
                                        config.listen_host = "127.0.0.1";
                                        config.listen_port = 24740;
                                        config.port_range = 8;
                                        return config;
                                    }()};

    void SetUp() override {
        RestServerFixture::SetUp();
        cluster.start();
        node.attach_cluster(cluster, /*worker_count=*/2);
        ASSERT_NE(node.job_coordinator(), nullptr);
    }
};

TEST_F(RestClusterFixture, RunReportAnswersForADatasetlessRun) {
    auto post = [&](const std::string& name, const nlohmann::json& body) {
        auto response = request("POST", "/v1/" + name, body.dump());
        EXPECT_EQ(response.status, 200) << name << ": " << response.body;
        return json_of(response).value("value", nlohmann::json::object());
    };
    post("create_project", {{"name", "report-1007"}});
    const auto captured = post(
        "capture_intent",
        {{"project", "report-1007"},
         {"text", "predict which customers will churn next month and why"}});
    const std::string input_id = captured.value("input_id", std::string{});
    ASSERT_FALSE(input_id.empty()) << captured.dump();
    const auto compiled =
        post("compile_intent", {{"project", "report-1007"}, {"input_id", input_id}});
    const std::string graph = compiled.value("name", std::string{});
    ASSERT_FALSE(graph.empty()) << compiled.dump();
    post("approve_plan", {{"project", "report-1007"}, {"graph", graph}});
    post("gate", {{"project", "report-1007"}, {"graph", graph}});
    const auto started = post("run", {{"project", "report-1007"}, {"graph", graph}});
    const std::string run_id = started.value("run_id", std::string{});
    ASSERT_FALSE(run_id.empty()) << started.dump();

    // Let the run finish, playing the owning loop (tick + drain) meanwhile.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::shared_ptr<zetta::pipeline::Run> run;
    while (std::chrono::steady_clock::now() < deadline) {
        node.tick();
        server->drain();
        run = node.find_run(run_id);
        if (run && (run->status() == zetta::pipeline::RunStatus::Succeeded ||
                    run->status() == zetta::pipeline::RunStatus::Failed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(run);
    ASSERT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
    auto got = request("GET", "/v1/get_run?run_id=" + run_id);
    EXPECT_EQ(got.status, 200) << got.body;

    auto report = request("GET", "/v1/get_run_report?run_id=" + run_id);
    ASSERT_EQ(report.status, 200) << report.body;
    auto report_body = json_of(report);
    ASSERT_TRUE(report_body.at("ok").get<bool>()) << report.body;
    EXPECT_TRUE(report_body.at("value").contains("model"));

    auto limited = request("GET", "/v1/get_run_report?run_id=" + run_id + "&limit=10");
    ASSERT_EQ(limited.status, 200) << limited.body;
    EXPECT_LE(json_of(limited).at("value").at("at_risk").size(), 10u);

    // A run that trained no model: an error envelope that says why.
    auto bare = node.create_run("report-1007");
    ASSERT_TRUE(bare);
    auto no_model = request("GET", "/v1/get_run_report?run_id=" + bare->id());
    EXPECT_EQ(no_model.status, 404) << no_model.body;
    auto no_model_body = json_of(no_model);
    ASSERT_TRUE(no_model_body.is_object()) << no_model.body;
    EXPECT_EQ(no_model_body.at("error").at("code"), "not_found");
    EXPECT_NE(no_model_body.at("error").at("message").get<std::string>().find("model"),
              std::string::npos);
}

// GET /v1/openapi.json serves exactly what the generator produces from the
// server's registry — the served document and the drift-checked artifact
// share one source of truth.
// #1008 over the wire: an upload well past the HTTP parser's old 64 KiB default
// body limit lands, and reads back through list_datasets. Before the limit was
// raised, a body this size could not reach the capability at all.
TEST_F(RestServerFixture, UploadDatasetOverRestAcceptsALargeBody) {
    std::string content = "account,plan,mrr\n";
    std::size_t rows = 0;
    while (content.size() < 1024 * 1024) {  // ~1 MiB of csv
        content += std::to_string(rows) + ",pro," + std::to_string(rows * 3) + "\n";
        ++rows;
    }
    const nlohmann::json body = {
        {"table_name", "uploaded"}, {"format", "csv"}, {"content", content}};
    auto uploaded = request("POST", "/v1/upload_dataset", body.dump());
    ASSERT_EQ(uploaded.status, 200) << uploaded.body.substr(0, 400);
    auto uploaded_body = json_of(uploaded);
    ASSERT_TRUE(uploaded_body.at("ok").get<bool>()) << uploaded.body.substr(0, 400);
    EXPECT_EQ(uploaded_body.at("value").at("rows"), rows);

    auto listed = request("GET", "/v1/list_datasets");
    ASSERT_EQ(listed.status, 200) << listed.body;
    bool found = false;
    for (const auto& dataset : json_of(listed).at("value").at("datasets")) {
        if (dataset.at("name") == "uploaded") {
            found = true;
            EXPECT_EQ(dataset.at("rows"), rows);
        }
    }
    EXPECT_TRUE(found) << listed.body;
}

TEST_F(RestServerFixture, OpenApiDocumentIsServed) {
    auto served = request("GET", "/v1/openapi.json");
    EXPECT_EQ(served.status, 200);
    const nlohmann::json document = json_of(served);
    ASSERT_TRUE(document.is_object()) << served.body;
    EXPECT_EQ(document, zetta::api::openapi_document(server->registry()));
    EXPECT_TRUE(document.at("paths").contains("/v1/server_info"));
}

// #1076: a long poll is a suspended coroutine, not a blocking-pool job. Five
// hundred concurrent waits must all register, and a no-wait receive must still
// reach the owner loop and finish immediately. Publishing wakes every waiter;
// each long poll makes exactly one more governed call to collect the event.
TEST_F(RestServerFixture, FiveHundredLongPollsLeaveNoWaitReceivesFast) {
    const auto created = request(
        "POST", "/v1/create_topic",
        R"({"topic":"topic:stress","exposed":true})");
    ASSERT_EQ(created.status, 200) << created.body;

    constexpr std::size_t kLongPolls = 500;
    const std::size_t audit_before = node.audit_event_count();
    OpenSockets long_polls;
    long_polls.descriptors.reserve(kLongPolls);
    for (std::size_t index = 0; index < kLongPolls; ++index) {
        const int descriptor = open_http_request(
            server->port(), "GET",
            "/v1/receive_events?topic=stress&wait_ms=5000", "",
            owner_secret);
        ASSERT_GE(descriptor, 0) << "long poll " << index;
        long_polls.descriptors.push_back(descriptor);
        if ((index + 1) % 25 == 0) server->drain();
    }

    const auto registration_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (node.audit_event_count() < audit_before + kLongPolls &&
           std::chrono::steady_clock::now() < registration_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    const bool all_registered =
        node.audit_event_count() >= audit_before + kLongPolls;

    HttpResponse no_wait;
    std::atomic<bool> no_wait_finished{false};
    const auto no_wait_started = std::chrono::steady_clock::now();
    std::thread no_wait_client([&] {
        no_wait = http_request(
            server->port(), "GET",
            "/v1/receive_events?topic=stress&wait_ms=0", "",
            owner_secret);
        no_wait_finished = true;
    });
    const auto fast_deadline =
        no_wait_started + std::chrono::milliseconds(100);
    while (!no_wait_finished &&
           std::chrono::steady_clock::now() < fast_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    const bool finished_while_waiters_parked = no_wait_finished.load();
    const auto no_wait_elapsed =
        std::chrono::steady_clock::now() - no_wait_started;

    zetta::api::ApiContext context{
        node, nullptr, "", zetta::observability::make_root_trace(),
        &node.span_recorder()};
    const auto published = server->registry().call(
        "publish_event",
        nlohmann::json{{"topic", "stress"}, {"payload", "wake"}},
        context);
    EXPECT_TRUE(published.ok) << published.error.message;

    const auto completion_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!no_wait_finished &&
           std::chrono::steady_clock::now() < completion_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    no_wait_client.join();

    const auto expected_audits = audit_before + (kLongPolls * 2) + 2;
    while (all_registered && node.audit_event_count() < expected_audits &&
           std::chrono::steady_clock::now() < completion_deadline) {
        server->drain();
        std::this_thread::yield();
    }

    EXPECT_TRUE(all_registered)
        << "every long poll must register without waiting for a pool thread";
    EXPECT_TRUE(finished_while_waiters_parked)
        << "the no-wait receive did not finish within 100 ms";
    EXPECT_LT(no_wait_elapsed, std::chrono::milliseconds(5));
    ASSERT_EQ(no_wait.status, 200) << no_wait.body;
    const nlohmann::json no_wait_body = json_of(no_wait);
    ASSERT_TRUE(no_wait_body.at("ok").get<bool>()) << no_wait.body;
    EXPECT_LT(no_wait_body.at("duration_ms").get<double>(), 5.0);
    EXPECT_EQ(node.audit_event_count(), expected_audits)
        << "each long poll may make only its initial and final governed calls";
    if (all_registered && node.audit_event_count() == expected_audits) {
        for (int& descriptor : long_polls.descriptors) {
            const auto response = read_http_response(descriptor);
            descriptor = -1;
            ASSERT_EQ(response.status, 200) << response.body;
            const auto body = json_of(response);
            ASSERT_TRUE(body.at("ok").get<bool>()) << response.body;
            ASSERT_EQ(body.at("value").at("events").size(), 1);
        }
    }
    std::cout << "#1076 500 long polls: no-wait wall "
              << std::chrono::duration<double, std::milli>(no_wait_elapsed).count()
              << " ms, registry "
              << no_wait_body.at("duration_ms").get<double>() << " ms\n";
}

TEST_F(SmallConnectionCacheFixture, FiveHundredHeldPollsLeaveTwentyFreshConnectionsFast) {
    ASSERT_EQ(request("POST", "/v1/create_topic",
                      R"({"topic":"topic:acceptance","exposed":true})").status,
              200);
    ASSERT_EQ(request("POST", "/v1/publish_event",
                      R"({"topic":"acceptance","payload":"head"})").status,
              200);
    const std::size_t audit_before = node.audit_event_count();
    constexpr std::size_t kLongPolls = 500;
    constexpr std::size_t kProbes = 20;
    OpenSockets long_polls;
    for (std::size_t index = 0; index < kLongPolls; ++index) {
        const int descriptor = open_http_request(
            server->port(), "GET",
            "/v1/receive_events?topic=acceptance&from_sequence=2&wait_ms=10000",
            "", owner_secret);
        ASSERT_GE(descriptor, 0) << "connect timed out at held poll " << index;
        long_polls.descriptors.push_back(descriptor);
        if ((index + 1) % 25 == 0) server->drain();
    }
    const auto registration_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (node.audit_event_count() < audit_before + kLongPolls &&
           std::chrono::steady_clock::now() < registration_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    ASSERT_EQ(node.audit_event_count(), audit_before + kLongPolls);

    std::vector<HttpResponse> responses;
    std::vector<double> elapsed_ms;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        for (std::size_t index = 0; index < kProbes; ++index) {
            const auto started = std::chrono::steady_clock::now();
            responses.push_back(http_request(
                server->port(), "GET",
                "/v1/receive_events?topic=acceptance&from_sequence=2&wait_ms=0",
                "", owner_secret));
            elapsed_ms.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count());
            if (responses.back().status != 200) break;
        }
        finished = true;
    });
    const auto probe_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!finished && std::chrono::steady_clock::now() < probe_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    const bool finished_with_polls_held = finished.load();
    EXPECT_EQ(node.audit_event_count(), audit_before + kLongPolls + kProbes);

    const auto published = request(
        "POST", "/v1/publish_event",
        R"({"topic":"acceptance","payload":"release"})");
    EXPECT_EQ(published.status, 200) << published.body;
    const auto cleanup_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!finished && std::chrono::steady_clock::now() < cleanup_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    client.join();
    EXPECT_TRUE(finished_with_polls_held);
    ASSERT_EQ(responses.size(), kProbes);
    for (std::size_t index = 0; index < kProbes; ++index) {
        ASSERT_EQ(responses[index].status, 200) << responses[index].body;
        const auto body = json_of(responses[index]);
        ASSERT_TRUE(body.at("ok").get<bool>()) << responses[index].body;
        EXPECT_TRUE(body.at("value").at("events").empty());
        EXPECT_LT(elapsed_ms[index], 5.0) << "fresh connection " << index;
    }
    std::cout << "#1076 500 held polls, 20 fresh connections: max wall "
              << *std::max_element(elapsed_ms.begin(), elapsed_ms.end()) << " ms\n";
}

TEST_F(ConnectionLimitFixture, FullCapacityAcceptsAndRejectsPromptlyThenRecovers) {
    ASSERT_EQ(request("POST", "/v1/create_topic",
                      R"({"topic":"topic:limited","exposed":true})").status,
              200);
    const std::size_t audit_before = node.audit_event_count();
    OpenSockets long_polls;
    for (int index = 0; index < 2; ++index) {
        const int descriptor = open_http_request(
            server->port(), "GET", "/v1/receive_events?topic=limited&wait_ms=10000",
            "", owner_secret);
        ASSERT_GE(descriptor, 0);
        long_polls.descriptors.push_back(descriptor);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (node.audit_event_count() < audit_before + 2 &&
           std::chrono::steady_clock::now() < deadline) {
        server->drain();
        std::this_thread::yield();
    }
    ASSERT_EQ(node.audit_event_count(), audit_before + 2);
    for (int index = 0; index < 20; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const auto response = request(
            "GET", "/v1/receive_events?topic=limited&wait_ms=0");
        EXPECT_EQ(response.status, 503) << response.body;
        EXPECT_EQ(json_of(response).at("error").at("code"), "busy");
        EXPECT_LT(std::chrono::steady_clock::now() - started,
                  std::chrono::milliseconds(100));
    }
    EXPECT_EQ(node.audit_event_count(), audit_before + 2);

    zetta::api::ApiContext context{
        node, nullptr, "", zetta::observability::make_root_trace(),
        &node.span_recorder()};
    ASSERT_TRUE(server->registry().call(
        "publish_event", nlohmann::json{{"topic", "limited"}, {"payload", "release"}},
        context).ok);
    const auto released = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (node.audit_event_count() < audit_before + 5 &&
           std::chrono::steady_clock::now() < released) {
        server->drain();
        std::this_thread::yield();
    }
    ASSERT_EQ(node.audit_event_count(), audit_before + 5);
    for (int& descriptor : long_polls.descriptors) {
        const auto response = read_http_response(descriptor);
        descriptor = -1;
        EXPECT_EQ(response.status, 200) << response.body;
    }
    EXPECT_EQ(request("GET", "/v1/receive_events?topic=limited&wait_ms=0").status, 200);
}

TEST_F(RestServerFixture, StopWakesHeldLongPollsAndJoinsSessions) {
    ASSERT_EQ(request("POST", "/v1/create_topic",
                      R"({"topic":"topic:shutdown","exposed":true})").status,
              200);
    const auto audit_before = node.audit_event_count();
    OpenSockets long_polls;
    const int descriptor = open_http_request(
        server->port(), "GET", "/v1/receive_events?topic=shutdown&wait_ms=30000",
        "", owner_secret);
    ASSERT_GE(descriptor, 0);
    long_polls.descriptors.push_back(descriptor);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (node.audit_event_count() == audit_before &&
           std::chrono::steady_clock::now() < deadline) {
        server->drain();
        std::this_thread::yield();
    }
    ASSERT_EQ(node.audit_event_count(), audit_before + 1);
    const auto started = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    EXPECT_FALSE(server->running());
    ASSERT_TRUE(server->start()) << server->reason();
    EXPECT_EQ(request("GET", "/v1/receive_events?topic=shutdown&wait_ms=0").status, 200);
}

TEST_F(RestServerFixture, ZeroConnectionCapacityIsRejected) {
    server->stop();
    options.max_connections = 0;
    server = std::make_unique<zetta::api::ApiServer>(node, nullptr, options);
    EXPECT_FALSE(server->start());
    EXPECT_NE(server->reason().find("max_connections"), std::string::npos);
}

TEST_F(RestServerFixture, StopDrainsOffloadedOwnerWork) {
    server->stop();
    node.tick();  // adopt this thread as the real owner loop does
    zetta::api::Capability capability{
        "shutdown_offload", "Shutdown offload", "Exercise an offloaded owner hop",
        nlohmann::json{{"type", "object"}}, {},
        zetta::api::CapabilityKind::Read, {}, false, false,
        [](const nlohmann::json&, zetta::api::ApiContext& context) {
            context.node.run_on_owner([] {});
            return zetta::api::ApiResult::success({});
        }};
    capability.offloadable = true;
    ASSERT_TRUE(server->registry().add(std::move(capability)));
    ASSERT_TRUE(server->start()) << server->reason();
    auto owner_work_queued = std::make_shared<std::atomic<bool>>(false);
    node.set_owner_wakeup([owner_work_queued] { *owner_work_queued = true; });
    OpenSockets clients;
    const int descriptor = open_http_request(
        server->port(), "GET", "/v1/shutdown_offload", "", owner_secret);
    ASSERT_GE(descriptor, 0);
    clients.descriptors.push_back(descriptor);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!owner_work_queued->load() && std::chrono::steady_clock::now() < deadline) {
        server->drain();
        std::this_thread::yield();
    }
    ASSERT_TRUE(owner_work_queued->load());
    const auto started = std::chrono::steady_clock::now();
    server->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    node.set_owner_wakeup({});
}

TEST_F(RestServerFixture, LongPollTimeoutMakesOneFinalGovernedRead) {
    ASSERT_EQ(request("POST", "/v1/create_topic",
                      R"({"topic":"topic:timeout","exposed":true})")
                  .status,
              200);
    const std::size_t audit_before = node.audit_event_count();
    const auto started = std::chrono::steady_clock::now();
    const auto response = request(
        "GET", "/v1/receive_events?topic=timeout&wait_ms=20");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(response.status, 200) << response.body;
    const auto body = json_of(response);
    ASSERT_TRUE(body.at("ok").get<bool>()) << response.body;
    EXPECT_TRUE(body.at("value").at("events").empty());
    EXPECT_GE(elapsed, std::chrono::milliseconds(20));
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_EQ(node.audit_event_count(), audit_before + 2);
}

TEST_F(RestServerFixture, DroppingATopicWakesItsLongPoll) {
    ASSERT_EQ(request("POST", "/v1/create_topic",
                      R"({"topic":"topic:closing","exposed":true})")
                  .status,
              200);
    const std::size_t audit_before = node.audit_event_count();
    HttpResponse waiting;
    std::atomic<bool> finished{false};
    std::thread client([&] {
        waiting = http_request(
            server->port(), "GET",
            "/v1/receive_events?topic=closing&wait_ms=5000", "",
            owner_secret);
        finished = true;
    });
    const auto registration_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (node.audit_event_count() < audit_before + 1 &&
           std::chrono::steady_clock::now() < registration_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    const bool registered = node.audit_event_count() >= audit_before + 1;

    const auto dropped = request(
        "POST", "/v1/drop_topic", R"({"topic":"closing"})");
    EXPECT_EQ(dropped.status, 200) << dropped.body;
    const auto completion_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!finished &&
           std::chrono::steady_clock::now() < completion_deadline) {
        server->drain();
        std::this_thread::yield();
    }
    client.join();

    EXPECT_TRUE(registered);
    EXPECT_EQ(waiting.status, 404) << waiting.body;
    EXPECT_EQ(json_of(waiting).at("error").at("code"), "not_found");
    EXPECT_EQ(node.audit_event_count(), audit_before + 3);
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
