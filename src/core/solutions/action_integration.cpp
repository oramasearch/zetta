#include "zetta/solutions/action_integration.hpp"

#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/audit_ledger.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::solutions {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kDatabaseName = "action_delivery";
constexpr std::string_view kOutboxTableName = "outbox";
constexpr std::string_view kDeadLetterTableName = "dead_letter";
constexpr std::string_view kPublicationTableName = "report_publications";

class CollectingErrorHandler : public nlohmann::json_schema::error_handler {
public:
    void error(
        const nlohmann::json::json_pointer& pointer,
        const nlohmann::json&,
        const std::string& message) override {
        if (failed) {
            return;
        }
        failed = true;
        const std::string location = pointer.to_string();
        reason = (location.empty() ? "/" : location) + ": " + message;
    }

    bool failed = false;
    std::string reason;
};

bool validate(
    nlohmann::json_schema::json_validator& validator,
    const Json& value,
    std::string* error) {
    CollectingErrorHandler handler;
    validator.validate(value, handler);
    if (!handler.failed) {
        return true;
    }
    if (error != nullptr) {
        *error = handler.reason;
    }
    return false;
}

nlohmann::json_schema::json_validator& task_validator() {
    static nlohmann::json_schema::json_validator validator(action_task_schema());
    return validator;
}

nlohmann::json_schema::json_validator& publication_validator() {
    static nlohmann::json_schema::json_validator validator(
        report_publication_schema());
    return validator;
}

std::shared_ptr<zetta::data::Table> ensure_table(
    zetta::Node& node,
    std::string_view name,
    std::string* error) {
    auto database = node.find_database(std::string(kDatabaseName));
    if (!database) {
        database = node.create_database(std::string(kDatabaseName));
    }
    if (!database) {
        if (error != nullptr) {
            *error = "could not create action-delivery database";
        }
        return nullptr;
    }
    auto table = database->find_table(std::string(name));
    if (!table) {
        table = database->create_table(std::string(name));
    }
    if (!table && error != nullptr) {
        *error = "could not create action-delivery table '" +
                 std::string(name) + "'";
    }
    return table;
}

struct StoredRecord {
    std::string row_id;
    Json value;
};

std::optional<StoredRecord> find_record(
    const std::shared_ptr<zetta::data::Table>& table,
    std::string_view field,
    std::string_view value) {
    if (!table) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < table->store().size(); ++index) {
        const auto row = table->store().at(index);
        if (!row || !row->contains(field) || !row->at(field).is_string()) {
            continue;
        }
        if (row->at(field).get<std::string>() == value) {
            return StoredRecord{table->store().id_at(index), *row};
        }
    }
    return std::nullopt;
}

bool persist_task(zetta::Node& node, const ActionTask& task, std::string* error) {
    const Json value = to_json(task);
    if (!validate(task_validator(), value, error)) {
        return false;
    }
    auto table = ensure_table(node, kOutboxTableName, error);
    if (!table) {
        return false;
    }
    if (const auto stored = find_record(table, "task_id", task.task_id)) {
        table->update_row(stored->row_id, value);
    } else {
        table->add_row(value);
    }
    return true;
}

bool persist_publication(
    zetta::Node& node,
    const ReportPublication& publication,
    std::string* error) {
    const Json value = to_json(publication);
    if (!validate(publication_validator(), value, error)) {
        return false;
    }
    auto table = ensure_table(node, kPublicationTableName, error);
    if (!table) {
        return false;
    }
    if (const auto stored = find_record(
            table,
            "publication_id",
            publication.publication_id)) {
        table->update_row(stored->row_id, value);
    } else {
        table->add_row(value);
    }
    return true;
}

bool copy_to_dead_letter(
    zetta::Node& node,
    const ActionTask& task,
    std::string* error) {
    auto table = ensure_table(node, kDeadLetterTableName, error);
    if (!table) {
        return false;
    }
    if (find_record(table, "task_id", task.task_id)) {
        return true;
    }
    table->add_row(to_json(task));
    return true;
}

bool validate_task_spec(const ActionTaskSpec& spec, std::string* error) {
    ActionTask task;
    task.task_id = "validation";
    task.decision_id = spec.decision_id;
    task.owner = spec.owner;
    task.sla_due_at = spec.sla_due_at;
    task.connector = spec.connector.kind;
    task.endpoint = spec.connector.endpoint;
    task.timeout_seconds = spec.connector.timeout_seconds;
    task.payload = spec.payload;
    task.idempotency_key = spec.idempotency_key;
    task.max_attempts = spec.max_attempts;
    task.model_version = spec.model_version;
    task.agent_version = spec.agent_version;
    task.cell = spec.cell;
    task.currency = spec.currency;
    task.created_at = "validation";
    task.updated_at = "validation";
    return spec.max_attempts <= 100 &&
           validate(task_validator(), to_json(task), error);
}

