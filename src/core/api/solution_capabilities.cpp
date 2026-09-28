#include "zetta/api/capabilities.hpp"

#include <string>
#include <utility>

#include "zetta/ml/solution.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/solutions/solution_execution.hpp"
#include "zetta/solutions/solution_templates.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

const nlohmann::json& empty_schema() {
    static const nlohmann::json schema = {
        {"type", "object"}, {"additionalProperties", false}};
    return schema;
}

const nlohmann::json& string_array_schema() {
    static const nlohmann::json schema = {
        {"type", "array"}, {"items", {{"type", "string"}}}};
    return schema;
}

const nlohmann::json& composition_entry_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"methodology", "role", "minimum_level"}},
        {"additionalProperties", false},
        {"properties",
         {{"methodology", {{"type", "string"}}},
          {"role", {{"type", "string"}}},
          {"minimum_level", {{"type", "integer"}}}}}};
    return schema;
}

const nlohmann::json& autonomy_default_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"action_class", "autonomy"}},
        {"additionalProperties", false},
        {"properties",
         {{"action_class", {{"type", "string"}}},
          {"autonomy", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& data_role_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"role", "required", "description"}},
        {"additionalProperties", false},
        {"properties",
         {{"role", {{"type", "string"}}},
          {"required", {{"type", "boolean"}}},
          {"description", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& capability_level_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"level", "name", "description"}},
        {"additionalProperties", false},
        {"properties",
         {{"level", {{"type", "integer"}}},
          {"name", {{"type", "string"}}},
          {"description", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& missing_component_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id", "name", "issue"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"issue", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& solution_item_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id", "name", "promise", "use_case", "composition",
                       "pipeline_order",
                       "action_patterns", "latency_classes", "autonomy_defaults",
                       "required_data_roles", "decision_policy_template",
                       "measurement_design", "policy_template", "experiment_template",
                       "measurement", "minimal_template", "guardrails", "levels",
                       "missing_components", "status", "runnable", "issue",
                       "promotion_condition"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"promise", {{"type", "string"}}},
          {"use_case", {{"type", "string"}}},
          {"composition",
           {{"type", "array"}, {"items", composition_entry_schema()}}},
          {"pipeline_order", string_array_schema()},
          {"action_patterns", string_array_schema()},
          {"latency_classes", string_array_schema()},
          {"autonomy_defaults",
           {{"type", "array"}, {"items", autonomy_default_schema()}}},
          {"required_data_roles",
           {{"type", "array"}, {"items", data_role_schema()}}},
          {"decision_policy_template", {{"type", "string"}}},
          {"measurement_design", {{"type", "string"}}},
          {"policy_template", zetta::solutions::decision_policy_config_schema()},
          {"experiment_template", zetta::solutions::solution_experiment_template_schema()},
          {"measurement", zetta::solutions::solution_measurement_schema()},
          {"minimal_template", {{"type", "boolean"}}},
          {"guardrails", {{"type", "array"}, {"items", zetta::solutions::solution_guardrail_schema()}}},
          {"levels",
           {{"type", "array"}, {"items", capability_level_schema()}}},
          {"missing_components",
           {{"type", "array"}, {"items", missing_component_schema()}}},
          {"status", {{"type", "string"}}},
          {"runnable", {{"type", "boolean"}}},
          {"issue", {{"type", "string"}}},
          {"promotion_condition", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& list_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"solutions"}},
        {"additionalProperties", false},
        {"properties",
         {{"solutions", {{"type", "array"}, {"items", solution_item_schema()}}}}}};
    return schema;
}

const nlohmann::json& get_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"solution"}},
        {"additionalProperties", false},
        {"properties", {{"solution", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& get_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"solution"}},
        {"additionalProperties", false},
        {"properties", {{"solution", solution_item_schema()}}}};
    return schema;
}

const nlohmann::json& create_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "solution", "level", "sources", "foundation"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}, {"minLength", 1}}},
          {"solution", {{"type", "string"}, {"minLength", 1}}},
          {"level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
          {"sources",
           {{"type", "object"},
            {"description", "common callback hints plus optional M-id overrides"}}},
          {"foundation", zetta::solutions::solution_foundation_schema()},
          {"name", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& create_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "solution", "level", "graph", "preview",
                       "methodologies", "missing_components", "policy_id",
                       "experiment_id", "monitor_id", "readout_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}}},
          {"solution", {{"type", "string"}}},
          {"level", {{"type", "integer"}}},
          {"graph", {{"type", "string"}}},
          {"preview", {{"type", "boolean"}}},
          {"policy_id", {{"type", "string"}}},
          {"experiment_id", {{"type", "string"}}},
          {"monitor_id", {{"type", "string"}}},
          {"readout_id", {{"type", "string"}}},
          {"methodologies", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"missing_components",
           {{"type", "array"}, {"items", missing_component_schema()}}}}}};
    return schema;
}

const nlohmann::json& run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}, {"minLength", 1}}},
          {"graph", {{"type", "string"}, {"minLength", 1}}},
          {"delivery_token", {{"type", "string"}, {"writeOnly", true}}}}}};
    return schema;
}

const nlohmann::json& run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "engine", "status", "preview"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id", {{"type", "string"}}},
          {"engine", {{"type", "string"}}},
          {"status", {{"type", "string"}}},
          {"preview", {{"type", "boolean"}}}}}};
    return schema;
}

ApiResult handle_list(const nlohmann::json&, ApiContext&) {
    nlohmann::json entries = nlohmann::json::array();
    for (const zetta::ml::Solution& solution : zetta::ml::solutions()) {
        entries.push_back(zetta::ml::solution_json(solution));
    }
    return ApiResult::success({{"solutions", std::move(entries)}});
}

ApiResult handle_get(const nlohmann::json& input, ApiContext&) {
    const std::string id = input.at("solution").get<std::string>();
    const zetta::ml::Solution* solution = zetta::ml::find_solution(id);
    if (solution == nullptr) {
        return ApiResult::failure("not_found", "no solution named '" + id + "'");
    }
    return ApiResult::success({{"solution", zetta::ml::solution_json(*solution)}});
}

ApiResult handle_create(const nlohmann::json& input, ApiContext& context) {
    return zetta::solutions::create_composed_solution(input, context);
}

ApiResult handle_run(const nlohmann::json& input, ApiContext& context) {
    return zetta::solutions::run_composed_solution(input, context);
}

} // namespace

void register_solution_capabilities(Registry& registry) {
    registry.add(Capability{
        "list_solutions", "List solutions",
        "Read all twelve catalogue solutions and eight Band-B sketches, including composition, data roles, governance, capability levels, and runnable status. No side effects.",
        empty_schema(), list_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false, handle_list});
    registry.add(Capability{
        "get_solution", "Get solution",
        "Read one catalogue solution or sketch by id, including every production dependency and tracking issue. No side effects.",
        get_schema(), get_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false, handle_get});
    registry.add(Capability{
        "create_solution", "Create solution",
        "Create a composed Pipeline, versioned policy, protected experiment, guardrail monitor and ROI definition from executable defaults and explicit data, delivery and economics settings.",
        create_schema(), create_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_solution"}, true,
        false, handle_create});
    registry.add(Capability{
        "run_solution", "Run solution",
        "Materialize the explicit snapshot, run registered methodologies, apply policy and holdout protection, deliver governed actions and record the ledger in a durable Run.",
        run_schema(), run_output_schema(), CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "run_solution"}, true,
        false, handle_run});
}

} // namespace zetta::api
