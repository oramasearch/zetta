#include "zetta/api/capabilities.hpp"

#include <string>
#include <utility>

#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/action_integration.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::solutions::ActionConnectorConfig;
using zetta::solutions::ActionRuntime;
using zetta::solutions::ActionTaskSpec;
using zetta::solutions::ReportPublicationSpec;

std::string actor_of(const ApiContext& context) {
    if (!context.acting_key.empty()) {
        return context.acting_key;
    }
    return "local";
}

const nlohmann::json& connector_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"kind", "endpoint"}},
        {"properties",
         {{"kind", {{"type", "string"},
                    {"enum", {"webhook", "salesforce", "hubspot"}}}},
          {"endpoint", {{"type", "string"}, {"minLength", 1}}},
          {"token", {{"type", "string"}}},
          {"timeout_seconds",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 300}}}}}};
    return schema;
}

const nlohmann::json& task_spec_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"decision_id", "owner", "sla_due_at", "connector", "payload",
          "idempotency_key", "model_version", "agent_version", "cell",
          "currency"}},
        {"properties",
         {{"decision_id", {{"type", "string"}, {"minLength", 1}}},
          {"owner", {{"type", "string"}, {"minLength", 1}}},
          {"sla_due_at", {{"type", "string"}, {"minLength", 1}}},
          {"connector", connector_schema()},
          {"payload", {{"type", "object"}}},
          {"idempotency_key", {{"type", "string"}, {"minLength", 1}}},
          {"max_attempts",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
          {"model_version", {{"type", "string"}, {"minLength", 1}}},
          {"agent_version", {{"type", "string"}, {"minLength", 1}}},
          {"cell", {{"type", "string"}, {"minLength", 1}}},
          {"currency", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& deliver_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"task_id"}},
        {"properties",
         {{"task_id", {{"type", "string"}, {"minLength", 1}}},
          {"token", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& publication_spec_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"run_id", "title", "driver_report", "decision_log", "connector",
          "idempotency_key", "model_version", "policy_version",
          "agent_version", "cell", "currency"}},
        {"properties",
         {{"run_id", {{"type", "string"}, {"minLength", 1}}},
          {"title", {{"type", "string"}, {"minLength", 1}}},
          {"driver_report", {{"type", "object"}}},
          {"decision_log", {{"type", "array"},
                            {"items", {{"type", "object"}}}}},
          {"connector", connector_schema()},
          {"idempotency_key", {{"type", "string"}, {"minLength", 1}}},
          {"max_attempts",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
          {"model_version", {{"type", "string"}, {"minLength", 1}}},
          {"policy_version", {{"type", "string"}, {"minLength", 1}}},
          {"agent_version", {{"type", "string"}, {"minLength", 1}}},
          {"cell", {{"type", "string"}, {"minLength", 1}}},
          {"currency", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

ActionConnectorConfig connector_from(const nlohmann::json& value) {
    ActionConnectorConfig connector;
    connector.kind = zetta::solutions::action_connector_kind_from(
        value.at("kind").get<std::string>()).value();
    connector.endpoint = value.at("endpoint").get<std::string>();
    connector.token = value.value("token", std::string{});
    connector.timeout_seconds = value.value("timeout_seconds", 30);
    return connector;
}

ActionTaskSpec task_spec_from(const nlohmann::json& input) {
    ActionTaskSpec spec;
    spec.decision_id = input.at("decision_id").get<std::string>();
    spec.owner = input.at("owner").get<std::string>();
    spec.sla_due_at = input.at("sla_due_at").get<std::string>();
    spec.connector = connector_from(input.at("connector"));
    spec.payload = input.at("payload");
    spec.idempotency_key = input.at("idempotency_key").get<std::string>();
    spec.max_attempts = input.value("max_attempts", std::size_t{3});
    spec.model_version = input.at("model_version").get<std::string>();
    spec.agent_version = input.at("agent_version").get<std::string>();
    spec.cell = input.at("cell").get<std::string>();
    spec.currency = input.at("currency").get<std::string>();
    return spec;
}

ApiResult handle_enqueue(const nlohmann::json& input, ApiContext& context) {
    ActionRuntime runtime(context.node);
    std::string error;
    const auto task = runtime.enqueue_task(
        task_spec_from(input),
        actor_of(context),
        &error);
    if (!task) {
        return ApiResult::failure("invalid_argument", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*task));
}

ApiResult handle_deliver(const nlohmann::json& input, ApiContext& context) {
    ActionRuntime runtime(context.node);
    std::string error;
    const auto task = runtime.deliver_task(
        input.at("task_id").get<std::string>(),
        input.value("token", std::string{}),
        actor_of(context),
        &error);
    if (!task) {
        return ApiResult::failure("invalid_argument", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*task));
}

ApiResult handle_closed_loop(const nlohmann::json& input, ApiContext& context) {
    ActionRuntime runtime(context.node);
    std::string error;
    const auto task = runtime.execute_closed_loop(
        task_spec_from(input),
        actor_of(context),
        &error);
    if (!task) {
        return ApiResult::failure("invalid_argument", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*task));
}

ApiResult handle_publish(const nlohmann::json& input, ApiContext& context) {
    ReportPublicationSpec spec;
    spec.run_id = input.at("run_id").get<std::string>();
    spec.title = input.at("title").get<std::string>();
    spec.driver_report = input.at("driver_report");
    spec.decision_log = input.at("decision_log");
    spec.connector = connector_from(input.at("connector"));
    spec.idempotency_key = input.at("idempotency_key").get<std::string>();
    spec.max_attempts = input.value("max_attempts", std::size_t{3});
    spec.model_version = input.at("model_version").get<std::string>();
    spec.policy_version = input.at("policy_version").get<std::string>();
    spec.agent_version = input.at("agent_version").get<std::string>();
    spec.cell = input.at("cell").get<std::string>();
    spec.currency = input.at("currency").get<std::string>();

    ActionRuntime runtime(context.node);
    std::string error;
    const auto publication = runtime.publish_report(
        std::move(spec),
        actor_of(context),
        &error);
    if (!publication) {
        return ApiResult::failure("invalid_argument", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*publication));
}

}  // namespace

void register_action_capabilities(Registry& registry) {
    registry.add(Capability{
        "publish_report",
        "Publish a driver report",
        "Publish a driver report and decision log to a forum webhook with bounded retries and ledger evidence.",
        publication_spec_schema(),
        zetta::solutions::report_publication_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "publish_report"},
        true,
        false,
        handle_publish});
    registry.add(Capability{
        "enqueue_action_task",
        "Enqueue an action task",
        "Write an owned, SLA-bound task to the replicated action outbox after policy and holdout checks.",
        task_spec_schema(),
        zetta::solutions::action_task_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "enqueue_action_task"},
        true,
        false,
        handle_enqueue});
    // Both send HTTP (#1406, LSR-173): `blocks` runs them on the blocking pool
    // from REST and MCP. ActionRuntime::deliver_task does every outbox, policy
    // and ledger step on the owner through Node::run_on_owner and only the
    // attempts on the pool thread, so a stalled endpoint never holds the owner
    // loop.
    Capability deliver{
        "deliver_action_task",
        "Deliver an action task",
        "Deliver one outbox task with retries, an idempotency key, acknowledgements, and dead-letter handling.",
        deliver_schema(),
        zetta::solutions::action_task_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "deliver_action_task"},
        true,
        false,
        handle_deliver};
    deliver.blocks = true;
    registry.add(std::move(deliver));
    Capability closed_loop{
        "execute_closed_loop",
        "Execute a bounded closed loop",
        "Enqueue and deliver one policy-approved task while rechecking holdout and the policy kill-switch before execution.",
        task_spec_schema(),
        zetta::solutions::action_task_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "execute_closed_loop"},
        true,
        false,
        handle_closed_loop};
    closed_loop.blocks = true;
    registry.add(std::move(closed_loop));
}

}  // namespace zetta::api