bool validate_publication_spec(
    const ReportPublicationSpec& spec,
    std::string* error) {
    ReportPublication publication;
    publication.publication_id = "validation";
    publication.run_id = spec.run_id;
    publication.title = spec.title;
    publication.driver_report = spec.driver_report;
    publication.decision_log = spec.decision_log;
    publication.endpoint = spec.connector.endpoint;
    publication.idempotency_key = spec.idempotency_key;
    publication.max_attempts = spec.max_attempts;
    publication.model_version = spec.model_version;
    publication.policy_version = spec.policy_version;
    publication.agent_version = spec.agent_version;
    publication.cell = spec.cell;
    publication.currency = spec.currency;
    publication.created_at = "validation";
    publication.updated_at = "validation";
    return spec.max_attempts <= 100 &&
           validate(publication_validator(), to_json(publication), error);
}

void add_common_headers(
    zetta::connect::HttpRequest& request,
    const ActionConnectorMessage& message,
    const ActionConnectorConfig& config) {
    request.method = "POST";
    request.url = config.endpoint;
    // The endpoint is whatever the action or solution author stored, so it may
    // reach only public addresses (plus the classes the node allows), checked on
    // the resolved address at delivery (#1394, connect/destination_policy.hpp).
    request.destinations = zetta::connect::DestinationPolicy::PublicOnly;
    request.timeout_seconds = config.timeout_seconds;
    request.headers.push_back({"Content-Type", "application/json"});
    request.headers.push_back({"Idempotency-Key", message.idempotency_key});
    if (!config.token.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + config.token});
        request.secret_headers.push_back("Authorization");
    }
}

