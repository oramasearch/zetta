#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/native_http_transport.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/connect/destination_policy.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/action_integration.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using Json = nlohmann::json;
using zetta::cluster::ReplicationManager;
using zetta::solutions::ActionConnectorKind;
using zetta::solutions::ActionRuntime;
using zetta::solutions::ActionTaskSpec;
using zetta::solutions::ActionTaskStatus;
using zetta::solutions::DecisionPolicyStore;
using zetta::solutions::ReportPublicationSpec;

std::size_t content_length(const std::string& request) {
    const std::string name = "Content-Length:";
    const std::size_t position = request.find(name);
    if (position == std::string::npos) {
        return 0;
    }
    const std::size_t begin = request.find_first_not_of(
        " \t",
        position + name.size());
    const std::size_t end = request.find("\r\n", begin);
    return static_cast<std::size_t>(std::stoul(request.substr(begin, end - begin)));
}

class LocalHttpServer {
public:
    explicit LocalHttpServer(std::vector<int> statuses)
        : statuses_(std::move(statuses)) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) {
            error_ = std::strerror(errno);
            return;
        }
        int enabled = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(
                socket_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0 ||
            ::listen(socket_, 8) != 0) {
            error_ = std::strerror(errno);
            ::close(socket_);
            socket_ = -1;
            return;
        }
        socklen_t size = sizeof(address);
        if (::getsockname(
                socket_,
                reinterpret_cast<sockaddr*>(&address),
                &size) != 0) {
            error_ = std::strerror(errno);
            ::close(socket_);
            socket_ = -1;
            return;
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~LocalHttpServer() {
        if (worker_.joinable() && !finished_) {
            ::shutdown(socket_, SHUT_RDWR);
            ::close(socket_);
            socket_ = -1;
        }
        wait();
        if (socket_ >= 0) {
            ::close(socket_);
        }
    }

    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    std::string url(std::string path = "/tasks") const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    void wait() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    const std::vector<std::string>& requests() const {
        return requests_;
    }

    const std::string& error() const {
        return error_;
    }

private:
    std::string read_request(int connection) {
        std::string request;
        std::array<char, 4096> buffer{};
        std::size_t expected = std::string::npos;
        while (true) {
            const ssize_t received = ::recv(
                connection,
                buffer.data(),
                buffer.size(),
                0);
            if (received <= 0) {
                break;
            }
            request.append(buffer.data(), static_cast<std::size_t>(received));
            const std::size_t headers_end = request.find("\r\n\r\n");
            if (headers_end != std::string::npos && expected == std::string::npos) {
                expected = headers_end + 4 + content_length(request);
            }
            if (expected != std::string::npos && request.size() >= expected) {
                break;
            }
        }
        return request;
    }

    void serve() {
        for (const int status : statuses_) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const int connection = ::accept(
                socket_,
                reinterpret_cast<sockaddr*>(&peer),
                &peer_size);
            if (connection < 0) {
                error_ = std::strerror(errno);
                finished_ = true;
                return;
            }
            requests_.push_back(read_request(connection));
            const std::string body = "{\"ack\":\"received\"}";
            const std::string reason = status >= 200 && status < 300
                                           ? "OK"
                                           : "Failure";
            const std::string response =
                "HTTP/1.1 " + std::to_string(status) + " " + reason +
                "\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            std::size_t sent = 0;
            while (sent < response.size()) {
                const ssize_t count = ::send(
                    connection,
                    response.data() + sent,
                    response.size() - sent,
                    0);
                if (count <= 0) {
                    error_ = std::strerror(errno);
                    break;
                }
                sent += static_cast<std::size_t>(count);
            }
            ::close(connection);
        }
        finished_ = true;
    }

    int socket_ = -1;
    std::uint16_t port_ = 0;
    std::vector<int> statuses_;
    std::vector<std::string> requests_;
    std::string error_;
    std::thread worker_;
    std::atomic<bool> finished_{false};
};

