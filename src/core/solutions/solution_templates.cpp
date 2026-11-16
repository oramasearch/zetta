// SPDX-License-Identifier: MIT

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

Json numeric_predicate(const std::string& field, const std::string& operation,
                       double value) {
    return {{"name", field}, {"role", "entity"}, {"field", field},
            {"operator", operation}, {"value", value}};
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
    DefaultAction{"B-1", "price_recommendation", "A0", "holdout", "net_margin", "currency", "margin_floor_violations"},
    DefaultAction{"B-2", "people_leader_review", "A2", "holdout", "attrition_reduction", "fraction", "small_group_outputs"},
    DefaultAction{"B-3", "route_ticket", "A2", "holdout", "resolution_quality", "resolution", "misroute_rate"},
    DefaultAction{"B-4", "field_review_task", "A2", "holdout", "workflow_completed", "document", "unvalidated_workflow_actions"},
    DefaultAction{"B-5", "supplier_watch_task", "A2", "holdout", "disruption_avoided", "supplier", "missing_flag_evidence_rate"},
    DefaultAction{"B-6", "care_coordination_task", "A2", "holdout", "operational_resolution", "case", "care_denial_actions"},
    DefaultAction{"B-7", "underwriter_review_task", "A2", "holdout", "performing_account", "application", "automated_adverse_decisions"},
    DefaultAction{"B-8", "investigation_recommendation", "A0", "holdout", "incident_avoided", "incident", "alert_volume"},
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

void lifetime_value_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "strategic"}, {"minimum_score", 1000.0},
         {"action", "budget_reallocation"},
         {"allowed_actions", {"budget_reallocation", "audience_sync",
                              "no_contact"}}},
        {{"name", "growth"}, {"minimum_score", 500.0},
         {"action", "audience_sync"},
         {"allowed_actions", {"audience_sync", "no_contact"}}},
        {{"name", "standard"}, {"minimum_score", 0.0},
         {"action", "no_contact"}}};
    result.policy["actions"] = {
        {"audience_sync", {{"autonomy", "A2"}, {"sla_seconds", 86400},
                           {"frequency_cap", frequency(1)}}},
        {"budget_reallocation", {{"autonomy", "A1"},
                                  {"approval_above", 0.0},
                                  {"sla_seconds", 86400},
                                  {"frequency_cap", frequency(1)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["budget"] = {{"limit", 10000.0}};
    result.policy["eligibility_rules"] = {
        predicate("consent", true), predicate("channel_eligible", true),
        {{"name", "margin_floor"}, {"role", "entity"},
         {"field", "margin"}, {"operator", "gte"}, {"value", 0.2}}};
    result.policy["exclusion_rules"] = {
        predicate("do_not_target", true),
        predicate("account_closed", true)};
    result.experiment.holdouts = {
        {"strategic", 0.1}, {"growth", 0.1}, {"standard", 0.1}};
    result.experiment.treatment_cells = {
        "audience_sync", "budget_reallocation"};
    result.guardrails = {
        {"consent_violations", 0.0, "maximum"},
        {"margin_violations", 0.0, "maximum"},
        {"discount_fairness_gap", 0.1, "maximum"},
        {"budget_utilization", 1.0, "maximum"},
        {"cohort_calibration_error", 0.1, "maximum"}};
    result.measurement = {"holdout", "net_value", "currency", 365};
}

void collections_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "priority"}, {"minimum_score", 0.7}, {"action", "dialer_queue"},
         {"allowed_actions", {"dialer_queue", "digital_treatment", "settlement", "no_contact"}}},
        {{"name", "standard"}, {"minimum_score", 0.4}, {"action", "digital_treatment"},
         {"allowed_actions", {"digital_treatment", "no_contact"}}},
        {{"name", "defer"}, {"minimum_score", 0.0}, {"action", "no_contact"}}};
    result.policy["actions"] = {
        {"dialer_queue", {{"autonomy", "A2"}, {"sla_seconds", 86400},
                          {"frequency_cap", frequency(1)}}},
        {"digital_treatment", {{"autonomy", "A2"}, {"sla_seconds", 86400},
                               {"frequency_cap", frequency(1)}}},
        {"settlement", {{"autonomy", "A1"}, {"approval_above", 0.0},
                        {"sla_seconds", 86400}, {"frequency_cap", frequency(1)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 6;
    result.policy["frequency_cap"] = {
        {"max_actions", 1}, {"window_seconds", 7 * 86400}};
    result.policy["eligibility_rules"] = {
        predicate("collections_eligible", true),
        predicate("regulatory_contact_allowed", true),
        predicate("within_contact_hours", true),
        predicate("vulnerable", false)};
    result.policy["exclusion_rules"] = {
        predicate("collections_eligible", false),
        predicate("regulatory_contact_allowed", false),
        predicate("within_contact_hours", false),
        predicate("vulnerable", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"priority", 0.1}, {"standard", 0.1}, {"defer", 0.1}};
    result.experiment.treatment_cells = {
        "dialer_queue", "digital_treatment"};
    result.guardrails = {
        {"complaints", 0.0, "maximum"},
        {"regulatory_contact_violations", 0.0, "maximum"},
        {"vulnerable_customer_contacts", 0.0, "maximum"},
        {"contact_frequency", 1.0, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
}

void claims_triage_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "siu_review"}, {"minimum_score", 0.75},
         {"action", "siu_referral"},
         {"allowed_actions", {"siu_referral", "denial_recommendation"}}},
        {{"name", "adjuster_review"}, {"minimum_score", 0.40},
         {"action", "adjuster_assignment"},
         {"allowed_actions", {"adjuster_assignment", "denial_recommendation"}}},
        {{"name", "straight_through"}, {"minimum_score", 0.0},
         {"action", "fast_track"},
         {"allowed_actions", {"fast_track", "denial_recommendation"}}}};
    result.policy["actions"] = {
        {"fast_track", {{"autonomy", "A2"}, {"approval_above", 5000.0},
                         {"sla_seconds", 3600},
                         {"frequency_cap", frequency(1)}}},
        {"adjuster_assignment", {{"autonomy", "A2"},
                                 {"sla_seconds", 14400},
                                 {"frequency_cap", frequency(1)}}},
        {"siu_referral", {{"autonomy", "A2"},
                          {"sla_seconds", 86400},
                          {"frequency_cap", frequency(1)}}},
        {"denial_recommendation", {{"autonomy", "A0"},
                                   {"sla_seconds", 86400}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 1;
    result.policy["frequency_cap"] = frequency(1);
    result.policy["budget"] = {{"limit", 100000.0}};
    result.policy["eligibility_rules"] = {
        predicate("fnol_complete", true),
        predicate("coverage_confirmed", true),
        predicate("identity_verified", true),
        predicate("document_confidence_clear", true),
        predicate("fairness_review_passed", true)};
    result.policy["exclusion_rules"] = {
        predicate("catastrophe_override", true),
        predicate("manual_only", true)};
    result.experiment.holdouts = {
        {"siu_review", 0.1},
        {"adjuster_review", 0.1},
        {"straight_through", 0.1}};
    result.experiment.treatment_cells = {
        "path_assignment", "referral_task"};
    result.guardrails = {
        {"automated_denials", 0.0, "maximum"},
        {"low_confidence_path_assignments", 0.0, "maximum"},
        {"fairness_review_failures", 0.0, "maximum"},
        {"field_extraction_accuracy", 0.8, "minimum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {"holdout", "referral_confirmed", "claim", 45};
}

void credit_underwriting_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "elevated_risk"}, {"minimum_score", 0.7},
         {"action", "underwriter_review_task"},
         {"allowed_actions", {"underwriter_review_task",
                              "decline_recommendation",
                              "lower_limit_recommendation", "no_action"}}},
        {{"name", "review"}, {"minimum_score", 0.4},
         {"action", "limit_review_task"},
         {"allowed_actions", {"limit_review_task", "increase_limit",
                              "decline_recommendation",
                              "lower_limit_recommendation", "no_action"}}},
        {{"name", "eligible"}, {"minimum_score", 0.0},
         {"action", "approve_credit"},
         {"allowed_actions", {"approve_credit", "increase_limit",
                              "decline_recommendation",
                              "lower_limit_recommendation", "no_action"}}}};
    result.policy["actions"] = {
        {"approve_credit", {{"autonomy", "A2"},
                            {"approval_field", "requested_limit"},
                            {"approval_above", 5000.0},
                            {"sla_seconds", 86400},
                            {"frequency_cap", frequency(1)}}},
        {"underwriter_review_task", {{"autonomy", "A2"},
                                      {"sla_seconds", 86400},
                                      {"frequency_cap", frequency(1)}}},
        {"limit_review_task", {{"autonomy", "A2"},
                                {"sla_seconds", 86400},
                                {"frequency_cap", frequency(1)}}},
        {"increase_limit", {{"autonomy", "A1"},
                             {"sla_seconds", 86400},
                             {"frequency_cap", frequency(1)}}},
        {"decline_recommendation", {{"autonomy", "A0"},
                                     {"sla_seconds", 86400}}},
        {"lower_limit_recommendation", {{"autonomy", "A0"},
                                         {"sla_seconds", 86400}}},
        {"no_action", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 1;
    result.policy["frequency_cap"] = frequency(1);
    result.policy["budget"] = {{"limit", 0.0}};
    result.policy["eligibility_rules"] = {
        predicate("eligibility_verified", true),
        predicate("income_verified", true),
        predicate("model_validation_approved", true),
        predicate("fairness_review_passed", true),
        predicate("adverse_action_reasons_ready", true)};
    result.policy["exclusion_rules"] = {
        predicate("fraud_hold", true), predicate("bankruptcy_hold", true),
        predicate("manual_only", true)};
    result.experiment.holdouts = {
        {"elevated_risk", 0.1}, {"review", 0.1}, {"eligible", 0.1}};
    result.experiment.treatment_cells = {"policy_assisted_decision"};
    result.guardrails = {
        {"automated_adverse_decisions", 0.0, "maximum"},
        {"missing_adverse_action_reasons", 0.0, "maximum"},
        {"model_validation_approval_failures", 0.0, "maximum"},
        {"fairness_review_failures", 0.0, "maximum"},
        {"approval_rate_gap", 0.1, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {"holdout", "performing_account", "application", 90};
}

void demand_forecast_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "critical"}, {"minimum_score", 0.2},
         {"action", "exception_queue"}},
        {{"name", "review"}, {"minimum_score", 0.05},
         {"action", "exception_queue"}},
        {{"name", "routine"}, {"minimum_score", 0.0},
         {"action", "no_contact"}}};
    result.policy["actions"] = {
        {"exception_queue",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1)}}},
        {"planner_report", {{"autonomy", "A2"}, {"sla_seconds", 86400}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 52;
    result.policy["frequency_cap"] = {
        {"max_actions", 1}, {"window_seconds", 7 * 86400}};
    result.policy["eligibility_rules"] = {
        predicate("planner_owned", true),
        predicate("forecast_fresh", true),
        predicate("hierarchy_consistent", true)};
    result.policy["exclusion_rules"] = {
        predicate("stockout_censored", true),
        predicate("override_locked", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"critical", 0.1}, {"review", 0.1}, {"routine", 0.1}};
    result.experiment.treatment_cells = {"planner_report"};
    result.guardrails = {
        {"forecast_value_added", 0.0, "minimum"},
        {"interval_coverage", 0.75, "minimum"},
        {"stale_forecast_fraction", 0.0, "maximum"},
        {"stockout_censored_fraction", 0.1, "maximum"},
        {"unaudited_override_fraction", 0.0, "maximum"}};
}

void replenishment_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "replenish"}, {"minimum_score", 1.0},
         {"action", "draft_order"},
         {"allowed_actions", {"draft_order", "erp_replenishment_task",
                              "release_order", "no_contact"}}},
        {{"name", "no_order"}, {"minimum_score", 0.0},
         {"action", "no_contact"}}};
    result.policy["actions"] = {
        {"draft_order", {{"autonomy", "A0"}, {"sla_seconds", 86400},
                         {"frequency_cap", frequency(1)}}},
        {"erp_replenishment_task",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1)}}},
        {"release_order",
         {{"autonomy", "A2"}, {"approval_field", "order_quantity"},
          {"approval_above", 100.0}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 52;
    result.policy["frequency_cap"] = frequency(1);
    result.policy["budget"] = {{"limit", 10000.0}};
    result.policy["eligibility_rules"] = {
        predicate("eligible", true), predicate("lead_time_confirmed", true),
        numeric_predicate("order_quantity", "lte", 500.0)};
    result.policy["exclusion_rules"] = {
        predicate("inventory_hold", true), predicate("supplier_blocked", true)};
    result.experiment.holdouts = {{"replenish", 0.1}, {"no_order", 0.1}};
    result.experiment.treatment_cells = {"replenishment_action"};
    result.guardrails = {
        {"replenishment_spend", 10000.0, "maximum"},
        {"service_level", 0.95, "minimum"},
        {"inventory_exposure", 5000.0, "maximum"},
        {"order_volatility", 0.5, "maximum"},
        {"unapproved_over_bound_orders", 0.0, "maximum"}};
    result.measurement = {"holdout", "service_level", "fraction", 30};
}

void dynamic_pricing_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "high_demand"}, {"minimum_score", 24.0},
         {"action", "price_recommendation"},
         {"allowed_actions", {"price_recommendation", "pricing_review_task",
                              "controlled_price_experiment",
                              "publish_price_change", "no_contact"}}},
        {{"name", "standard_demand"}, {"minimum_score", 0.0},
         {"action", "price_recommendation"},
         {"allowed_actions", {"price_recommendation", "pricing_review_task",
                              "controlled_price_experiment",
                              "publish_price_change", "no_contact"}}}};
    result.policy["actions"] = {
        {"price_recommendation",
         {{"autonomy", "A0"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"pricing_review_task",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"controlled_price_experiment",
         {{"autonomy", "A2"}, {"approval_field", "authority_distance"},
          {"approval_above", 0.05}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"publish_price_change",
         {{"autonomy", "A2"}, {"approval_field", "authority_distance"},
          {"approval_above", 0.05}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 52;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("pricing_eligible", true),
        predicate("current_price_fresh", true),
        numeric_predicate("gross_margin", "gte", 0.25),
        numeric_predicate("authority_distance", "lte", 0.15)};
    result.policy["exclusion_rules"] = {
        predicate("manual_pricing_only", true),
        predicate("legal_hold", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"high_demand", 0.1}, {"standard_demand", 0.1}};
    result.experiment.treatment_cells = {"price_change"};
    result.guardrails = {
        {"margin_floor_violations", 0.0, "maximum"},
        {"authority_band_violations", 0.0, "maximum"},
        {"unapproved_price_changes", 0.0, "maximum"},
        {"price_feature_leakage", 0.0, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {"holdout", "net_margin", "currency", 30};
}

void kpi_anomaly_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "critical"}, {"minimum_score", 0.8},
         {"action", "decision_forum_report"},
         {"allowed_actions", {"decision_forum_report", "executive_review",
                              "no_contact"}}},
        {{"name", "anomaly"}, {"minimum_score", 0.0},
         {"action", "decision_forum_report"},
         {"allowed_actions", {"decision_forum_report", "no_contact"}}}};
    result.policy["actions"] = {
        {"decision_forum_report",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"executive_review",
         {{"autonomy", "A1"}, {"sla_seconds", 14400},
          {"frequency_cap", frequency(1, 7)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 12;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("eligible", true),
        predicate("forum_owner_assigned", true),
        predicate("sensitive_driver_access", true)};
    result.policy["exclusion_rules"] = {
        predicate("suppressed", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"critical", 0.1}, {"anomaly", 0.1}};
    result.experiment.treatment_cells = {"forum_routed"};
    result.guardrails = {
        {"alert_volume", 12.0, "maximum"},
        {"false_escalation_rate", 0.25, "maximum"},
        {"sensitive_driver_access_violations", 0.0, "maximum"},
        {"causal_claim_violations", 0.0, "maximum"},
        {"decision_closure_rate", 0.9, "minimum"},
        {"action_execution_rate", 0.95, "minimum"}};
}

void predictive_maintenance_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "imminent"}, {"minimum_score", 0.65},
         {"action", "draft_work_order"},
         {"allowed_actions", {"draft_work_order", "scheduled_work_order",
                              "no_action"}}},
        {{"name", "elevated"}, {"minimum_score", 0.25},
         {"action", "inspection_work_order"},
         {"allowed_actions", {"inspection_work_order",
                              "scheduled_work_order", "no_action"}}},
        {{"name", "monitor"}, {"minimum_score", 0.0},
         {"action", "no_action"}}};
    result.policy["actions"] = {
        {"draft_work_order",
         {{"autonomy", "A0"}, {"sla_seconds", 14400},
          {"frequency_cap", frequency(1, 7)}}},
        {"inspection_work_order",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"scheduled_work_order",
         {{"autonomy", "A1"}, {"sla_seconds", 14400},
          {"frequency_cap", frequency(1, 7)}}},
        {"no_action", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 24;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("maintenance_eligible", true),
        predicate("approved_asset_class", true),
        predicate("maintenance_window_available", true),
        predicate("safe_to_service", true)};
    result.policy["exclusion_rules"] = {
        predicate("safety_lockout", true),
        predicate("active_work_order", true),
        predicate("sensor_data_untrusted", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"imminent", 0.1}, {"elevated", 0.1}, {"monitor", 0.1}};
    result.experiment.treatment_cells = {"work_order"};
    result.guardrails = {
        {"unsafe_work_order_rate", 0.0, "maximum"},
        {"false_work_order_rate", 0.1, "maximum"},
        {"missed_failure_rate", 0.05, "maximum"},
        {"sensor_drift_psi", 0.2, "maximum"},
        {"maintenance_capacity_utilization", 0.9, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {"holdout", "failure_avoided", "asset", 30};
}

void automated_outbound_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "priority"}, {"minimum_score", 0.7}, {"action", "outbound_journey"},
         {"allowed_actions", {"outbound_journey", "copy_review", "no_contact"}}},
        {{"name", "standard"}, {"minimum_score", 0.4}, {"action", "nurture_journey"},
         {"allowed_actions", {"nurture_journey", "copy_review", "no_contact"}}},
        {{"name", "low"}, {"minimum_score", 0.0}, {"action", "no_contact"}}};
    result.policy["actions"] = {
        {"outbound_journey", {{"autonomy", "A2"}, {"sla_seconds", 3600},
                              {"frequency_cap", frequency(1, 7)}}},
        {"nurture_journey", {{"autonomy", "A2"}, {"sla_seconds", 86400},
                             {"frequency_cap", frequency(1, 14)}}},
        {"copy_review", {{"autonomy", "A1"}, {"sla_seconds", 86400},
                         {"frequency_cap", frequency(1, 7)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 4;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("consent", true), predicate("suppressed", false),
        predicate("channel_eligible", true)};
    result.policy["exclusion_rules"] = {
        predicate("suppressed", true), predicate("unsubscribed", true),
        predicate("do_not_contact", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"priority", 0.1}, {"standard", 0.1}, {"low", 0.1}};
    result.experiment.treatment_cells = {"journey_sent"};
    result.guardrails = {{"unsubscribe_rate", 0.01, "maximum"},
                         {"complaint_rate", 0.005, "maximum"},
                         {"consent_violation_rate", 0.0, "maximum"},
                         {"contact_frequency", 1.0, "maximum"},
                         {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {"holdout", "response", "response", 30};
}

void fraud_decisioning_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "critical"}, {"minimum_score", 0.9},
         {"action", "fraud_review_task"},
         {"allowed_actions", {"fraud_review_task", "challenge", "block"}}},
        {{"name", "elevated"}, {"minimum_score", 0.65},
         {"action", "challenge"},
         {"allowed_actions", {"challenge", "fraud_review_task"}}},
        {{"name", "standard"}, {"minimum_score", 0.0},
         {"action", "approve"}}};
    result.policy["actions"] = {
        {"fraud_review_task",
         {{"autonomy", "A2"}, {"sla_seconds", 900},
          {"frequency_cap", frequency(1, 1)}}},
        {"challenge",
         {{"autonomy", "A2"}, {"sla_seconds", 2},
          {"frequency_cap", frequency(1, 1)}}},
        {"approve",
         {{"autonomy", "A2"}, {"sla_seconds", 2},
          {"frequency_cap", frequency(1, 1)}}},
        {"block",
         {{"autonomy", "A1"}, {"sla_seconds", 900},
          {"frequency_cap", frequency(1, 1)}}}};
    result.policy["contact_cap"] = 1;
    result.policy["frequency_cap"] = frequency(1, 1);
    result.policy["eligibility_rules"] = {
        predicate("eligible", true),
        predicate("account_open", true),
        predicate("request_time_features_current", true),
        predicate("graph_features_current", true)};
    result.policy["exclusion_rules"] = {
        predicate("excluded", true),
        predicate("manual_authorization_only", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"critical", 0.1}, {"elevated", 0.1}, {"standard", 0.1}};
    result.experiment.treatment_cells = {
        "champion", "challenger", "exploration"};
    result.guardrails = {
        {"fraud_loss_rate", 0.02, "maximum"},
        {"false_decline_rate", 0.01, "maximum"},
        {"challenge_rate", 0.15, "maximum"},
        {"review_queue_rate", 0.2, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement.window_days = 45;
}

void next_best_action_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "recommended"}, {"minimum_score", 0.0},
         {"action", "decision_api_response"},
         {"allowed_actions", {"decision_api_response", "high_stakes_review",
                              "no_action"}}},
        {{"name", "no_positive_value"}, {"minimum_score", -1000000000.0},
         {"action", "no_action"}}};
    result.policy["actions"] = {
        {"decision_api_response",
         {{"autonomy", "A2"}, {"sla_seconds", 1},
          {"frequency_cap", frequency(1, 7)}}},
        {"high_stakes_review",
         {{"autonomy", "A1"}, {"sla_seconds", 3600},
          {"frequency_cap", frequency(1, 7)}}},
        {"no_action", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 4;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("eligible", true), predicate("consent", true),
        predicate("within_contact_cap", true)};
    result.policy["exclusion_rules"] = {
        predicate("excluded", true), predicate("do_not_contact", true),
        predicate("consent", false)};
    result.policy["budget"] = {{"limit", 1000.0}};
    result.experiment.holdouts = {{"recommended", 0.1},
                                  {"no_positive_value", 0.1}};
    result.experiment.treatment_cells = {"exploit", "explore"};
    result.guardrails = {{"consent_violation_rate", 0.0, "maximum"},
                         {"contact_cap_violation_rate", 0.0, "maximum"},
                         {"missing_propensity_rate", 0.0, "maximum"},
                         {"margin_floor_violation_rate", 0.0, "maximum"},
                         {"action_execution_rate", 0.95, "minimum"}};
}