std::string text_or(
    const Json& value,
    std::string_view field,
    std::string fallback) {
    const auto found = value.find(field);
    if (found != value.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return fallback;
}

class WebhookConnector final : public ActionConnector {
public:
    ActionConnectorKind kind() const override {
        return ActionConnectorKind::Webhook;
    }

    zetta::connect::HttpRequest request(
        const ActionConnectorMessage& message,
        const ActionConnectorConfig& config) const override {
        zetta::connect::HttpRequest request;
        add_common_headers(request, message, config);
        request.body = Json{{"idempotency_key", message.idempotency_key},
                            {"owner", message.owner},
                            {"sla_due_at", message.sla_due_at},
                            {"action", message.action},
                            {"entity_id", message.entity_id},
                            {"payload", message.payload}}
                           .dump();
        return request;
    }
};

class SalesforceTaskConnector final : public ActionConnector {
public:
    ActionConnectorKind kind() const override {
        return ActionConnectorKind::Salesforce;
    }

    zetta::connect::HttpRequest request(
        const ActionConnectorMessage& message,
        const ActionConnectorConfig& config) const override {
        zetta::connect::HttpRequest request;
        add_common_headers(request, message, config);
        const std::string activity_date =
            message.sla_due_at.size() >= 10
                ? message.sla_due_at.substr(0, 10)
                : message.sla_due_at;
        request.body = Json{
            {"Subject", text_or(message.payload, "subject", message.action)},
            {"Description",
             text_or(message.payload, "description", message.payload.dump())},
            {"OwnerId", message.owner},
            {"WhatId", message.entity_id},
            {"ActivityDate", activity_date}}
                           .dump();
        return request;
    }
};

class HubSpotTaskConnector final : public ActionConnector {
public:
    ActionConnectorKind kind() const override {
        return ActionConnectorKind::HubSpot;
    }

    zetta::connect::HttpRequest request(
        const ActionConnectorMessage& message,
        const ActionConnectorConfig& config) const override {
        zetta::connect::HttpRequest request;
        add_common_headers(request, message, config);
        request.body = Json{
            {"properties",
             {{"hs_task_subject",
               text_or(message.payload, "subject", message.action)},
              {"hs_task_body",
               text_or(message.payload, "description", message.payload.dump())},
              {"hs_timestamp", message.sla_due_at},
              {"hubspot_owner_id", message.owner}}}}
                           .dump();
        return request;
    }
};

Json acknowledgement_from(const zetta::connect::HttpResponse& response) {
    Json body = Json::parse(response.body, nullptr, false);
    if (body.is_discarded()) {
        body = response.body;
    }
    return {{"http_status", response.status}, {"body", std::move(body)}};
}

std::string response_error(const zetta::connect::HttpResponse& response) {
    if (!response.error.empty()) {
        return response.error;
    }
    return "HTTP " + std::to_string(response.status) +
           (response.body.empty() ? std::string{} : ": " + response.body);
}

void record_delivery_outcome(
    zetta::Node& node,
    const std::string& action_id,
    std::string outcome,
    int status,
    const std::string& currency) {
    OutcomeLedgerValue value;
    value.action_id = action_id;
    value.outcome = std::move(outcome);
    value.value = static_cast<double>(status);
    value.currency = currency;
    value.source = OutcomeSource::Connector;
    value.occurred_at = zetta::util::now_iso8601();
    node.record_outcome(std::move(value));
}

std::shared_ptr<LedgerEntry> record_task_action(
    zetta::Node& node,
    const ActionTask& task,
    const DecisionRecord& decision,
    const std::string& actor) {
    ActionLedgerValue value;
    value.subject = decision.entity_id;
    value.score_id = decision.id;
    value.score = decision.score;
    value.decision = decision.status;
    value.action = decision.action;
    value.actor_kind = ActionActorKind::Agent;
    value.actor = actor.empty() ? "local" : actor;
    value.model_version = task.model_version;
    value.policy_version = decision.policy_id + ":" +
                           std::to_string(decision.policy_version);
    value.agent_version = task.agent_version;
    value.cell = task.cell;
    value.cost = decision.cost;
    value.currency = task.currency;
    value.score_at = decision.evidence.value("score_at", decision.created);
    value.decision_at = decision.created;
    value.action_at = zetta::util::now_iso8601();
    return node.record_action(std::move(value));
}

std::shared_ptr<LedgerEntry> record_report_action(
    zetta::Node& node,
    const ReportPublication& publication,
    const std::string& actor) {
    ActionLedgerValue value;
    value.subject = publication.run_id;
    value.score_id = publication.run_id;
    value.decision = "publish";
    value.action = "publish_driver_report";
    value.actor_kind = ActionActorKind::Agent;
    value.actor = actor.empty() ? "local" : actor;
    value.model_version = publication.model_version;
    value.policy_version = publication.policy_version;
    value.agent_version = publication.agent_version;
    value.cell = publication.cell;
    value.currency = publication.currency;
    const std::string now = zetta::util::now_iso8601();
    value.score_at = now;
    value.decision_at = now;
    value.action_at = now;
    return node.record_action(std::move(value));
}

std::optional<DecisionPolicyStore> policy_store_for(
    zetta::Node& node,
    std::string* error) {
    if (node.replication() == nullptr) {
        if (error != nullptr) {
            *error = "action delivery requires a replicated node";
        }
        return std::nullopt;
    }
    return DecisionPolicyStore(*node.replication(), &node);
}

zetta::connect::HttpTransport resolve_transport(
    const zetta::connect::HttpTransport& injected) {
    if (injected) {
        return injected;
    }
    return zetta::connect::default_http_transport();
}

}  // namespace

std::string_view to_string(ActionConnectorKind kind) {
    switch (kind) {
        case ActionConnectorKind::Webhook:
            return "webhook";
        case ActionConnectorKind::Salesforce:
            return "salesforce";
        case ActionConnectorKind::HubSpot:
            return "hubspot";
    }
    return "webhook";
}

std::string_view to_string(ActionTaskStatus status) {
    switch (status) {
        case ActionTaskStatus::Pending:
            return "pending";
        case ActionTaskStatus::Delivered:
            return "delivered";
        case ActionTaskStatus::Blocked:
            return "blocked";
        case ActionTaskStatus::DeadLetter:
            return "dead_letter";
    }
    return "pending";
}

std::optional<ActionConnectorKind> action_connector_kind_from(
    std::string_view value) {
    if (value == "webhook") {
        return ActionConnectorKind::Webhook;
    }
    if (value == "salesforce") {
        return ActionConnectorKind::Salesforce;
    }
    if (value == "hubspot") {
        return ActionConnectorKind::HubSpot;
    }
    return std::nullopt;
}

std::optional<ActionTaskStatus> action_task_status_from(
    std::string_view value) {
    if (value == "pending") {
        return ActionTaskStatus::Pending;
    }
    if (value == "delivered") {
        return ActionTaskStatus::Delivered;
    }
    if (value == "blocked") {
        return ActionTaskStatus::Blocked;
    }
    if (value == "dead_letter") {
        return ActionTaskStatus::DeadLetter;
    }
    return std::nullopt;
}

