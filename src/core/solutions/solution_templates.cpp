#include "zetta/solutions/solution_templates.hpp"

#include <array>
#include <utility>

namespace zetta::solutions {
namespace {

using Json = nlohmann::json;

Json frequency(int maximum, int window_days = 30) {
    return {{"max_actions", maximum}, {"window_seconds", window_days * 86400}};
}

Json predicate(const std::string& field, bool value) {
    return {{"name", field}, {"role", "entity"}, {"field", field},
            {"operator", "eq"}, {"value", value}};
}

struct DefaultAction {
    const char* id;
    const char* action;
    const char* autonomy;
    const char* design;
    const char* outcome;
    const char* unit;
    const char* guardrail;
};

constexpr std::array kDefaults = {
    DefaultAction{"S-1", "csm_task", "A2", "holdout", "retained", "customer", "complaints"},
    DefaultAction{"S-2", "sales_queue_task", "A2", "holdout", "converted", "opportunity", "sla_misses"},
    DefaultAction{"S-3", "audience_sync", "A2", "holdout", "net_value", "currency", "consent_violations"},
    DefaultAction{"S-4", "collections_queue", "A2", "champion_challenger", "net_recovery", "currency", "complaints"},
    DefaultAction{"S-5", "planner_report", "A2", "fva", "forecast_error", "absolute_error", "stale_forecasts"},
    DefaultAction{"S-6", "draft_order", "A0", "phased", "service_level", "fraction", "spend_overruns"},
    DefaultAction{"S-7", "decision_forum_report", "A2", "phased", "decision_adopted", "decision", "false_escalations"},
    DefaultAction{"S-8", "draft_work_order", "A0", "phased", "asset_failure", "failure", "unsafe_actions"},
    DefaultAction{"S-9", "outbound_journey", "A2", "holdout", "response", "response", "complaints"},
    DefaultAction{"S-10", "fraud_review_task", "A1", "champion_challenger", "fraud_loss", "currency", "false_declines"},
    DefaultAction{"S-11", "eligible_action", "A1", "uplift_cells", "net_value", "currency", "consent_violations"},
    DefaultAction{"S-12", "referral_task", "A0", "phased", "referral_confirmed", "claim", "automated_denials"},
};

SolutionTemplates defaults_for(const DefaultAction& action) {
    SolutionTemplates result;
    result.policy = {
        {"autonomy", "A0"}, {"kill_switch", false},
        {"tiers", {{{"name", "priority"}, {"minimum_score", 0.7}, {"action", action.action}},
                   {{"name", "standard"}, {"minimum_score", 0.0}, {"action", "no_contact"}}}},
        {"actions", {{action.action, {{"autonomy", action.autonomy}, {"sla_seconds", 86400},
                                     {"frequency_cap", frequency(1)}}},
                     {"no_contact", {{"autonomy", "A0"}}}}},
        {"contact_cap", 12}, {"frequency_cap", frequency(1)},
        {"eligibility_rules", {predicate("eligible", true)}},
        {"exclusion_rules", {predicate("excluded", true)}},
        {"budget", {{"limit", 0.0}}}};
    result.experiment.holdouts = {{"priority", 0.1}, {"standard", 0.1}};
    result.guardrails = {{action.guardrail, 0.0, "maximum"}};
    result.measurement = {action.design, action.outcome, action.unit, 30};
    return result;
}

void retention_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "high"}, {"minimum_score", 0.7}, {"action", "csm_task"},
         {"allowed_actions", {"csm_task", "discount", "no_contact"}}},
        {{"name", "medium"}, {"minimum_score", 0.4}, {"action", "retention_journey"},
         {"allowed_actions", {"retention_journey", "no_contact"}}},
        {{"name", "low"}, {"minimum_score", 0.0}, {"action", "no_contact"}}};
    result.policy["actions"] = {
        {"csm_task", {{"autonomy", "A2"}, {"sla_seconds", 86400},
                      {"frequency_cap", frequency(1)}}},
        {"retention_journey", {{"autonomy", "A2"}, {"sla_seconds", 86400},
                               {"frequency_cap", frequency(1)}}},
        {"discount", {{"autonomy", "A2"}, {"approval_above", 50.0},
                      {"sla_seconds", 86400}, {"frequency_cap", frequency(1)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["budget"] = {{"limit", 1000.0}};
    result.policy["eligibility_rules"] = {
        predicate("in_collections", false), predicate("open_complaint", false),
        predicate("vulnerable", false)};
    result.policy["exclusion_rules"] = {
        predicate("in_collections", true), predicate("open_complaint", true),
        predicate("vulnerable", true)};
    result.experiment.holdouts = {{"high", 0.1}, {"medium", 0.1}, {"low", 0.1}};
    result.experiment.treatment_cells = {"csm_task", "retention_journey"};
    result.guardrails = {{"complaints", 0.01, "maximum"},
                         {"discount_budget", 1000.0, "maximum"},
                         {"contact_fatigue", 1.0, "maximum"},
                         {"action_execution_rate", 0.95, "minimum"}};
}

void lead_scoring_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "hot"}, {"minimum_score", 0.7}, {"action", "sales_queue_task"},
         {"allowed_actions", {"sales_queue_task", "priority_queue_review", "no_contact"}}},
        {{"name", "standard"}, {"minimum_score", 0.0}, {"action", "nurture_queue_task"},
         {"allowed_actions", {"nurture_queue_task", "no_contact"}}}};
    result.policy["actions"] = {
        {"sales_queue_task", {{"autonomy", "A2"}, {"sla_seconds", 3600},
                              {"frequency_cap", frequency(1, 7)}}},
        {"nurture_queue_task", {{"autonomy", "A2"}, {"sla_seconds", 14400},
                                {"frequency_cap", frequency(1, 7)}}},
        {"priority_queue_review", {{"autonomy", "A1"}, {"sla_seconds", 3600},
                                   {"frequency_cap", frequency(1, 7)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 6;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("eligible", true), predicate("territory_eligible", true),
        predicate("rep_capacity_available", true)};
    result.policy["exclusion_rules"] = {
        predicate("excluded", true), predicate("do_not_contact", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {{"hot", 0.1}, {"standard", 0.1}};
    result.experiment.treatment_cells = {"routed"};
    result.guardrails = {{"sla_miss_rate", 0.0, "maximum"},
                         {"capacity_violation_rate", 0.0, "maximum"},
                         {"routing_coverage_rate", 0.9, "minimum"},
                         {"action_execution_rate", 0.95, "minimum"}};
}

} // namespace

SolutionTemplates solution_templates(const std::string& solution_id) {
    for (const DefaultAction& action : kDefaults) {
        if (solution_id == action.id) {
            SolutionTemplates result = defaults_for(action);
            if (solution_id == "S-1") {
                retention_defaults(result);
            } else if (solution_id == "S-2") {
                lead_scoring_defaults(result);
            }
            return result;
        }
    }
    return {};
}

Json to_json(const SolutionExperimentTemplate& value) {
    Json holdouts = Json::array();
    for (const TierHoldout& holdout : value.holdouts) {
        holdouts.push_back({{"tier", holdout.tier}, {"fraction", holdout.fraction}});
    }
    return {{"holdout_per_tier", std::move(holdouts)},
            {"treatment_cells", value.treatment_cells}, {"seed_rule", value.seed_rule}};
}

Json to_json(const SolutionGuardrail& value) {
    return {{"metric", value.metric}, {"bound", value.bound}, {"direction", value.direction}};
}

Json to_json(const SolutionMeasurement& value) {
    return {{"design", value.design},
            {"outcome", {{"name", value.outcome}, {"unit", value.unit},
                         {"window_days", value.window_days}}}};
}

const Json& solution_experiment_template_schema() {
    static const Json schema = Json::parse(R"JSON(
{
  "type": "object",
  "additionalProperties": false,
  "required": [
    "holdout_per_tier",
    "treatment_cells",
    "seed_rule"
  ],
  "properties": {
    "holdout_per_tier": {
      "type": "array",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": [
          "tier",
          "fraction"
        ],
        "properties": {
          "tier": {
            "type": "string"
          },
          "fraction": {
            "type": "number",
            "minimum": 0,
            "maximum": 1
          }
        }
      }
    },
    "treatment_cells": {
      "type": "array",
      "items": {
        "type": "string"
      }
    },
    "seed_rule": {
      "type": "string",
      "enum": [
        "project_solution_v1"
      ]
    }
  }
}
)JSON");
    return schema;
}

const Json& solution_guardrail_schema() {
    static const Json schema = Json::parse(R"JSON(
{
  "type": "object",
  "additionalProperties": false,
  "required": [
    "metric",
    "bound",
    "direction"
  ],
  "properties": {
    "metric": {
      "type": "string",
      "minLength": 1
    },
    "bound": {
      "type": "number"
    },
    "direction": {
      "type": "string",
      "enum": [
        "minimum",
        "maximum"
      ]
    }
  }
}
)JSON");
    return schema;
}

const Json& solution_measurement_schema() {
    static const Json schema = Json::parse(R"JSON(
{
  "type": "object",
  "additionalProperties": false,
  "required": [
    "design",
    "outcome"
  ],
  "properties": {
    "design": {
      "type": "string",
      "enum": [
        "holdout",
        "uplift_cells",
        "champion_challenger",
        "fva",
        "phased"
      ]
    },
    "outcome": {
      "type": "object",
      "additionalProperties": false,
      "required": [
        "name",
        "unit",
        "window_days"
      ],
      "properties": {
        "name": {
          "type": "string"
        },
        "unit": {
          "type": "string"
        },
        "window_days": {
          "type": "integer",
          "minimum": 1
        }
      }
    }
  }
}
)JSON");
    return schema;
}

} // namespace zetta::solutions