void employee_attrition_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "elevated"}, {"minimum_score", 0.6},
         {"action", "people_leader_review"},
         {"allowed_actions", {"people_leader_review",
                              "organizational_intervention_review",
                              "no_action"}}},
        {{"name", "watch"}, {"minimum_score", 0.35},
         {"action", "aggregate_risk_report"},
         {"allowed_actions", {"aggregate_risk_report", "no_action"}}},
        {{"name", "baseline"}, {"minimum_score", 0.0},
         {"action", "no_action"}}};
    result.policy["actions"] = {
        {"people_leader_review",
         {{"autonomy", "A2"}, {"sla_seconds", 604800},
          {"frequency_cap", frequency(1, 30)}}},
        {"aggregate_risk_report",
         {{"autonomy", "A2"}, {"sla_seconds", 604800},
          {"frequency_cap", frequency(1, 30)}}},
        {"organizational_intervention_review",
         {{"autonomy", "A1"}, {"approval_above", 0.0},
          {"sla_seconds", 604800},
          {"frequency_cap", frequency(1, 30)}}},
        {"no_action", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 4;
    result.policy["frequency_cap"] = frequency(1, 30);
    result.policy["eligibility_rules"] = {
        numeric_predicate("group_size", "gte", 5.0),
        predicate("people_leader_authorized", true),
        predicate("fairness_review_passed", true),
        predicate("legal_approval_recorded", true),
        predicate("individual_level_output", false)};
    result.policy["exclusion_rules"] = {
        predicate("restricted_cohort", true),
        predicate("individual_level_output", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"elevated", 0.1}, {"watch", 0.1}, {"baseline", 0.1}};
    result.experiment.treatment_cells = {"people_leader_review"};
    result.guardrails = {
        {"small_group_outputs", 0.0, "maximum"},
        {"individual_score_outputs", 0.0, "maximum"},
        {"fairness_review_failures", 0.0, "maximum"},
        {"approval_gate_failures", 0.0, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {
        "holdout", "attrition_reduction", "fraction", 90};
}

void ticket_triage_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["threshold"] = 0.6;
    result.policy["tiers"] = {
        {{"name", "confident"}, {"minimum_score", 0.6},
         {"action", "route_ticket"},
         {"allowed_actions", {"route_ticket", "route_and_draft",
                              "close_ticket", "abstain"}}},
        {{"name", "abstain"}, {"minimum_score", -1000000000.0},
         {"action", "abstain"}}};
    result.policy["actions"] = {
        {"route_ticket",
         {{"autonomy", "A2"}, {"sla_seconds", 3600},
          {"frequency_cap", frequency(1, 1)}}},
        {"route_and_draft",
         {{"autonomy", "A2"}, {"sla_seconds", 3600},
          {"frequency_cap", frequency(1, 1)}}},
        {"close_ticket",
         {{"autonomy", "A1"}, {"sla_seconds", 3600},
          {"frequency_cap", frequency(1, 1)}}},
        {"abstain", {{"autonomy", "A0"}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 1;
    result.policy["frequency_cap"] = frequency(1, 1);
    result.policy["eligibility_rules"] = {
        predicate("service_eligible", true),
        predicate("taxonomy_owned_by_customer", true),
        predicate("taxonomy_version_current", true),
        {{"name", "queue_present"}, {"role", "entity"},
         {"field", "queue"}, {"operator", "exists"}},
        {{"name", "ticket_type_status_present"}, {"role", "entity"},
         {"field", "new_ticket_type"}, {"operator", "exists"}}};
    result.policy["exclusion_rules"] = {
        predicate("new_ticket_type", true),
        predicate("suppressed", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.policy["tunable_bounds"] = {
        {"threshold", {{"minimum", 0.5}, {"maximum", 0.95}}}};
    result.experiment.holdouts = {
        {"confident", 0.1}, {"abstain", 0.1}};
    result.experiment.treatment_cells = {"service_routed"};
    result.guardrails = {
        {"misroute_rate", 0.1, "maximum"},
        {"resolution_quality", 0.8, "minimum"},
        {"abstention_rate", 0.5, "maximum"},
        {"new_ticket_type_rate", 0.1, "maximum"},
        {"unapproved_auto_close", 0.0, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {
        "holdout", "resolution_quality", "resolution", 30};
}

void document_workflow_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "validated"}, {"minimum_score", 0.9},
         {"action", "workflow_handoff"},
         {"allowed_actions",
          {"validation_record", "workflow_review_task",
           "workflow_handoff", "high_impact_approval",
           "field_review_task"}}},
        {{"name", "review"}, {"minimum_score", 0.0},
         {"action", "field_review_task"},
         {"allowed_actions",
          {"validation_record", "workflow_review_task",
           "workflow_handoff", "high_impact_approval",
           "field_review_task"}}}};
    result.policy["actions"] = {
        {"validation_record", {{"autonomy", "A0"}}},
        {"field_review_task",
         {{"autonomy", "A2"}, {"sla_seconds", 14400},
          {"frequency_cap", frequency(1)}}},
        {"workflow_review_task",
         {{"autonomy", "A2"}, {"sla_seconds", 14400},
          {"frequency_cap", frequency(1)}}},
        {"workflow_handoff",
         {{"autonomy", "A2"}, {"sla_seconds", 3600},
          {"frequency_cap", frequency(1)}}},
        {"high_impact_approval",
         {{"autonomy", "A1"}, {"sla_seconds", 3600},
          {"frequency_cap", frequency(1)}}},
        {"no_contact", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 1;
    result.policy["frequency_cap"] = frequency(1);
    result.policy["eligibility_rules"] = {
        predicate("document_available", true)};
    result.policy["exclusion_rules"] = {
        predicate("excluded", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"validated", 0.1}, {"review", 0.1}};
    result.experiment.treatment_cells = {"workflow_enabled"};
    result.guardrails = {
        {"unvalidated_workflow_actions", 0.0, "maximum"},
        {"review_queue_miss_rate", 0.0, "maximum"},
        {"field_extraction_accuracy", 0.8, "minimum"},
        {"field_validation_rate", 0.0, "minimum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {
        "holdout", "workflow_completed", "document", 30};
}

void supplier_risk_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "critical"}, {"minimum_score", 0.65},
         {"action", "procurement_review_task"},
         {"allowed_actions", {"supplier_watch_task",
                              "procurement_review_task",
                              "supplier_mitigation_task", "no_action"}}},
        {{"name", "watch"}, {"minimum_score", 0.0},
         {"action", "supplier_watch_task"},
         {"allowed_actions", {"supplier_watch_task", "no_action"}}},
        {{"name", "monitor"}, {"minimum_score", -1.0},
         {"action", "no_action"}}};
    result.policy["actions"] = {
        {"supplier_watch_task",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"procurement_review_task",
         {{"autonomy", "A2"}, {"sla_seconds", 14400},
          {"frequency_cap", frequency(1, 7)}}},
        {"supplier_mitigation_task",
         {{"autonomy", "A2"}, {"approval_field", "mitigation_cost"},
          {"approval_above", 10000.0}, {"sla_seconds", 14400},
          {"frequency_cap", frequency(1, 7)}}},
        {"no_action", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 12;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("procurement_eligible", true),
        predicate("external_data_current", true),
        predicate("evidence_complete", true)};
    result.policy["exclusion_rules"] = {
        predicate("supplier_closed", true),
        predicate("sanctions_hold", true),
        predicate("evidence_complete", false)};
    result.policy["budget"] = {{"limit", 50000.0}};
    result.experiment.holdouts = {
        {"critical", 0.1}, {"watch", 0.1}, {"monitor", 0.1}};
    result.experiment.treatment_cells = {"procurement_action"};
    result.guardrails = {
        {"missing_flag_evidence_rate", 0.0, "maximum"},
        {"stale_external_data_rate", 0.0, "maximum"},
        {"single_source_exposure_rate", 0.5, "maximum"},
        {"mitigation_budget", 50000.0, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {"holdout", "disruption_avoided", "supplier", 60};
}

void healthcare_operations_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "high"}, {"minimum_score", 0.7},
         {"action", "care_coordination_task"},
         {"allowed_actions", {"care_coordination_task",
                              "scheduling_assistance",
                              "revenue_cycle_review",
                              "assistance_exception_review",
                              "no_action"}}},
        {{"name", "elevated"}, {"minimum_score", 0.4},
         {"action", "scheduling_assistance"},
         {"allowed_actions", {"care_coordination_task",
                              "scheduling_assistance",
                              "revenue_cycle_review",
                              "assistance_exception_review",
                              "no_action"}}},
        {{"name", "routine"}, {"minimum_score", 0.0},
         {"action", "no_action"},
         {"allowed_actions", {"revenue_cycle_review", "no_action"}}}};
    result.policy["actions"] = {
        {"care_coordination_task",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"scheduling_assistance",
         {{"autonomy", "A2"}, {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"revenue_cycle_review",
         {{"autonomy", "A2"}, {"sla_seconds", 172800},
          {"frequency_cap", frequency(1, 7)}}},
        {"assistance_exception_review",
         {{"autonomy", "A1"}, {"approval_above", 0.0},
          {"sla_seconds", 86400},
          {"frequency_cap", frequency(1, 7)}}},
        {"no_action", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 6;
    result.policy["frequency_cap"] = frequency(1, 7);
    result.policy["eligibility_rules"] = {
        predicate("operations_eligible", true),
        predicate("privacy_posture_approved", true),
        predicate("minimum_necessary_fields", true),
        predicate("equity_review_passed", true),
        predicate("care_access_unaffected", true)};
    result.policy["exclusion_rules"] = {
        predicate("patient_opt_out", true),
        predicate("clinical_decision", true),
        predicate("care_denial_requested", true),
        predicate("phi_scope_exceeded", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"high", 0.1}, {"elevated", 0.1}, {"routine", 0.1}};
    result.experiment.treatment_cells = {"assistive_work"};
    result.guardrails = {
        {"care_denial_actions", 0.0, "maximum"},
        {"equity_rate_gap", 0.2, "maximum"},
        {"phi_scope_violations", 0.0, "maximum"},
        {"privacy_posture_failures", 0.0, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {
        "holdout", "operational_resolution", "case", 30};
}

void it_security_operations_defaults(SolutionTemplates& result) {
    result.minimal = false;
    result.policy["tiers"] = {
        {{"name", "critical"}, {"minimum_score", 0.8},
         {"action", "bounded_containment"},
         {"allowed_actions", {"investigation_recommendation",
                              "containment_approval", "bounded_containment",
                              "no_action"}}},
        {{"name", "elevated"}, {"minimum_score", 0.4},
         {"action", "bounded_containment"},
         {"allowed_actions", {"investigation_recommendation",
                              "containment_approval", "bounded_containment",
                              "no_action"}}},
        {{"name", "advisory"}, {"minimum_score", 0.0},
         {"action", "no_action"}}};
    result.policy["actions"] = {
        {"investigation_recommendation",
         {{"autonomy", "A0"}, {"sla_seconds", 3600},
          {"frequency_cap", frequency(1, 1)}}},
        {"containment_approval",
         {{"autonomy", "A1"}, {"sla_seconds", 900},
          {"frequency_cap", frequency(1, 1)}}},
        {"bounded_containment",
         {{"autonomy", "A2"}, {"approval_field", "affected_assets"},
          {"approval_above", 1.0}, {"sla_seconds", 300},
          {"frequency_cap", frequency(1, 1)}}},
        {"no_action", {{"autonomy", "A0"}}}};
    result.policy["contact_cap"] = 10;
    result.policy["frequency_cap"] = frequency(1, 1);
    result.policy["eligibility_rules"] = {
        predicate("eligible", true), predicate("owner_assigned", true),
        predicate("approved_playbook", true),
        predicate("containment_allowed", true),
        predicate("dependency_scope_known", true),
        numeric_predicate("affected_assets", "lte", 1.0)};
    result.policy["exclusion_rules"] = {
        predicate("manual_only", true),
        predicate("protected_asset", true),
        predicate("active_incident", true)};
    result.policy["budget"] = {{"limit", 0.0}};
    result.experiment.holdouts = {
        {"critical", 0.1}, {"elevated", 0.1}, {"advisory", 0.1}};
    result.experiment.treatment_cells = {"operational_response"};
    result.guardrails = {
        {"alert_volume", 10.0, "maximum"},
        {"containment_scope", 1.0, "maximum"},
        {"unapproved_containment", 0.0, "maximum"},
        {"kill_switch_bypass", 0.0, "maximum"},
        {"action_execution_rate", 0.95, "minimum"}};
    result.measurement = {"holdout", "incident_avoided", "incident", 30};
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
            } else if (solution_id == "S-3") {
                lifetime_value_defaults(result);
            } else if (solution_id == "S-4") {
                collections_defaults(result);
            } else if (solution_id == "S-5") {
                demand_forecast_defaults(result);
            } else if (solution_id == "S-6") {
                replenishment_defaults(result);
            } else if (solution_id == "S-7") {
                kpi_anomaly_defaults(result);
            } else if (solution_id == "S-8") {
                predictive_maintenance_defaults(result);
            } else if (solution_id == "S-9") {
                automated_outbound_defaults(result);
            } else if (solution_id == "S-11") {
                next_best_action_defaults(result);
            } else if (solution_id == "S-12") {
                claims_triage_defaults(result);
            } else if (solution_id == "S-10") {
                fraud_decisioning_defaults(result);
            } else if (solution_id == "B-1") {
                dynamic_pricing_defaults(result);
            } else if (solution_id == "B-2") {
                employee_attrition_defaults(result);
            } else if (solution_id == "B-3") {
                ticket_triage_defaults(result);
            } else if (solution_id == "B-4") {
                document_workflow_defaults(result);
            } else if (solution_id == "B-5") {
                supplier_risk_defaults(result);
            } else if (solution_id == "B-6") {
                healthcare_operations_defaults(result);
            } else if (solution_id == "B-7") {
                credit_underwriting_defaults(result);
            } else if (solution_id == "B-8") {
                it_security_operations_defaults(result);
            }
            return result;
        }
    }
    return {};
}

Json solution_policy_template(const std::string& solution_id, int level) {
    Json policy = solution_templates(solution_id).policy;
    if (solution_id == "B-4") {
        const std::string validated_action =
            level <= 1 ? "validation_record"
                       : level == 2 ? "workflow_review_task"
                                    : "workflow_handoff";
        for (Json& tier : policy["tiers"]) {
            if (tier.at("name") == "validated") {
                tier["action"] = validated_action;
            }
        }
        return policy;
    }
    if (solution_id == "B-8") {
        const std::string action =
            level <= 1 ? "investigation_recommendation"
            : level == 2 ? "containment_approval"
                         : "bounded_containment";
        for (Json& tier : policy["tiers"]) {
            if (tier.at("name") != "advisory") {
                tier["action"] = action;
                tier["allowed_actions"] = {action, "no_action"};
            }
        }
        return policy;
    }
    if (solution_id == "B-5") {
        const std::string action =
            level <= 1 ? "supplier_watch_task"
                       : level == 2 ? "procurement_review_task"
                                    : "supplier_mitigation_task";
        for (Json& tier : policy["tiers"]) {
            if (tier.at("name") == "critical") {
                tier["action"] = action;
            }
        }
        return policy;
    }
    if (solution_id == "B-3" && level >= 2) {
        for (Json& tier : policy["tiers"]) {
            if (tier.at("name") == "confident") {
                tier["action"] = "route_and_draft";
            }
        }
        return policy;
    }
    if (solution_id != "S-6" && solution_id != "B-1") {
        return policy;
    }
    const bool replenishment = solution_id == "S-6";
    const std::string action = replenishment
                                   ? level <= 1 ? "draft_order"
                                     : level >= 4 ? "release_order"
                                                  : "erp_replenishment_task"
                                   : level <= 1 ? "price_recommendation"
                                     : level == 2 ? "pricing_review_task"
                                     : level == 3 ? "controlled_price_experiment"
                                                  : "publish_price_change";
    for (Json& tier : policy["tiers"]) {
        if ((replenishment && tier.at("name") == "replenish") ||
            (!replenishment && tier.at("name") != "no_change")) {
            tier["action"] = action;
        }
    }
    return policy;
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