Json to_json(const ActionTask& task) {
    return {{"task_id", task.task_id},
            {"decision_id", task.decision_id},
            {"owner", task.owner},
            {"sla_due_at", task.sla_due_at},
            {"connector", to_string(task.connector)},
            {"endpoint", task.endpoint},
            {"timeout_seconds", task.timeout_seconds},
            {"payload", task.payload},
            {"idempotency_key", task.idempotency_key},
            {"max_attempts", task.max_attempts},
            {"attempts", task.attempts},
            {"status", to_string(task.status)},
            {"ledger_action_id", task.ledger_action_id},
            {"last_error", task.last_error},
            {"acknowledgement", task.acknowledgement},
            {"model_version", task.model_version},
            {"agent_version", task.agent_version},
            {"cell", task.cell},
            {"currency", task.currency},
            {"created_at", task.created_at},
            {"updated_at", task.updated_at}};
}

Json to_json(const ReportPublication& publication) {
    return {{"publication_id", publication.publication_id},
            {"run_id", publication.run_id},
            {"title", publication.title},
            {"driver_report", publication.driver_report},
            {"decision_log", publication.decision_log},
            {"endpoint", publication.endpoint},
            {"idempotency_key", publication.idempotency_key},
            {"max_attempts", publication.max_attempts},
            {"attempts", publication.attempts},
            {"status", to_string(publication.status)},
            {"ledger_action_id", publication.ledger_action_id},
            {"last_error", publication.last_error},
            {"acknowledgement", publication.acknowledgement},
            {"model_version", publication.model_version},
            {"policy_version", publication.policy_version},
            {"agent_version", publication.agent_version},
            {"cell", publication.cell},
            {"currency", publication.currency},
            {"created_at", publication.created_at},
            {"updated_at", publication.updated_at}};
}

const Json& action_task_schema() {
    static const Json schema = Json::parse(R"JSON({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ActionTask",
      "type": "object",
      "additionalProperties": false,
      "required": ["task_id", "decision_id", "owner", "sla_due_at",
        "connector", "endpoint", "timeout_seconds", "payload", "idempotency_key",
        "max_attempts", "attempts", "status", "ledger_action_id",
        "last_error", "acknowledgement", "model_version", "agent_version",
        "cell", "currency", "created_at", "updated_at"],
      "properties": {
        "task_id": {"type": "string", "minLength": 1},
        "decision_id": {"type": "string", "minLength": 1},
        "owner": {"type": "string", "minLength": 1},
        "sla_due_at": {"type": "string", "minLength": 1},
        "connector": {"type": "string", "enum": ["webhook", "salesforce", "hubspot"]},
        "endpoint": {"type": "string", "minLength": 1},
        "timeout_seconds": {"type": "integer", "minimum": 1, "maximum": 300},
        "payload": {"type": "object"},
        "idempotency_key": {"type": "string", "minLength": 1},
        "max_attempts": {"type": "integer", "minimum": 1, "maximum": 100},
        "attempts": {"type": "integer", "minimum": 0},
        "status": {"type": "string", "enum": ["pending", "delivered", "blocked", "dead_letter"]},
        "ledger_action_id": {"type": "string"},
        "last_error": {"type": "string"},
        "acknowledgement": {"type": "object"},
        "model_version": {"type": "string", "minLength": 1},
        "agent_version": {"type": "string", "minLength": 1},
        "cell": {"type": "string", "minLength": 1},
        "currency": {"type": "string", "minLength": 1},
        "created_at": {"type": "string", "minLength": 1},
        "updated_at": {"type": "string", "minLength": 1}
      }
    })JSON");
    return schema;
}

