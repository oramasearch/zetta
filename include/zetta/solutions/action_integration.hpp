// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/connect/http_transport.hpp"
#include "zetta/runtime.hpp"

namespace zetta {
class Node;
}

namespace zetta::solutions {

enum class ActionConnectorKind {
    Webhook,
    Salesforce,
    HubSpot,
};

enum class ActionTaskStatus {
    Pending,
    Delivered,
    Blocked,
    DeadLetter,
};

std::string_view to_string(ActionConnectorKind kind);
std::string_view to_string(ActionTaskStatus status);
std::optional<ActionConnectorKind> action_connector_kind_from(
    std::string_view value);
std::optional<ActionTaskStatus> action_task_status_from(
    std::string_view value);

struct ActionConnectorConfig {
    ActionConnectorKind kind = ActionConnectorKind::Webhook;
    std::string endpoint;
    std::string token;
    int timeout_seconds = 30;
};

struct ActionTaskSpec {
    std::string decision_id;
    std::string owner;
    std::string sla_due_at;
    ActionConnectorConfig connector;
    nlohmann::json payload = nlohmann::json::object();
    std::string idempotency_key;
    std::size_t max_attempts = 3;
    std::string model_version;
    std::string agent_version;
    std::string cell;
    std::string currency;
};

struct ActionTask {
    std::string task_id;
    std::string decision_id;
    std::string owner;
    std::string sla_due_at;
    ActionConnectorKind connector = ActionConnectorKind::Webhook;
    std::string endpoint;
    int timeout_seconds = 30;
    nlohmann::json payload = nlohmann::json::object();
    std::string idempotency_key;
    std::size_t max_attempts = 3;
    std::size_t attempts = 0;
    ActionTaskStatus status = ActionTaskStatus::Pending;
    std::string ledger_action_id;
    std::string last_error;
    nlohmann::json acknowledgement = nlohmann::json::object();
    std::string model_version;
    std::string agent_version;
    std::string cell;
    std::string currency;
    std::string created_at;
    std::string updated_at;
    // Immutable idempotency binding (#1410); empty on readable legacy rows.
    std::string actor;
    std::string spec_digest;
};

struct ReportPublicationSpec {
    std::string run_id;
    std::string title;
    nlohmann::json driver_report = nlohmann::json::object();
    nlohmann::json decision_log = nlohmann::json::array();
    ActionConnectorConfig connector;
    std::string idempotency_key;
    std::size_t max_attempts = 3;
    std::string model_version;
    std::string policy_version;
    std::string agent_version;
    std::string cell;
    std::string currency;
};

struct ReportPublication {
    std::string publication_id;
    std::string run_id;
    std::string title;
    nlohmann::json driver_report = nlohmann::json::object();
    nlohmann::json decision_log = nlohmann::json::array();
    std::string endpoint;
    std::string idempotency_key;
    std::size_t max_attempts = 3;
    std::size_t attempts = 0;
    ActionTaskStatus status = ActionTaskStatus::Pending;
    std::string ledger_action_id;
    std::string last_error;
    nlohmann::json acknowledgement = nlohmann::json::object();
    std::string model_version;
    std::string policy_version;
    std::string agent_version;
    std::string cell;
    std::string currency;
    std::string created_at;
    std::string updated_at;
    std::string actor;
    std::string spec_digest;
    int timeout_seconds = 30;
};

nlohmann::json to_json(const ActionTask& task);
nlohmann::json to_json(const ReportPublication& publication);

const nlohmann::json& action_task_schema();
const nlohmann::json& report_publication_schema();

std::optional<ActionTask> parse_action_task(
    const nlohmann::json& value,
    std::string* error = nullptr);
std::optional<ReportPublication> parse_report_publication(
    const nlohmann::json& value,
    std::string* error = nullptr);

struct ActionConnectorMessage {
    std::string idempotency_key;
    std::string owner;
    std::string sla_due_at;
    std::string action;
    std::string entity_id;
    nlohmann::json payload = nlohmann::json::object();
};

class ActionConnector {
public:
    virtual ~ActionConnector() = default;

    virtual ActionConnectorKind kind() const = 0;
    virtual zetta::connect::HttpRequest request(
        const ActionConnectorMessage& message,
        const ActionConnectorConfig& config) const = 0;
};

std::unique_ptr<ActionConnector> make_action_connector(
    ActionConnectorKind kind);

// A report publication made ready on the Node owner thread: persisted, recorded
// in the action ledger, and with its outbound request built. `request` is empty
// when the publication is already settled (delivered or dead-lettered by an
// earlier call with the same idempotency key), so nothing is sent again.
//
// The split exists so the network round-trip can run OFF the owner thread
// (lake#801): `prepare_report` and `record_report_attempt` touch the Node and
// run on the owner; sending `request` touches nothing and runs anywhere.
struct PreparedReportPublication {
    ReportPublication publication;
    std::optional<zetta::connect::HttpRequest> request;
};

// Sends one prepared report request: the async default transport when one is
// installed, else the synchronous default run on a runtime pool worker, else a
// response whose `error` says no transport is installed. Never touches a Node,
// so it is safe from any coroutine; never blocks the Node owner.
zetta::runtime::task<zetta::connect::HttpResponse> send_report_request(
    zetta::connect::HttpRequest request);

class ActionRuntime {
public:
    explicit ActionRuntime(
        zetta::Node& node,
        zetta::connect::HttpTransport transport = {});

    // Keys bind the nonempty actor and complete non-secret specification.
    // Reuse, direct delivery, and report preparation reject missing/mismatched
    // bindings before using credentials; see docs/solutions/action-integration.md (#1410).
    std::optional<ActionTask> enqueue_task(
        ActionTaskSpec spec,
        std::string actor,
        std::string* error = nullptr);
    std::optional<ActionTask> find_task(
        const std::string& task_id,
        std::string* error = nullptr) const;
    std::optional<ActionTask> deliver_task(
        const std::string& task_id,
        std::string token,
        std::string actor,
        std::string* error = nullptr);
    std::optional<ActionTask> execute_closed_loop(
        ActionTaskSpec spec,
        std::string actor,
        std::string* error = nullptr);

    // Validate, persist and ledger-record a report publication and build the
    // request to send, without sending it. Owner thread.
    std::optional<PreparedReportPublication> prepare_report(
        ReportPublicationSpec spec,
        std::string actor,
        std::string* error = nullptr);

    // Record one delivery attempt's response against `publication` (owner
    // thread): an acknowledged response settles it as Delivered; a failure is
    // recorded with its reason and, once `max_attempts` is reached, settles it
    // as DeadLetter. Returns the updated publication.
    std::optional<ReportPublication> record_report_attempt(
        ReportPublication publication,
        const zetta::connect::HttpResponse& response,
        std::string* error = nullptr);

    // prepare_report, then every attempt through the runtime's transport, on
    // the calling thread. Blocks for the whole delivery: never call it inside a
    // Node owner tick for a delivery a request is waiting on (lake#801) — use
    // prepare_report + send_report_request + record_report_attempt instead.
    std::optional<ReportPublication> publish_report(
        ReportPublicationSpec spec,
        std::string actor,
        std::string* error = nullptr);

private:
    zetta::Node& node_;
    zetta::connect::HttpTransport transport_;
};

}  // namespace zetta::solutions