// A loopback listener that never accepts: whether a client connected shows as a
// pending connection on its queue. Used to prove a refused delivery never dialled.
class IdleListener {
public:
    IdleListener() {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t size = sizeof(address);
        if (socket_ < 0 ||
            ::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(socket_, 8) != 0 ||
            ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            return;
        }
        port_ = ntohs(address.sin_port);
    }
    ~IdleListener() {
        if (socket_ >= 0) ::close(socket_);
    }
    IdleListener(const IdleListener&) = delete;
    IdleListener& operator=(const IdleListener&) = delete;

    std::uint16_t port() const { return port_; }
    std::string url(const std::string& host, std::string path = "/tasks") const {
        return "http://" + host + ":" + std::to_string(port_) + path;
    }
    // True when some client has connected (a connection waits to be accepted).
    bool was_dialled() const {
        pollfd descriptor{socket_, POLLIN, 0};
        return ::poll(&descriptor, 1, 100) > 0;
    }

private:
    int socket_ = -1;
    std::uint16_t port_ = 0;
};

Json policy_config(bool kill_switch = false) {
    return {{"kill_switch", kill_switch},
            {"threshold", 0.5},
            {"tiers",
             {{{"name", "high"},
               {"minimum_score", 0.8},
               {"action", "call"}}}},
            {"budget", {{"limit", 100.0}}},
            {"contact_cap", 3},
            {"frequency_cap", {{"max_actions", 2}, {"window_seconds", 86400}}},
            {"eligibility_rules", Json::array()},
            {"exclusion_rules", Json::array()},
            {"autonomy", "A3"},
            {"tunable_bounds", Json::object()}};
}

Json entity(std::string id, bool holdout = false) {
    return {{"entity_id", std::move(id)},
            {"score", 0.9},
            {"cost", 4.0},
            {"contacts", 0},
            {"recent_actions", 0},
            {"holdout", holdout},
            {"score_at", "2026-09-28T10:00:00.000Z"}};
}

struct ActionIntegrationFixture : ::testing::Test {
    zetta::test::TempDir directory;
    ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{{directory.path().string(), false}, &replication};
    DecisionPolicyStore store{replication, &node};

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::install_native_http_transport();
        // The fixtures listen on 127.0.0.1; an action endpoint reaches loopback
        // only when the node allows it (#1394).
        zetta::connect::AllowedDestinations allowed;
        allowed.loopback = true;
        zetta::connect::set_allowed_destinations(allowed);
    }

    void TearDown() override {
        zetta::connect::set_allowed_destinations({});
        zetta::api::shutdown_native_http_transport();
        node.stop();
    }

    std::pair<std::string, std::string> create_decision(
        std::string name,
        bool holdout = false) {
        std::string error;
        const auto policy = store.create(name, policy_config(), "owner", &error);
        EXPECT_TRUE(policy) << error;
        if (!policy) {
            return {};
        }
        const auto decision = store.decide(
            policy->id,
            entity(name + "-entity", holdout),
            "agent",
            &error);
        EXPECT_TRUE(decision) << error;
        if (!decision) {
            return {policy->id, {}};
        }
        return {policy->id, decision->id};
    }

    ActionTaskSpec task_spec(
        const std::string& decision_id,
        const std::string& endpoint,
        ActionConnectorKind connector,
        std::string key) {
        ActionTaskSpec spec;
        spec.decision_id = decision_id;
        spec.owner = "owner-7";
        spec.sla_due_at = "2026-09-29T10:00:00.000Z";
        spec.connector.kind = connector;
        spec.connector.endpoint = endpoint;
        spec.connector.token = "local-test-token";
        spec.payload = {{"subject", "Call account"},
                        {"description", "Discuss renewal"}};
        spec.idempotency_key = std::move(key);
        spec.max_attempts = 3;
        spec.model_version = "model-v3";
        spec.agent_version = "agent-v2";
        spec.cell = "treatment";
        spec.currency = "USD";
        return spec;
    }
};