const Json& report_publication_schema() {
    static const Json schema = Json::parse(R"JSON({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ReportPublication",
      "type": "object",
      "additionalProperties": false,
      "required": ["publication_id", "run_id", "title", "driver_report",
        "decision_log", "endpoint", "idempotency_key", "max_attempts",
        "attempts", "status", "ledger_action_id", "last_error",
        "acknowledgement", "model_version", "policy_version",
        "agent_version", "cell", "currency", "created_at", "updated_at"],
      "properties": {
        "publication_id": {"type": "string", "minLength": 1},
        "run_id": {"type": "string", "minLength": 1},
        "title": {"type": "string", "minLength": 1},
        "driver_report": {"type": "object"},
        "decision_log": {"type": "array", "items": {"type": "object"}},
        "endpoint": {"type": "string", "minLength": 1},
        "idempotency_key": {"type": "string", "minLength": 1},
        "max_attempts": {"type": "integer", "minimum": 1, "maximum": 100},
        "attempts": {"type": "integer", "minimum": 0},
        "status": {"type": "string", "enum": ["pending", "delivered", "blocked", "dead_letter"]},
        "ledger_action_id": {"type": "string"},
        "last_error": {"type": "string"},
        "acknowledgement": {"type": "object"},
        "model_version": {"type": "string", "minLength": 1},
        "policy_version": {"type": "string", "minLength": 1},
        "agent_version": {"type": "string", "minLength": 1},
        "cell": {"type": "string", "minLength": 1},
        "currency": {"type": "string", "minLength": 1},
        "created_at": {"type": "string", "minLength": 1},
        "updated_at": {"type": "string", "minLength": 1}
      }
    })JSON");
    return schema;
}

std::optional<ActionTask> parse_action_task(
    const Json& value,
    std::string* error) {
    if (!validate(task_validator(), value, error)) {
        return std::nullopt;
    }
    ActionTask task;
    task.task_id = value.at("task_id").get<std::string>();
    task.decision_id = value.at("decision_id").get<std::string>();
    task.owner = value.at("owner").get<std::string>();
    task.sla_due_at = value.at("sla_due_at").get<std::string>();
    task.connector = action_connector_kind_from(
        value.at("connector").get<std::string>()).value();
    task.endpoint = value.at("endpoint").get<std::string>();
    task.timeout_seconds = value.at("timeout_seconds").get<int>();
    task.payload = value.at("payload");
    task.idempotency_key = value.at("idempotency_key").get<std::string>();
    task.max_attempts = value.at("max_attempts").get<std::size_t>();
    task.attempts = value.at("attempts").get<std::size_t>();
    task.status = action_task_status_from(
        value.at("status").get<std::string>()).value();
    task.ledger_action_id = value.at("ledger_action_id").get<std::string>();
    task.last_error = value.at("last_error").get<std::string>();
    task.acknowledgement = value.at("acknowledgement");
    task.model_version = value.at("model_version").get<std::string>();
    task.agent_version = value.at("agent_version").get<std::string>();
    task.cell = value.at("cell").get<std::string>();
    task.currency = value.at("currency").get<std::string>();
    task.created_at = value.at("created_at").get<std::string>();
    task.updated_at = value.at("updated_at").get<std::string>();
    return task;
}

std::optional<ReportPublication> parse_report_publication(
    const Json& value,
    std::string* error) {
    if (!validate(publication_validator(), value, error)) {
        return std::nullopt;
    }
    ReportPublication publication;
    publication.publication_id = value.at("publication_id").get<std::string>();
    publication.run_id = value.at("run_id").get<std::string>();
    publication.title = value.at("title").get<std::string>();
    publication.driver_report = value.at("driver_report");
    publication.decision_log = value.at("decision_log");
    publication.endpoint = value.at("endpoint").get<std::string>();
    publication.idempotency_key =
        value.at("idempotency_key").get<std::string>();
    publication.max_attempts = value.at("max_attempts").get<std::size_t>();
    publication.attempts = value.at("attempts").get<std::size_t>();
    publication.status = action_task_status_from(
        value.at("status").get<std::string>()).value();
    publication.ledger_action_id =
        value.at("ledger_action_id").get<std::string>();
    publication.last_error = value.at("last_error").get<std::string>();
    publication.acknowledgement = value.at("acknowledgement");
    publication.model_version = value.at("model_version").get<std::string>();
    publication.policy_version = value.at("policy_version").get<std::string>();
    publication.agent_version = value.at("agent_version").get<std::string>();
    publication.cell = value.at("cell").get<std::string>();
    publication.currency = value.at("currency").get<std::string>();
    publication.created_at = value.at("created_at").get<std::string>();
    publication.updated_at = value.at("updated_at").get<std::string>();
    return publication;
}

std::unique_ptr<ActionConnector> make_action_connector(
    ActionConnectorKind kind) {
    switch (kind) {
        case ActionConnectorKind::Webhook:
            return std::make_unique<WebhookConnector>();
        case ActionConnectorKind::Salesforce:
            return std::make_unique<SalesforceTaskConnector>();
        case ActionConnectorKind::HubSpot:
            return std::make_unique<HubSpotTaskConnector>();
    }
    return nullptr;
}

ActionRuntime::ActionRuntime(
    zetta::Node& node,
    zetta::connect::HttpTransport transport)
    : node_(node),
      transport_(std::move(transport)) {}

