// The backup routes over the wire (zetta#1518, docs/data/backup-snapshot.md),
// called exactly as the Orama operator's backup Job calls them
// (orama-operator internal/backup/zetta.go): POST /v1/backup_snapshot answers
// {snapshot, summary} at the top level, GET streams the archive, DELETE releases
// it, POST /v1/restore_snapshot restores it into a fresh node and answers the
// same summary; a node holding data is refused with 409; every route needs
// cluster:admin and is audited; and POST /v1/restore_snapshot:discard empties
// only a node started as a restore target.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/server.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

namespace {

// One request per connection, the response read by its Content-Length.
struct Response {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    nlohmann::json json() const {
        return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    }
};

Response request(unsigned short port, const std::string& bearer, const std::string& method,
                 const std::string& target, const std::string& body = {}) {
    Response response;
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return response;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    timeval timeout{};
    timeout.tv_sec = 60;
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        return response;
    }
    std::string head = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    head += "Authorization: Bearer " + bearer + "\r\nConnection: close\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    const std::string out = head + body;
    for (std::size_t sent = 0; sent < out.size();) {
        const ssize_t wrote = ::send(descriptor, out.data() + sent, out.size() - sent, 0);
        if (wrote <= 0) break;
        sent += static_cast<std::size_t>(wrote);
    }
    std::string received;
    char buffer[256 * 1024];
    std::size_t header_end = std::string::npos;
    std::uint64_t length = 0;
    for (;;) {
        if (header_end != std::string::npos && received.size() >= header_end + 4 + length) break;
        const ssize_t got = ::recv(descriptor, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        received.append(buffer, static_cast<std::size_t>(got));
        if (header_end == std::string::npos) {
            header_end = received.find("\r\n\r\n");
            if (header_end == std::string::npos) continue;
            const std::string head_text = received.substr(0, header_end);
            response.status = std::atoi(head_text.substr(9, 3).c_str());
            std::size_t line = head_text.find("\r\n");
            while (line != std::string::npos) {
                const std::size_t next = head_text.find("\r\n", line + 2);
                const std::string field = head_text.substr(
                    line + 2, next == std::string::npos ? std::string::npos : next - line - 2);
                const std::size_t colon = field.find(':');
                if (colon != std::string::npos) {
                    std::string name = field.substr(0, colon);
                    std::ranges::transform(name, name.begin(),
                                           [](unsigned char c) { return std::tolower(c); });
                    std::string value = field.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    response.headers[name] = value;
                }
                line = next;
            }
            length = std::strtoull(response.headers["content-length"].c_str(), nullptr, 10);
        }
    }
    ::close(descriptor);
    if (header_end != std::string::npos) response.body = received.substr(header_end + 4, length);
    return response;
}

// A file-backed node, its API server and an owner loop on its own thread.
struct TestNode {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    std::unique_ptr<zetta::Node> node;
    std::unique_ptr<zetta::api::ApiServer> server;
    std::string secret = "backup-routes-owner-secret";
    std::atomic<bool> stopping{false};
    std::thread loop;

    explicit TestNode(bool restore_target = false) {
        zetta::NodeConfig config{dir.path().string(), /*preflight=*/false};
        config.restore_target = restore_target;
        node = std::make_unique<zetta::Node>(config, &replication);
        EXPECT_TRUE(node->start());
        EXPECT_TRUE(node->bootstrap_owner(secret).performed);
        zetta::api::ServerOptions options;
        options.port = 0;
        server = std::make_unique<zetta::api::ApiServer>(*node, nullptr, options);
        EXPECT_TRUE(server->start()) << server->reason();
    }

    void run_loop() {
        node->set_owner_wakeup([this] { server->wake(); });
        loop = std::thread([this] {
            auto last_tick = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
            while (!stopping) {
                server->drain();
                node->drain_owner_work();
                if (std::chrono::steady_clock::now() - last_tick >= std::chrono::milliseconds(100)) {
                    last_tick = std::chrono::steady_clock::now();
                    node->tick();
                }
                server->wait_for_work(std::chrono::milliseconds(20));
            }
        });
    }