TEST_F(ActionIntegrationFixture, WebhookRetriesWithOneIdempotencyKeyAndRecordsLedger) {
    LocalHttpServer server({500, 503, 200});
    const auto [policy_id, decision_id] = create_decision("webhook-retry");
    ASSERT_FALSE(policy_id.empty());
    ASSERT_FALSE(decision_id.empty());

    ActionRuntime runtime(node);
    std::string error;
    const auto delivered = runtime.execute_closed_loop(
        task_spec(
            decision_id,
            server.url(),
            ActionConnectorKind::Webhook,
            "delivery-key-1"),
        "agent-key",
        &error);
    ASSERT_TRUE(delivered) << error;
    EXPECT_EQ(delivered->status, ActionTaskStatus::Delivered);
    EXPECT_EQ(delivered->attempts, 3u);
    EXPECT_EQ(delivered->acknowledgement.at("http_status"), 200);
    server.wait();
    ASSERT_TRUE(server.error().empty()) << server.error();
    ASSERT_EQ(server.requests().size(), 3u);
    for (const std::string& request : server.requests()) {
        EXPECT_NE(
            request.find("Idempotency-Key: delivery-key-1"),
            std::string::npos);
    }

    const auto duplicate = runtime.enqueue_task(
        task_spec(
            decision_id,
            server.url(),
            ActionConnectorKind::Webhook,
            "delivery-key-1"),
        "agent-key",
        &error);
    ASSERT_TRUE(duplicate) << error;
    EXPECT_EQ(duplicate->task_id, delivered->task_id);
    EXPECT_EQ(duplicate->status, ActionTaskStatus::Delivered);
    EXPECT_EQ(node.ledger_entry_count(), 4u);
}

TEST_F(ActionIntegrationFixture, ExhaustedRetriesEnterTheDeadLetterTable) {
    LocalHttpServer server({500, 502});
    const auto [policy_id, decision_id] = create_decision("dead-letter");
    ASSERT_FALSE(policy_id.empty());
    ActionTaskSpec spec = task_spec(
        decision_id,
        server.url(),
        ActionConnectorKind::Webhook,
        "dead-letter-key");
    spec.max_attempts = 2;

    ActionRuntime runtime(node);
    std::string error;
    const auto task = runtime.execute_closed_loop(
        std::move(spec),
        "agent-key",
        &error);
    ASSERT_TRUE(task) << error;
    EXPECT_EQ(task->status, ActionTaskStatus::DeadLetter);
    server.wait();
    ASSERT_TRUE(server.error().empty()) << server.error();
    const auto database = node.find_database("action_delivery");
    ASSERT_TRUE(database);
    const auto dead_letters = database->find_table("dead_letter");
    ASSERT_TRUE(dead_letters);
    ASSERT_EQ(dead_letters->store().size(), 1u);
    EXPECT_EQ(dead_letters->store().at(0)->at("task_id"), task->task_id);
    EXPECT_EQ(node.ledger_entry_count(), 4u);
}

TEST_F(ActionIntegrationFixture, SalesforceAndHubSpotPluginsUseLocalHttp) {
    const std::vector<std::pair<ActionConnectorKind, std::string>> cases = {
        {ActionConnectorKind::Salesforce, "\"OwnerId\":\"owner-7\""},
        {ActionConnectorKind::HubSpot, "\"hubspot_owner_id\":\"owner-7\""}};
    int index = 0;
    for (const auto& [connector, expected_body] : cases) {
        LocalHttpServer server({201});
        const auto [policy_id, decision_id] = create_decision(
            "crm-" + std::to_string(index));
        ASSERT_FALSE(policy_id.empty());
        ActionTaskSpec spec = task_spec(
            decision_id,
            server.url("/crm/tasks"),
            connector,
            "crm-key-" + std::to_string(index));

        ActionRuntime runtime(node);
        std::string error;
        const auto task = runtime.execute_closed_loop(
            std::move(spec),
            "agent-key",
            &error);
        ASSERT_TRUE(task) << error;
        EXPECT_EQ(task->status, ActionTaskStatus::Delivered);
        server.wait();
        ASSERT_TRUE(server.error().empty()) << server.error();
        ASSERT_EQ(server.requests().size(), 1u);
        EXPECT_NE(server.requests().front().find(expected_body), std::string::npos);
        EXPECT_NE(
            server.requests().front().find(
                "Authorization: Bearer local-test-token"),
            std::string::npos);
        ++index;
    }
}