std::optional<ActionTask> ActionRuntime::enqueue_task(
    ActionTaskSpec spec,
    std::string,
    std::string* error) {
    if (!validate_task_spec(spec, error)) {
        if (error != nullptr && error->empty()) {
            *error = "max_attempts must not exceed 100";
        }
        return std::nullopt;
    }
    auto store = policy_store_for(node_, error);
    if (!store) {
        return std::nullopt;
    }
    const auto decision = store->get_decision(spec.decision_id);
    if (!decision) {
        if (error != nullptr) {
            *error = "no decision with id '" + spec.decision_id + "'";
        }
        return std::nullopt;
    }
    if (const auto blocked = store->delivery_blocker(*decision)) {
        if (error != nullptr) {
            *error = *blocked;
        }
        return std::nullopt;
    }

    auto table = ensure_table(node_, kOutboxTableName, error);
    if (!table) {
        return std::nullopt;
    }
    if (const auto existing = find_record(
            table,
            "idempotency_key",
            spec.idempotency_key)) {
        return parse_action_task(existing->value, error);
    }

    ActionTask task;
    task.task_id = zetta::util::new_uuid();
    task.decision_id = std::move(spec.decision_id);
    task.owner = std::move(spec.owner);
    task.sla_due_at = std::move(spec.sla_due_at);
    task.connector = spec.connector.kind;
    task.endpoint = std::move(spec.connector.endpoint);
    task.timeout_seconds = spec.connector.timeout_seconds;
    task.payload = std::move(spec.payload);
    task.idempotency_key = std::move(spec.idempotency_key);
    task.max_attempts = spec.max_attempts;
    task.model_version = std::move(spec.model_version);
    task.agent_version = std::move(spec.agent_version);
    task.cell = std::move(spec.cell);
    task.currency = std::move(spec.currency);
    task.created_at = zetta::util::now_iso8601();
    task.updated_at = task.created_at;
    if (!persist_task(node_, task, error)) {
        return std::nullopt;
    }
    return task;
}

std::optional<ActionTask> ActionRuntime::find_task(
    const std::string& task_id,
    std::string* error) const {
    const auto database = node_.find_database(std::string(kDatabaseName));
    const auto table = database
                           ? database->find_table(std::string(kOutboxTableName))
                           : nullptr;
    const auto record = find_record(table, "task_id", task_id);
    if (!record) {
        if (error != nullptr) {
            *error = "no action task with id '" + task_id + "'";
        }
        return std::nullopt;
    }
    return parse_action_task(record->value, error);
}

std::optional<ActionTask> ActionRuntime::deliver_task(
    const std::string& task_id,
    std::string token,
    std::string actor,
    std::string* error) {
    auto task = find_task(task_id, error);
    if (!task) {
        return std::nullopt;
    }
    if (task->status == ActionTaskStatus::Delivered ||
        task->status == ActionTaskStatus::DeadLetter) {
        return task;
    }
    auto store = policy_store_for(node_, error);
    if (!store) {
        return std::nullopt;
    }
    const auto decision = store->get_decision(task->decision_id);
    if (!decision) {
        if (error != nullptr) {
            *error = "decision no longer exists";
        }
        return std::nullopt;
    }
    const zetta::connect::HttpTransport transport =
        resolve_transport(transport_);
    if (!transport) {
        if (error != nullptr) {
            *error = "no outbound HTTP transport is installed";
        }
        return std::nullopt;
    }
    const auto connector = make_action_connector(task->connector);
    if (!connector) {
        if (error != nullptr) {
            *error = "unknown action connector";
        }
        return std::nullopt;
    }

    ActionConnectorConfig config;
    config.kind = task->connector;
    config.endpoint = task->endpoint;
    config.token = std::move(token);
    config.timeout_seconds = task->timeout_seconds;
    while (task->attempts < task->max_attempts) {
        if (const auto blocked = store->delivery_blocker(*decision)) {
            task->status = ActionTaskStatus::Blocked;
            task->last_error = *blocked;
            task->updated_at = zetta::util::now_iso8601();
            if (!persist_task(node_, *task, error)) {
                return std::nullopt;
            }
            return task;
        }
        if (task->ledger_action_id.empty()) {
            const auto action = record_task_action(
                node_,
                *task,
                *decision,
                actor);
            if (!action) {
                if (error != nullptr) {
                    *error = "could not record action delivery in the ledger";
                }
                return std::nullopt;
            }
            task->ledger_action_id = action->id();
        }

        ActionConnectorMessage message;
        message.idempotency_key = task->idempotency_key;
        message.owner = task->owner;
        message.sla_due_at = task->sla_due_at;
        message.action = decision->action;
        message.entity_id = decision->entity_id;
        message.payload = task->payload;
        const auto response = transport(connector->request(message, config));
        ++task->attempts;
        task->updated_at = zetta::util::now_iso8601();
        if (response.ok()) {
            task->status = ActionTaskStatus::Delivered;
            task->last_error.clear();
            task->acknowledgement = acknowledgement_from(response);
            record_delivery_outcome(
                node_,
                task->ledger_action_id,
                "acknowledged",
                response.status,
                task->currency);
            if (!persist_task(node_, *task, error)) {
                return std::nullopt;
            }
            return task;
        }
        task->last_error = response_error(response);
        record_delivery_outcome(
            node_,
            task->ledger_action_id,
            "delivery_failed",
            response.status,
            task->currency);
        if (!persist_task(node_, *task, error)) {
            return std::nullopt;
        }
    }

    task->status = ActionTaskStatus::DeadLetter;
    task->updated_at = zetta::util::now_iso8601();
    record_delivery_outcome(
        node_,
        task->ledger_action_id,
        "dead_letter",
        0,
        task->currency);
    if (!persist_task(node_, *task, error)) {
        return std::nullopt;
    }
    if (!copy_to_dead_letter(node_, *task, error)) {
        return std::nullopt;
    }
    return task;
}

