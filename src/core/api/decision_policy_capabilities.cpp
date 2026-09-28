#include "zetta/api/capabilities.hpp"

#include <optional>
#include <string>
#include <utility>

#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/decision_policy.hpp"

namespace zetta::api {

namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::solutions::BatchDecisionOptions;
using zetta::solutions::DecisionPolicyStore;

std::string actor_of(const ApiContext& context) {
    if (!context.acting_key.empty()) {
        return context.acting_key;
    }
    return "local";
}

std::optional<DecisionPolicyStore> store_of(ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return std::nullopt;
    }
    return DecisionPolicyStore(*context.node.replication(), &context.node);
}

const nlohmann::json& policy_id_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"policy_id"}},
        {"properties",
         {{"policy_id", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& create_policy_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"name", "config"}},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1}}},
          {"config", zetta::solutions::decision_policy_config_schema()}}}};
    return schema;
}

const nlohmann::json& update_policy_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"policy_id"}},
        {"oneOf",
         {nlohmann::json{{"required", {"config"}}},
          nlohmann::json{{"required", {"tuning"}}}}},
        {"properties",
         {{"policy_id", {{"type", "string"}, {"minLength", 1}}},
          {"config", zetta::solutions::decision_policy_config_schema()},
          {"tuning",
           {{"type", "object"},
            {"minProperties", 1},
            {"additionalProperties", {{"type", "number"}}}}},
          {"reason", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& decide_schema() {
    static const nlohmann::json entity_schema = {
        {"type", "object"},
        {"required", {"entity_id", "score"}},
        {"properties",
         {{"entity_id",
           {{"oneOf",
             nlohmann::json::array(
                 {nlohmann::json{{"type", "string"}},
                  nlohmann::json{{"type", "number"}}})}}},
          {"score", {{"type", "number"}}},
          {"cost", {{"type", "number"}, {"minimum", 0}}},
          {"contacts", {{"type", "integer"}, {"minimum", 0}}},
          {"recent_actions", {{"type", "integer"}, {"minimum", 0}}},
          {"holdout", {{"type", "boolean"}}},
          {"action", {{"type", "string"}}}}}};
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"policy_id"}},
        {"oneOf",
         {nlohmann::json{{"required", {"entity"}}},
          nlohmann::json{{"required", {"database", "table"}}}}},
        {"properties",
         {{"policy_id", {{"type", "string"}, {"minLength", 1}}},
          {"entity", entity_schema},
          {"database", {{"type", "string"}, {"minLength", 1}}},
          {"table", {{"type", "string"}, {"minLength", 1}}},
          {"output_database", {{"type", "string"}, {"minLength", 1}}},
          {"output_table", {{"type", "string"}, {"minLength", 1}}},
          {"entity_column", {{"type", "string"}, {"minLength", 1}}},
          {"score_column", {{"type", "string"}, {"minLength", 1}}},
          {"cost_column", {{"type", "string"}, {"minLength", 1}}},
          {"contacts_column", {{"type", "string"}, {"minLength", 1}}},
          {"recent_actions_column", {{"type", "string"}, {"minLength", 1}}},
          {"holdout_column", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& decide_output_schema() {
    static const nlohmann::json schema = {
        {"oneOf",
         {zetta::solutions::decision_record_schema(),
          nlohmann::json{
              {"type", "object"},
              {"additionalProperties", false},
              {"required", {"table", "rows", "actionable", "pending_approval", "blocked"}},
              {"properties",
               {{"table", {{"type", "string"}}},
                {"rows", {{"type", "integer"}}},
                {"actionable", {{"type", "integer"}}},
                {"pending_approval", {{"type", "integer"}}},
                {"blocked", {{"type", "integer"}}}}}}}}};
    return schema;
}

const nlohmann::json& approve_decision_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"decision_id"}},
        {"properties",
         {{"decision_id", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

ApiResult no_replication() {
    return ApiResult::failure(
        "engine_error",
        "decision policies require a replicated node");
}

ApiResult handle_create_policy(const nlohmann::json& input, ApiContext& context) {
    auto store = store_of(context);
    if (!store) {
        return no_replication();
    }
    std::string error;
    const auto policy = store->create(
        input.at("name").get<std::string>(),
        input.at("config"),
        actor_of(context),
        &error);
    if (!policy) {
        return ApiResult::failure("invalid_argument", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*policy));
}

ApiResult handle_get_policy(const nlohmann::json& input, ApiContext& context) {
    auto store = store_of(context);
    if (!store) {
        return no_replication();
    }
    const std::string id = input.at("policy_id").get<std::string>();
    const auto policy = store->get(id);
    if (!policy) {
        return ApiResult::failure("not_found", "no decision policy with id '" + id + "'");
    }
    return ApiResult::success(zetta::solutions::to_json(*policy));
}

ApiResult handle_update_policy(const nlohmann::json& input, ApiContext& context) {
    auto store = store_of(context);
    if (!store) {
        return no_replication();
    }
    std::optional<nlohmann::json> replacement;
    std::optional<nlohmann::json> tuning;
    if (input.contains("config")) {
        replacement = input.at("config");
    }
    if (input.contains("tuning")) {
        tuning = input.at("tuning");
    }
    std::string error;
    const auto policy = store->update(
        input.at("policy_id").get<std::string>(),
        std::move(replacement),
        std::move(tuning),
        actor_of(context),
        input.value("reason", std::string{}),
        &error);
    if (!policy) {
        return ApiResult::failure("invalid_argument", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*policy));
}

ApiResult handle_decide(const nlohmann::json& input, ApiContext& context) {
    auto store = store_of(context);
    if (!store) {
        return no_replication();
    }
    const std::string policy_id = input.at("policy_id").get<std::string>();
    if (input.contains("entity")) {
        std::string error;
        const auto decision = store->decide(
            policy_id,
            input.at("entity"),
            actor_of(context),
            &error);
        if (!decision) {
            return ApiResult::failure("invalid_argument", error);
        }
        return ApiResult::success(zetta::solutions::to_json(*decision));
    }

    BatchDecisionOptions options;
    options.database = input.at("database").get<std::string>();
    options.table = input.at("table").get<std::string>();
    options.output_database = input.value("output_database", options.output_database);
    options.output_table = input.value("output_table", std::string{});
    options.entity_column = input.value("entity_column", options.entity_column);
    options.score_column = input.value("score_column", options.score_column);
    options.cost_column = input.value("cost_column", options.cost_column);
    options.contacts_column = input.value("contacts_column", options.contacts_column);
    options.recent_actions_column = input.value(
        "recent_actions_column",
        options.recent_actions_column);
    options.holdout_column = input.value(
        "holdout_column",
        options.holdout_column);
    const auto result = zetta::solutions::decide_table(
        context.node,
        *store,
        policy_id,
        options,
        actor_of(context));
    if (!result.ok()) {
        return ApiResult::failure("engine_error", result.error);
    }
    return ApiResult::success({{"table", result.table},
                               {"rows", result.rows},
                               {"actionable", result.actionable},
                               {"pending_approval", result.pending_approval},
                               {"blocked", result.blocked}});
}

ApiResult handle_approve_decision(const nlohmann::json& input, ApiContext& context) {
    auto store = store_of(context);
    if (!store) {
        return no_replication();
    }
    std::string error;
    const auto decision = store->approve(
        input.at("decision_id").get<std::string>(),
        actor_of(context),
        &error);
    if (!decision) {
        return ApiResult::failure("invalid_argument", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*decision));
}

}  // namespace

void register_decision_policy_capabilities(Registry& registry) {
    registry.add(Capability{
        "create_policy",
        "Create a decision policy",
        "Create version 1 of a replicated score-to-action policy and audit the change.",
        create_policy_schema(),
        zetta::solutions::decision_policy_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_policy"},
        true,
        false,
        handle_create_policy});

    registry.add(Capability{
        "get_policy",
        "Get a decision policy",
        "Read a decision policy and its immutable version history by uuid with no side effects.",
        policy_id_schema(),
        zetta::solutions::decision_policy_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        false,
        false,
        handle_get_policy});

    registry.add(Capability{
        "update_policy",
        "Update a decision policy",
        "Create and audit a new policy version, or tune declared numeric parameters within their bounds.",
        update_policy_schema(),
        zetta::solutions::decision_policy_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_policy"},
        true,
        false,
        handle_update_policy});

    registry.add(Capability{
        "decide",
        "Apply a decision policy",
        "Apply the current policy version to one scored entity or a scored table and persist version-stamped decisions.",
        decide_schema(),
        decide_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "decide"},
        true,
        false,
        handle_decide});

    registry.add(Capability{
        "approve_decision",
        "Approve an A1 decision",
        "Approve one pending A1 decision, make it actionable, and append an audit event.",
        approve_decision_schema(),
        zetta::solutions::decision_record_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "approve_decision"},
        true,
        false,
        handle_approve_decision});
}

}  // namespace zetta::api
