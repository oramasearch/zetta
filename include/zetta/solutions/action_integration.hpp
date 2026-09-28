#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/connect/http_transport.hpp"

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

class ActionRuntime {
public:
    explicit ActionRuntime(
        zetta::Node& node,
        zetta::connect::HttpTransport transport = {});

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

    std::optional<ReportPublication> publish_report(
        ReportPublicationSpec spec,
        std::string actor,
        std::string* error = nullptr);

private:
    zetta::Node& node_;
    zetta::connect::HttpTransport transport_;
};

}  // namespace zetta::solutions