std::optional<ActionTask> ActionRuntime::execute_closed_loop(
    ActionTaskSpec spec,
    std::string actor,
    std::string* error) {
    const std::string token = spec.connector.token;
    const auto task = enqueue_task(std::move(spec), actor, error);
    if (!task) {
        return std::nullopt;
    }
    return deliver_task(task->task_id, token, std::move(actor), error);
}

std::optional<PreparedReportPublication> ActionRuntime::prepare_report(
    ReportPublicationSpec spec,
    std::string actor,
    std::string* error) {
    if (spec.connector.kind != ActionConnectorKind::Webhook) {
        if (error != nullptr) {
            *error = "report publication requires the webhook connector";
        }
        return std::nullopt;
    }
    if (!validate_publication_spec(spec, error)) {
        if (error != nullptr && error->empty()) {
            *error = "max_attempts must not exceed 100";
        }
        return std::nullopt;
    }
    auto table = ensure_table(node_, kPublicationTableName, error);
    if (!table) {
        return std::nullopt;
    }
    PreparedReportPublication prepared;
    ReportPublication& publication = prepared.publication;
    const auto existing = find_record(
        table,
        "idempotency_key",
        spec.idempotency_key);
    if (existing) {
        const auto parsed = parse_report_publication(existing->value, error);
        if (!parsed) {
            return std::nullopt;
        }
        publication = *parsed;
        if (publication.status == ActionTaskStatus::Delivered ||
            publication.status == ActionTaskStatus::DeadLetter) {
            return prepared;
        }
    }

    if (!existing) {
        publication.publication_id = zetta::util::new_uuid();
        publication.run_id = std::move(spec.run_id);
        publication.title = std::move(spec.title);
        publication.driver_report = std::move(spec.driver_report);
        publication.decision_log = std::move(spec.decision_log);
        publication.endpoint = spec.connector.endpoint;
        publication.idempotency_key = std::move(spec.idempotency_key);
        publication.max_attempts = spec.max_attempts;
        publication.model_version = std::move(spec.model_version);
        publication.policy_version = std::move(spec.policy_version);
        publication.agent_version = std::move(spec.agent_version);
        publication.cell = std::move(spec.cell);
        publication.currency = std::move(spec.currency);
        publication.created_at = zetta::util::now_iso8601();
        publication.updated_at = publication.created_at;
        if (!persist_publication(node_, publication, error)) {
            return std::nullopt;
        }
    }
    if (publication.ledger_action_id.empty()) {
        const auto action = record_report_action(node_, publication, actor);
        if (!action) {
            if (error != nullptr) {
                *error = "could not record report publication in the ledger";
            }
            return std::nullopt;
        }
        publication.ledger_action_id = action->id();
    }

    const auto connector = make_action_connector(ActionConnectorKind::Webhook);
    ActionConnectorMessage message;
    message.idempotency_key = publication.idempotency_key;
    message.owner = "forum";
    message.action = "publish_driver_report";
    message.entity_id = publication.run_id;
    message.payload = {{"publication_id", publication.publication_id},
                       {"title", publication.title},
                       {"driver_report", publication.driver_report},
                       {"decision_log", publication.decision_log}};
    ActionConnectorConfig connector_config = spec.connector;
    connector_config.endpoint = publication.endpoint;
    prepared.request = connector->request(message, connector_config);
    return prepared;
}