    ~TestNode() {
        stopping = true;
        if (loop.joinable()) loop.join();
        node->set_owner_wakeup({});
        server->stop();
        node->stop();
    }

    Response call(const std::string& method, const std::string& target,
                  const std::string& body = {}, const std::string& bearer = {}) {
        return request(server->port(), bearer.empty() ? secret : bearer, method, target, body);
    }
};

// A table of 12 rows, a bucket of three objects (one past a slice), and a
// collection of 5 records, set up before the owner loop starts.
void seed(TestNode& source) {
    zetta::query::QueryService admin(*source.node);
    for (const char* statement : {"CREATE DATABASE lake", "CREATE TABLE lake.events (n INT)"}) {
        const auto outcome = admin.run(statement, "");
        ASSERT_TRUE(outcome.ok()) << statement << ": " << outcome.error;
    }
    for (int row = 0; row < 12; ++row) {
        const auto outcome =
            admin.run("INSERT INTO lake.events (n) VALUES (" + std::to_string(row) + ")", "");
        ASSERT_TRUE(outcome.ok()) << outcome.error;
    }
    zetta::data::ObjectStore& objects = *source.node->object_store();
    zetta::data::ObjectBucket bucket;
    bucket.name = "drive";
    ASSERT_TRUE(objects.create_bucket(bucket).ok());
    ASSERT_TRUE(objects.put("drive", "a", "alpha", {}).ok());
    ASSERT_TRUE(objects.put("drive", "b/c", std::string(1000, 'b'), {}).ok());
    ASSERT_TRUE(
        objects.put("drive", "large", std::string(zetta::data::ObjectStore::kSliceBytes + 99, 'L'), {})
            .ok());
    zetta::data::RecordStore& records = *source.node->record_store();
    zetta::data::RecordCollection collection;
    collection.name = "lake_sessions";
    ASSERT_TRUE(records.create_collection(collection).ok());
    for (int record = 0; record < 5; ++record) {
        zetta::data::RecordWrite write;
        write.collection = "lake_sessions";
        write.key = "s" + std::to_string(record);
        write.value = "c2VhbGVk";
        ASSERT_TRUE(records.write(write).ok());
    }
}

// Take a snapshot and fetch its archive, as the operator's Job does.
std::pair<nlohmann::json, std::string> snapshot_and_fetch(TestNode& source) {
    const Response taken = source.call("POST", "/v1/backup_snapshot");
    EXPECT_EQ(taken.status, 200) << taken.body;
    const nlohmann::json answer = taken.json();
    const std::string id = answer.value("snapshot", std::string{});
    const Response archive = source.call("GET", "/v1/backup_snapshot/" + id);
    EXPECT_EQ(archive.status, 200);
    EXPECT_EQ(archive.headers.at("content-type"), "application/octet-stream");
    return {answer, archive.body};
}

std::int64_t count_rows(TestNode& node) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const Response counted = node.call(
            "POST", "/v1/sql", R"({"statement":"SELECT count(*) AS n FROM lake.events"})");
        if (counted.status == 200) {
            const nlohmann::json rows = counted.json()["value"]["rows"];
            if (rows.is_array() && !rows.empty() && rows[0]["n"].is_number() &&
                rows[0]["n"].get<std::int64_t>() > 0) {
                return rows[0]["n"].get<std::int64_t>();
            }
        }
        if (std::chrono::steady_clock::now() > deadline) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace

TEST(BackupRoutes, TheOperatorsCallsBackUpAndRestoreIntoAFreshNode) {
    TestNode source;
    seed(source);
    source.run_loop();

    const auto [answer, archive] = snapshot_and_fetch(source);
    // The contract the operator reads: snapshot and summary at the top level,
    // and the same in the envelope's value.
    ASSERT_TRUE(answer["snapshot"].is_string()) << answer.dump();
    EXPECT_EQ(answer["value"]["snapshot"], answer["snapshot"]);
    const nlohmann::json summary = answer["summary"];
    EXPECT_EQ(summary["table:lake.events"], 12) << summary.dump();
    EXPECT_EQ(summary["bucket:drive"], 3);
    EXPECT_EQ(summary["collection:lake_sessions"], 5);
    EXPECT_FALSE(summary.contains("catalogue:user"));
    EXPECT_FALSE(summary.contains("catalogue:node"));
    EXPECT_GT(archive.size(), zetta::data::ObjectStore::kSliceBytes);

    const std::string id = answer["snapshot"].get<std::string>();
    EXPECT_EQ(source.call("DELETE", "/v1/backup_snapshot/" + id).status, 200);
    EXPECT_EQ(source.call("DELETE", "/v1/backup_snapshot/" + id).status, 404);
    EXPECT_EQ(source.call("GET", "/v1/backup_snapshot/" + id).status, 404);

    TestNode target;
    target.run_loop();
    const Response restored = target.call("POST", "/v1/restore_snapshot", archive);
    ASSERT_EQ(restored.status, 200) << restored.body;
    EXPECT_EQ(restored.json()["summary"], summary);
    EXPECT_EQ(restored.json()["value"]["details"]["node_id"], target.replication.node_id());
    EXPECT_EQ(count_rows(target), 12);
    EXPECT_EQ(target.call("GET", "/v1/objects/drive/b/c").body, std::string(1000, 'b'));
    EXPECT_EQ(target.call("GET", "/v1/objects/drive/large").body.size(),
              zetta::data::ObjectStore::kSliceBytes + 99);

    // A node holding data is never a target: 409.
    const Response again = target.call("POST", "/v1/restore_snapshot", archive);
    EXPECT_EQ(again.status, 409) << again.body;
    EXPECT_EQ(again.json()["error"]["code"], "conflict");
    EXPECT_EQ(source.call("POST", "/v1/restore_snapshot", archive).status, 409);

    // Every call is in the audit trail: its authorization and what it did.
    const Response audit = target.call(
        "POST", "/v1/sql",
        R"({"statement":"SELECT * FROM system.audit WHERE target = 'api:restore_snapshot'"})");
    ASSERT_EQ(audit.status, 200) << audit.body;
    EXPECT_GE(audit.json()["value"]["rows"].size(), 4u) << audit.body;  // two calls, two rows each
    const Response taken_audit = source.call(
        "POST", "/v1/sql",
        R"({"statement":"SELECT * FROM system.audit WHERE target = 'api:backup_snapshot'"})");
    ASSERT_EQ(taken_audit.status, 200);
    EXPECT_GE(taken_audit.json()["value"]["rows"].size(), 2u) << taken_audit.body;
}

TEST(BackupRoutes, EveryRouteNeedsClusterAdmin) {
    TestNode source;
    seed(source);
    zetta::query::QueryService admin(*source.node);
    for (const char* statement : {"CREATE ROLE operator_reader", "GRANT mcp:call TO ROLE operator_reader"}) {
        const auto outcome = admin.run(statement, "");
        ASSERT_TRUE(outcome.ok()) << statement << ": " << outcome.error;
    }
    auto reader = source.node->create_user("reader");
    ASSERT_TRUE(reader);
    ASSERT_TRUE(source.node->assign_role(reader->id(), source.node->find_role("operator_reader")->id()));
    const auto token = source.node->create_token(reader->id(), zetta::governance::TokenType::Api);
    ASSERT_TRUE(token.token);
    source.run_loop();

    const std::string id = zetta::util::new_uuid();
    for (const auto& [method, target] :
         std::vector<std::pair<std::string, std::string>>{
             {"POST", "/v1/backup_snapshot"},
             {"GET", "/v1/backup_snapshot/" + id},
             {"DELETE", "/v1/backup_snapshot/" + id},
             {"POST", "/v1/restore_snapshot"},
             {"POST", "/v1/restore_snapshot:discard"}}) {
        const Response refused = source.call(method, target, {}, token.secret);
        EXPECT_EQ(refused.status, 403) << method << " " << target << ": " << refused.body;
        EXPECT_EQ(refused.json()["error"]["code"], "unauthorized");
    }
    // Without a token at all: the gate answers 401.
    EXPECT_EQ(request(source.server->port(), "not-a-token", "POST", "/v1/backup_snapshot").status, 401);

    const Response audit = source.call(
        "POST", "/v1/sql",
        R"({"statement":"SELECT * FROM system.audit WHERE target = 'api:backup_snapshot'"})");
    ASSERT_EQ(audit.status, 200) << audit.body;
    bool denied = false;
    for (const auto& row : audit.json()["value"]["rows"]) {
        const auto& allowed = row.at("allowed");
        if (allowed == false || allowed == 0 || allowed == "false") denied = true;
    }
    EXPECT_TRUE(denied) << audit.body;
}

TEST(BackupRoutes, DiscardEmptiesOnlyARestoreTarget) {
    TestNode source;
    seed(source);
    source.run_loop();
    const auto [answer, archive] = snapshot_and_fetch(source);
    const std::string snapshot = answer["snapshot"].get<std::string>();

    // A node not started with --restore-target is never emptied, restored or not.
    TestNode live;
    live.run_loop();
    ASSERT_EQ(live.call("POST", "/v1/restore_snapshot", archive).status, 200);
    const nlohmann::json live_request{{"node_id", live.replication.node_id()}, {"snapshot", snapshot}};
    const Response refused = live.call("POST", "/v1/restore_snapshot:discard", live_request.dump());
    EXPECT_EQ(refused.status, 409) << refused.body;
    EXPECT_EQ(count_rows(live), 12);

    TestNode scratch(/*restore_target=*/true);
    scratch.run_loop();
    // Nothing restored yet: nothing to discard.
    const nlohmann::json early{{"node_id", scratch.replication.node_id()}, {"snapshot", snapshot}};
    EXPECT_EQ(scratch.call("POST", "/v1/restore_snapshot:discard", early.dump()).status, 409);
    const Response restored = scratch.call("POST", "/v1/restore_snapshot", archive);
    ASSERT_EQ(restored.status, 200) << restored.body;
    const std::string node_id = restored.json()["value"]["details"]["node_id"].get<std::string>();
    EXPECT_EQ(scratch.call("POST", "/v1/restore_snapshot:discard", "{}").status, 400);
    const nlohmann::json wrong{{"node_id", source.replication.node_id()}, {"snapshot", snapshot}};
    EXPECT_EQ(scratch.call("POST", "/v1/restore_snapshot:discard", wrong.dump()).status, 400);
    const nlohmann::json right{{"node_id", node_id}, {"snapshot", snapshot}};
    const Response emptied = scratch.call("POST", "/v1/restore_snapshot:discard", right.dump());
    ASSERT_EQ(emptied.status, 200) << emptied.body;
    EXPECT_EQ(scratch.call("GET", "/v1/objects/drive/a").status, 404);
    // The next weekly rehearsal finds it empty.
    const Response rehearsal = scratch.call("POST", "/v1/restore_snapshot", archive);
    ASSERT_EQ(rehearsal.status, 200) << rehearsal.body;
    EXPECT_EQ(rehearsal.json()["summary"], answer["summary"]);
}

TEST(BackupRoutes, ATruncatedArchiveIsRefused) {
    TestNode source;
    seed(source);
    source.run_loop();
    const auto [answer, archive] = snapshot_and_fetch(source);
    TestNode target;
    target.run_loop();
    const Response cut =
        target.call("POST", "/v1/restore_snapshot", archive.substr(0, archive.size() / 2));
    EXPECT_EQ(cut.status, 400) << cut.body;
    EXPECT_EQ(cut.json()["error"]["code"], "invalid_input");
}