TEST_F(ActionIntegrationFixture, DeliveryRechecksTheKillSwitchBeforeHttp) {
    const auto [policy_id, decision_id] = create_decision("kill-switch");
    ASSERT_FALSE(policy_id.empty());
    std::atomic<int> requests{0};
    ActionRuntime runtime(
        node,
        [&](const zetta::connect::HttpRequest&) {
            ++requests;
            return zetta::connect::HttpResponse{200, "{}", {}};
        });
    std::string error;
    const auto queued = runtime.enqueue_task(
        task_spec(
            decision_id,
            "http://127.0.0.1:1/unused",
            ActionConnectorKind::Webhook,
            "kill-key"),
        "agent-key",
        &error);
    ASSERT_TRUE(queued) << error;

    const auto updated = store.update(
        policy_id,
        policy_config(true),
        std::nullopt,
        "owner",
        "emergency stop",
        &error);
    ASSERT_TRUE(updated) << error;
    const auto blocked = runtime.deliver_task(
        queued->task_id,
        {},
        "agent-key",
        &error);
    ASSERT_TRUE(blocked) << error;
    EXPECT_EQ(blocked->status, ActionTaskStatus::Blocked);
    EXPECT_NE(blocked->last_error.find("kill-switch"), std::string::npos);
    EXPECT_EQ(requests.load(), 0);
    EXPECT_EQ(node.ledger_entry_count(), 0u);
}

TEST_F(ActionIntegrationFixture, HoldoutDecisionCannotEnterTheOutbox) {
    const auto [policy_id, decision_id] = create_decision("holdout", true);
    ASSERT_FALSE(policy_id.empty());
    ActionRuntime runtime(node);
    std::string error;
    const auto task = runtime.enqueue_task(
        task_spec(
            decision_id,
            "http://127.0.0.1:1/unused",
            ActionConnectorKind::Webhook,
            "holdout-key"),
        "agent-key",
        &error);
    EXPECT_FALSE(task);
    EXPECT_NE(error.find("holdout"), std::string::npos);
    const auto database = node.find_database("action_delivery");
    EXPECT_FALSE(database);
}

TEST_F(ActionIntegrationFixture, OutboxRowsReconcileThroughTheNodeCatalog) {
    const auto [policy_id, decision_id] = create_decision("replicated-outbox");
    ASSERT_FALSE(policy_id.empty());
    ActionRuntime runtime(node);
    std::string error;
    const auto task = runtime.enqueue_task(
        task_spec(
            decision_id,
            "http://127.0.0.1:1/unused",
            ActionConnectorKind::Webhook,
            "replicated-key"),
        "agent-key",
        &error);
    ASSERT_TRUE(task) << error;

    zetta::test::TempDir peer_directory;
    ReplicationManager peer_replication{zetta::util::new_uuid()};
    zetta::Node peer_node{
        {peer_directory.path().string(), false},
        &peer_replication};
    ASSERT_TRUE(peer_node.start());
    peer_replication.merge(replication.save());
    peer_node.tick();

    const auto database = peer_node.find_database("action_delivery");
    ASSERT_TRUE(database);
    const auto outbox = database->find_table("outbox");
    ASSERT_TRUE(outbox);
    ASSERT_EQ(outbox->store().size(), 1u);
    EXPECT_EQ(outbox->store().at(0)->at("task_id"), task->task_id);
    EXPECT_FALSE(outbox->store().at(0)->contains("token"));
    peer_node.stop();
}