std::optional<ReportPublication> ActionRuntime::record_report_attempt(
    ReportPublication publication,
    const zetta::connect::HttpResponse& response,
    std::string* error) {
    ++publication.attempts;
    publication.updated_at = zetta::util::now_iso8601();
    if (response.ok()) {
        publication.status = ActionTaskStatus::Delivered;
        publication.acknowledgement = acknowledgement_from(response);
        publication.last_error.clear();
        record_delivery_outcome(
            node_,
            publication.ledger_action_id,
            "acknowledged",
            response.status,
            publication.currency);
        if (!persist_publication(node_, publication, error)) {
            return std::nullopt;
        }
        return publication;
    }
    publication.last_error = response_error(response);
    record_delivery_outcome(
        node_,
        publication.ledger_action_id,
        "delivery_failed",
        response.status,
        publication.currency);
    if (publication.attempts >= publication.max_attempts) {
        publication.status = ActionTaskStatus::DeadLetter;
        record_delivery_outcome(
            node_,
            publication.ledger_action_id,
            "dead_letter",
            0,
            publication.currency);
    }
    if (!persist_publication(node_, publication, error)) {
        return std::nullopt;
    }
    return publication;
}

std::optional<ReportPublication> ActionRuntime::publish_report(
    ReportPublicationSpec spec,
    std::string actor,
    std::string* error) {
    if (!resolve_transport(transport_) &&
        spec.connector.kind == ActionConnectorKind::Webhook) {
        // Refuse before anything is persisted, as before the prepare/record
        // split, unless the publication is already settled (prepare_report
        // returns a settled one without a request).
        auto table = ensure_table(node_, kPublicationTableName, error);
        if (!table) {
            return std::nullopt;
        }
        const auto existing = find_record(
            table, "idempotency_key", spec.idempotency_key);
        if (!existing) {
            if (error != nullptr) {
                *error = "no outbound HTTP transport is installed";
            }
            return std::nullopt;
        }
    }
    auto prepared = prepare_report(std::move(spec), std::move(actor), error);
    if (!prepared) {
        return std::nullopt;
    }
    if (!prepared->request) {
        return std::move(prepared->publication);
    }
    const zetta::connect::HttpTransport transport =
        resolve_transport(transport_);
    if (!transport) {
        if (error != nullptr) {
            *error = "no outbound HTTP transport is installed";
        }
        return std::nullopt;
    }
    ReportPublication publication = std::move(prepared->publication);
    if (publication.attempts >= publication.max_attempts) {
        // A record left pending with no attempts remaining settles now.
        publication.status = ActionTaskStatus::DeadLetter;
        publication.updated_at = zetta::util::now_iso8601();
        record_delivery_outcome(
            node_,
            publication.ledger_action_id,
            "dead_letter",
            0,
            publication.currency);
        if (!persist_publication(node_, publication, error)) {
            return std::nullopt;
        }
        return publication;
    }
    while (publication.status == ActionTaskStatus::Pending) {
        auto recorded = record_report_attempt(
            std::move(publication), transport(*prepared->request), error);
        if (!recorded) {
            return std::nullopt;
        }
        publication = std::move(*recorded);
    }
    return publication;
}

zetta::runtime::task<zetta::connect::HttpResponse> send_report_request(
    zetta::connect::HttpRequest request) {
    // Copies, so a transport swapped while this request is in flight cannot
    // change the function under it.
    const zetta::connect::AsyncHttpTransport asynchronous =
        zetta::connect::default_async_http_transport();
    if (asynchronous) {
        // The error_code half is always clear: a transport failure arrives in
        // HttpResponse::error (http_transport.hpp).
        auto [transport_error, response] = co_await asynchronous(request);
        (void)transport_error;
        co_return std::move(response);
    }
    zetta::connect::HttpTransport synchronous =
        zetta::connect::default_http_transport();
    if (synchronous) {
        // The synchronous transport blocks the thread it runs on, so it runs on
        // a runtime pool worker — never on the Node owner (lake#801).
        co_return co_await zetta::runtime::offload(
            [synchronous = std::move(synchronous),
             request = std::move(request)] { return synchronous(request); });
    }
    zetta::connect::HttpResponse response;
    response.error = "no outbound HTTP transport is installed";
    co_return response;
}

}  // namespace zetta::solutions