TEST_F(ActionIntegrationFixture, PublishesDriverReportAndDecisionLog) {
    LocalHttpServer server({200});
    ReportPublicationSpec spec;
    spec.run_id = "run-42";
    spec.title = "Renewal drivers";
    spec.driver_report = {{"top_driver", "usage_decline"}};
    spec.decision_log = {{{"entity_id", "account-1"}, {"action", "call"}}};
    spec.connector.kind = ActionConnectorKind::Webhook;
    spec.connector.endpoint = server.url("/forum/reports");
    spec.idempotency_key = "report-key";
    spec.model_version = "model-v3";
    spec.policy_version = "policy-v4";
    spec.agent_version = "agent-v2";
    spec.cell = "population";
    spec.currency = "USD";

    ActionRuntime runtime(node);
    std::string error;
    const auto publication = runtime.publish_report(
        std::move(spec),
        "report-agent",
        &error);
    ASSERT_TRUE(publication) << error;
    EXPECT_EQ(publication->status, ActionTaskStatus::Delivered);
    server.wait();
    ASSERT_TRUE(server.error().empty()) << server.error();
    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_NE(
        server.requests().front().find("\"driver_report\""),
        std::string::npos);
    EXPECT_NE(
        server.requests().front().find("\"decision_log\""),
        std::string::npos);
    EXPECT_EQ(node.ledger_entry_count(), 2u);
}

// lake#801: the report's network round-trip can be taken off the Node owner.
// prepare_report persists and ledger-records the publication and builds the
// request without sending anything; record_report_attempt books one attempt
// and settles the publication as a dead letter once max_attempts is spent; a
// settled publication prepares no second request.
TEST_F(ActionIntegrationFixture, PreparesAndRecordsReportAttemptsWithoutSending) {
    ReportPublicationSpec spec;
    spec.run_id = "run-801";
    spec.title = "Churn and retention driver report";
    spec.driver_report = {{"drivers", nlohmann::json::array()}};
    spec.connector.kind = ActionConnectorKind::Webhook;
    spec.connector.endpoint = "https://example.com/hooks/actions";
    spec.connector.timeout_seconds = 7;
    spec.idempotency_key = "run-801/driver-report";
    spec.max_attempts = 2;
    spec.model_version = "model-v1";
    spec.policy_version = "1";
    spec.agent_version = "solution-foundation-v1";
    spec.cell = "retention/report";
    spec.currency = "USD";
    const ReportPublicationSpec again = spec;

    // The injected transport fails the test if anything is sent.
    ActionRuntime runtime(node, [](const zetta::connect::HttpRequest&) {
        ADD_FAILURE() << "prepare_report must not send";
        return zetta::connect::HttpResponse{};
    });
    std::string error;
    auto prepared = runtime.prepare_report(spec, "report-agent", &error);
    ASSERT_TRUE(prepared) << error;
    ASSERT_TRUE(prepared->request);
    EXPECT_EQ(prepared->request->url, "https://example.com/hooks/actions");
    EXPECT_EQ(prepared->request->timeout_seconds, 7);
    EXPECT_NE(prepared->request->body.find("publish_driver_report"),
              std::string::npos);
    EXPECT_EQ(prepared->publication.status, ActionTaskStatus::Pending);
    EXPECT_EQ(prepared->publication.attempts, 0u);
    EXPECT_FALSE(prepared->publication.ledger_action_id.empty());

    zetta::connect::HttpResponse unreachable;
    unreachable.error = "could not connect to example.com:443: timed out";
    auto first = runtime.record_report_attempt(
        prepared->publication, unreachable, &error);
    ASSERT_TRUE(first) << error;
    EXPECT_EQ(first->status, ActionTaskStatus::Pending);
    EXPECT_EQ(first->attempts, 1u);
    EXPECT_EQ(first->last_error, unreachable.error);
    auto second = runtime.record_report_attempt(*first, unreachable, &error);
    ASSERT_TRUE(second) << error;
    EXPECT_EQ(second->status, ActionTaskStatus::DeadLetter);
    EXPECT_EQ(second->attempts, 2u);

    auto settled = runtime.prepare_report(again, "report-agent", &error);
    ASSERT_TRUE(settled) << error;
    EXPECT_FALSE(settled->request);
    EXPECT_EQ(settled->publication.status, ActionTaskStatus::DeadLetter);
    EXPECT_EQ(settled->publication.publication_id,
              prepared->publication.publication_id);
}

// #1394 (LSR-161): an action endpoint is author-chosen, so delivery resolves it and
// dials only public addresses unless the node allows the class. A loopback endpoint
// is refused before any connection, by name and by literal.
TEST_F(ActionIntegrationFixture, DeliveryRefusesALoopbackEndpointTheNodeDoesNotAllow) {
    zetta::connect::set_allowed_destinations({});
    IdleListener listener;
    ASSERT_NE(listener.port(), 0);
    for (const std::string host : {"127.0.0.1", "localhost"}) {
        const auto [policy_id, decision_id] = create_decision("refused-" + host);
        ASSERT_FALSE(decision_id.empty());
        ActionTaskSpec spec = task_spec(decision_id, listener.url(host),
                                        ActionConnectorKind::Webhook, "refused-key-" + host);
        spec.max_attempts = 1;
        spec.connector.timeout_seconds = 2;

        ActionRuntime runtime(node);
        std::string error;
        const auto task = runtime.execute_closed_loop(std::move(spec), "agent-key", &error);
        ASSERT_TRUE(task) << error;
        EXPECT_EQ(task->status, ActionTaskStatus::DeadLetter) << host;
        EXPECT_NE(task->last_error.find("destination refused"), std::string::npos)
            << host << ": " << task->last_error;
        EXPECT_NE(task->last_error.find("loopback"), std::string::npos) << task->last_error;
        // The bearer token never left: nothing connected to the listener.
        EXPECT_FALSE(listener.was_dialled()) << host;
    }
}

// The same check guards a solution's report delivery, whose endpoint a writer
// stores and a later run sends its delivery token to.
TEST_F(ActionIntegrationFixture, ReportDeliveryRefusesAPrivateOrMetadataEndpoint) {
    zetta::connect::set_allowed_destinations({});
    IdleListener listener;
    ASSERT_NE(listener.port(), 0);
    const std::vector<std::pair<std::string, std::string>> endpoints = {
        {listener.url("127.0.0.1", "/forum/reports"), "loopback"},
        {"http://169.254.169.254/latest/meta-data/", "metadata"},
        {"http://10.0.0.7:8080/hook", "private"},
        {"http://[::ffff:127.0.0.1]:" + std::to_string(listener.port()) + "/r", "loopback"},
    };
    int index = 0;
    for (const auto& [endpoint, expected] : endpoints) {
        ReportPublicationSpec spec;
        spec.run_id = "run-refused-" + std::to_string(index);
        spec.title = "Renewal drivers";
        spec.driver_report = {{"top_driver", "usage_decline"}};
        spec.decision_log = Json::array();
        spec.connector.kind = ActionConnectorKind::Webhook;
        spec.connector.endpoint = endpoint;
        spec.connector.token = "delivery-token";
        spec.connector.timeout_seconds = 2;
        spec.idempotency_key = "report-refused-" + std::to_string(index++);
        spec.max_attempts = 1;
        spec.model_version = "model-v3";
        spec.policy_version = "policy-v4";
        spec.agent_version = "agent-v2";
        spec.cell = "population";
        spec.currency = "USD";

        ActionRuntime runtime(node);
        std::string error;
        const auto publication = runtime.publish_report(std::move(spec), "report-agent", &error);
        ASSERT_TRUE(publication) << error;
        EXPECT_NE(publication->status, ActionTaskStatus::Delivered) << endpoint;
        EXPECT_NE(publication->last_error.find("destination refused"), std::string::npos)
            << endpoint << ": " << publication->last_error;
        EXPECT_NE(publication->last_error.find(expected), std::string::npos)
            << publication->last_error;
    }
    EXPECT_FALSE(listener.was_dialled());
}

}  // namespace
