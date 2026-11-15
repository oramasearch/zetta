// SPDX-License-Identifier: MIT

#include "zetta/solutions/roi_readout.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/solutions/audit_ledger.hpp"
#include "zetta/solutions/decision_policy.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/solutions/solution_templates.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

namespace zetta::solutions {

namespace {

using Json = nlohmann::json;

constexpr double kConfidenceLevel = 0.95;
constexpr double kNormalCriticalValue = 1.959963984540054;

class ValidationError final : public nlohmann::json_schema::error_handler {
public:
    void error(
        const Json::json_pointer& pointer,
        const Json&,
        const std::string& message) override {
        if (!reason.empty()) {
            return;
        }
        const std::string path = pointer.to_string();
        reason = (path.empty() ? "/" : path) + ": " + message;
    }

    std::string reason;
};

bool validate(
    const Json& schema,
    const Json& value,
    std::string* error) {
    nlohmann::json_schema::json_validator validator(schema);
    ValidationError handler;
    validator.validate(value, handler);
    if (handler.reason.empty()) {
        return true;
    }
    if (error != nullptr) {
        *error = handler.reason;
    }
    return false;
}

Json roi_model_schema() {
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"outcome", "value_per_outcome", "investment", "currency"}},
        {"properties",
         {{"outcome", {{"type", "string"}, {"minLength", 1}}},
          {"value_per_outcome", {{"type", "number"}}},
          {"investment", {{"type", "number"}, {"minimum", 0}}},
          {"currency", {{"type", "string"}, {"minLength", 1}}}}}};
}

Json effect_estimate_schema() {
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"samples", "effect", "confidence_interval"}},
        {"properties",
         {{"samples", {{"type", "integer"}, {"minimum", 1}}},
          {"effect", {{"type", "number"}}},
          {"confidence_interval",
           {{"type", "object"},
            {"additionalProperties", false},
            {"required", {"lower", "upper"}},
            {"properties",
             {{"lower", {{"type", "number"}}},
              {"upper", {{"type", "number"}}}}}}}}}};
}

Json optional_number_schema() {
    return {{"type", {"number", "null"}}};
}

struct SampleSummary {
    double mean = 0.0;
    double variance = 0.0;
};

SampleSummary summarize(const std::vector<double>& values) {
    SampleSummary summary;
    if (values.empty()) {
        return summary;
    }
    for (const double value : values) {
        summary.mean += value;
    }
    summary.mean /= static_cast<double>(values.size());
    if (values.size() == 1) {
        return summary;
    }
    for (const double value : values) {
        const double difference = value - summary.mean;
        summary.variance += difference * difference;
    }
    summary.variance /= static_cast<double>(values.size() - 1);
    return summary;
}

EffectEstimate estimate_effect(
    const std::vector<double>& treatment,
    const std::vector<double>& holdout) {
    const SampleSummary treatment_summary = summarize(treatment);
    const SampleSummary holdout_summary = summarize(holdout);
    const double standard_error = std::sqrt(
        treatment_summary.variance / static_cast<double>(treatment.size()) +
        holdout_summary.variance / static_cast<double>(holdout.size()));
    EffectEstimate estimate;
    estimate.samples = treatment.size();
    estimate.effect = treatment_summary.mean - holdout_summary.mean;
    estimate.confidence_interval_lower =
        estimate.effect - kNormalCriticalValue * standard_error;
    estimate.confidence_interval_upper =
        estimate.effect + kNormalCriticalValue * standard_error;
    return estimate;
}

std::shared_ptr<zetta::pipeline::Project> resolve_project(
    zetta::Node& node,
    zetta::cluster::ReplicationManager& replication,
    const std::string& reference) {
    if (auto project = node.find_project(reference)) {
        return project;
    }
    // One lookup in the shared immutable view, not a deep copy (#1227).
    std::string name;
    {
        const std::shared_ptr<const Json> snapshot = replication.shared_snapshot();
        const auto found = snapshot->find(zetta::catalog::project_key(reference));
        if (found == snapshot->end() || !found->is_object()) {
            return nullptr;
        }
        const auto field = found->find("name");
        if (field == found->end() || !field->is_string()) {
            return nullptr;
        }
        name = field->get<std::string>();
    }
    return node.find_project(name);
}

std::shared_ptr<zetta::pipeline::ProjectGraph> resolve_graph(
    const std::shared_ptr<zetta::pipeline::Project>& project,
    const std::string& reference) {
    if (auto graph = project->find_graph(reference)) {
        return graph;
    }
    return project->find_graph_by_id(reference);
}

Json optional_value(const std::optional<double>& value) {
    if (!value) {
        return nullptr;
    }
    return *value;
}

Json effect_json(const EffectEstimate& estimate) {
    return {
        {"samples", estimate.samples},
        {"effect", estimate.effect},
        {"confidence_interval",
         {{"lower", estimate.confidence_interval_lower},
          {"upper", estimate.confidence_interval_upper}}}};
}

Json guardrail_panel(const std::shared_ptr<Monitor>& monitor) {
    if (!monitor) {
        return {
            {"available", false},
            {"guardrails", Json::array()},
            {"active_breaches", Json::array()},
            {"retrain_requested", false}};
    }
    Json guardrails = Json::array();
    for (const GuardrailState& state : monitor->value().guardrails) {
        guardrails.push_back(
            {{"name", state.bound.name},
             {"minimum", optional_value(state.bound.minimum)},
             {"maximum", optional_value(state.bound.maximum)},
             {"value", optional_value(state.value)},
             {"breached", state.breached},
             {"observed_at", state.observed_at}});
    }
    Json breaches = Json::array();
    for (const MonitorBreach& breach : monitor->value().breaches) {
        breaches.push_back(to_json(breach));
    }
    return {
        {"available", true},
        {"guardrails", std::move(guardrails)},
        {"active_breaches", std::move(breaches)},
        {"retrain_requested", monitor->value().retrain_requested}};
}

struct ReadoutEvidence {
    ExperimentReport experiment;
    std::map<std::string, ExperimentAssignmentValue> assignments;
    std::map<std::string, ActionLedgerValue> actions;
    std::map<std::string, double> response_by_subject;
    AgentOperationsReport operations;
    Json audit_extract = Json::array();
};

void count_action(
    const ActionLedgerValue& action,
    const std::unordered_map<std::string, DecisionRecord>& decisions,
    AgentOperationsReport& report) {
    ++report.actions;
    if (action.actor_kind == ActionActorKind::Agent) {
        ++report.agent_actions;
    } else {
        ++report.human_actions;
    }
    const auto found = decisions.find(action.decision);
    if (found == decisions.end()) {
        ++report.autonomy_unclassified;
        return;
    }
    const DecisionRecord& decision = found->second;
    if (decision.autonomy == "A0") {
        ++report.autonomy_a0;
    } else if (decision.autonomy == "A1") {
        ++report.autonomy_a1;
    } else if (decision.autonomy == "A2") {
        ++report.autonomy_a2;
    } else if (decision.autonomy == "A3") {
        ++report.autonomy_a3;
    } else {
        ++report.autonomy_unclassified;
    }
    if (decision.status == "approved" || !decision.approved_at.empty()) {
        ++report.approvals;
    }
}

std::optional<ReadoutEvidence> collect_evidence(
    zetta::Node& node,
    zetta::cluster::ReplicationManager& replication,
    const RoiReadoutDefinition& definition,
    std::string* error) {
    const auto experiment = node.experiment_manager().report(
        definition.spec.experiment,
        error);
    if (!experiment) {
        return std::nullopt;
    }
    ReadoutEvidence evidence;
    evidence.experiment = *experiment;
    for (const ExperimentAssignmentRecord& record : experiment->training_rows) {
        evidence.assignments.emplace(record.value.entity, record.value);
        if (record.value.exempt) {
            ++evidence.operations.overrides;
        }
    }

    DecisionPolicyStore policy_store{replication};
    std::unordered_map<std::string, DecisionRecord> decisions;
    for (DecisionRecord decision : policy_store.decisions()) {
        decisions.emplace(decision.id, std::move(decision));
    }

    for (const auto& entry : node.ledger_entries()) {
        const auto* action = std::get_if<ActionLedgerValue>(&entry->value());
        if (action == nullptr) {
            continue;
        }
        const auto assignment = evidence.assignments.find(action->subject);
        if (assignment == evidence.assignments.end()) {
            continue;
        }
        if (assignment->second.allocation != action->cell) {
            continue;
        }
        evidence.actions.emplace(entry->id(), *action);
        count_action(*action, decisions, evidence.operations);
    }

    for (const auto& entry : node.ledger_entries()) {
        if (entry->kind() == LedgerEntryKind::Action) {
            if (!evidence.actions.contains(entry->id())) {
                continue;
            }
            Json item = entry->metadata();
            item["id"] = entry->id();
            evidence.audit_extract.push_back(std::move(item));
            continue;
        }
        const auto* outcome = std::get_if<OutcomeLedgerValue>(&entry->value());
        if (outcome == nullptr || !evidence.actions.contains(outcome->action_id)) {
            continue;
        }
        Json item = entry->metadata();
        item["id"] = entry->id();
        item.erase("value");
        evidence.audit_extract.push_back(std::move(item));
        if (outcome->outcome != definition.spec.roi_model.outcome) {
            continue;
        }
        const ActionLedgerValue& action = evidence.actions.at(outcome->action_id);
        const auto action_time = zetta::util::parse_iso8601_seconds(action.action_at);
        const auto outcome_time = zetta::util::parse_iso8601_seconds(outcome->occurred_at);
        if (!action_time || !outcome_time || *outcome_time < *action_time ||
            *outcome_time > *action_time + definition.spec.outcome_window_days * 86400.0) {
            continue;
        }
        evidence.response_by_subject[action.subject] += outcome->value;
    }
    return evidence;
}

IncrementalityReadout build_incrementality(
    const RoiReadoutDefinition& definition,
    const zetta::pipeline::ValidationReport& correctness,
    const ReadoutEvidence& evidence) {
    IncrementalityReadout readout;
    readout.outcome = definition.spec.roi_model.outcome;
    readout.confidence_level = kConfidenceLevel;

    std::vector<double> holdout;
    std::map<std::string, std::vector<double>> treatment_cells;
    for (const ExperimentAllocation& allocation :
         evidence.experiment.experiment.value.allocations) {
        if (allocation.tier == ExperimentTier::Treatment ||
            allocation.tier == ExperimentTier::Exploration) {
            treatment_cells.emplace(allocation.name, std::vector<double>{});
        }
    }
    for (const auto& [subject, assignment] : evidence.assignments) {
        const auto response = evidence.response_by_subject.find(subject);
        if (response == evidence.response_by_subject.end()) {
            continue;
        }
        if (assignment.tier == ExperimentTier::Holdout && !assignment.exempt) {
            holdout.push_back(response->second);
            continue;
        }
        if (assignment.tier == ExperimentTier::Treatment ||
            assignment.tier == ExperimentTier::Exploration) {
            treatment_cells[assignment.allocation].push_back(response->second);
        }
    }
    readout.holdout_samples = holdout.size();

    if (definition.spec.measurement_design == "fva" ||
        definition.spec.measurement_design == "phased") {
        readout.reason = "Incrementality cannot be claimed: this measurement design requires its module-specific estimator.";
    } else if (!correctness.passed()) {
        readout.reason =
            "Incrementality cannot be claimed: correctness checks did not pass.";
    } else if (holdout.empty()) {
        readout.reason =
            "Incrementality cannot be claimed: no holdout outcomes are available.";
    } else if (treatment_cells.empty()) {
        readout.reason =
            "Incrementality cannot be claimed: no treatment cells are configured.";
    }

    bool every_cell_claimable = readout.reason.empty();
    std::vector<double> combined_treatment;
    for (const auto& [cell, values] : treatment_cells) {
        CellIncrementality cell_readout;
        cell_readout.cell = cell;
        if (!readout.reason.empty()) {
            cell_readout.reason = readout.reason;
        } else if (values.empty()) {
            cell_readout.reason =
                "Incrementality cannot be claimed: this cell has no outcomes.";
            every_cell_claimable = false;
        } else {
            cell_readout.claimable = true;
            cell_readout.estimate = estimate_effect(values, holdout);
            combined_treatment.insert(
                combined_treatment.end(),
                values.begin(),
                values.end());
        }
        readout.cells.push_back(std::move(cell_readout));
    }

    if (readout.reason.empty() && !every_cell_claimable) {
        readout.reason =
            "Incrementality cannot be claimed for the complete experiment: "
            "one or more treatment cells have no outcomes.";
    }
    if (every_cell_claimable && !combined_treatment.empty()) {
        readout.claimable = true;
        readout.combined = estimate_effect(combined_treatment, holdout);
    }
    return readout;
}

RoiStatement build_roi_statement(
    const RoiReadoutDefinition& definition,
    const IncrementalityReadout& incrementality) {
    RoiStatement statement;
    statement.model = definition.spec.roi_model;
    if (!incrementality.claimable || !incrementality.combined) {
        statement.reason = incrementality.reason;
        return statement;
    }
    statement.claimable = true;
    statement.incremental_outcomes =
        incrementality.combined->effect *
        static_cast<double>(incrementality.combined->samples);
    statement.gross_value =
        *statement.incremental_outcomes * statement.model.value_per_outcome;
    statement.net_value = *statement.gross_value - statement.model.investment;
    if (statement.model.investment > 0.0) {
        statement.return_on_investment =
            *statement.net_value / statement.model.investment;
    } else {
        statement.reason =
            "Return on investment is undefined because the customer-supplied "
            "investment is zero.";
    }
    return statement;
}

}  // namespace

Json to_json(const RoiModel& model) {
    return {
        {"outcome", model.outcome},
        {"value_per_outcome", model.value_per_outcome},
        {"investment", model.investment},
        {"currency", model.currency}};
}

Json to_json(const RoiReadoutDefinition& definition) {
    return {
        {"id", definition.id},
        {"solution", definition.spec.solution},
        {"version", definition.version},
        {"project", definition.spec.project},
        {"graph", definition.spec.graph},
        {"experiment", definition.spec.experiment},
        {"measurement_design", definition.spec.measurement_design},
        {"outcome_window_days", definition.spec.outcome_window_days},
        {"outcome_unit", definition.spec.outcome_unit},
        {"roi_model", to_json(definition.spec.roi_model)},
        {"created_by", definition.created_by},
        {"created", definition.created}};
}

Json to_json(const RoiReadout& readout) {
    Json correctness = readout.correctness_report;
    correctness["passed"] = readout.correctness_report.passed();
    Json cells = Json::array();
    for (const CellIncrementality& cell : readout.incrementality.cells) {
        Json item = {
            {"cell", cell.cell},
            {"claimable", cell.claimable},
            {"reason", cell.reason}};
        if (cell.estimate) {
            item["estimate"] = effect_json(*cell.estimate);
        }
        cells.push_back(std::move(item));
    }
    Json incrementality = {
        {"claimable", readout.incrementality.claimable},
        {"reason", readout.incrementality.reason},
        {"outcome", readout.incrementality.outcome},
        {"confidence_level", readout.incrementality.confidence_level},
        {"holdout_samples", readout.incrementality.holdout_samples},
        {"cells", std::move(cells)}};
    if (readout.incrementality.combined) {
        incrementality["combined"] =
            effect_json(*readout.incrementality.combined);
    }
    Json roi = {
        {"claimable", readout.roi.claimable},
        {"reason", readout.roi.reason},
        {"model", to_json(readout.roi.model)},
        {"incremental_outcomes", optional_value(readout.roi.incremental_outcomes)},
        {"gross_value", optional_value(readout.roi.gross_value)},
        {"net_value", optional_value(readout.roi.net_value)},
        {"return_on_investment",
         optional_value(readout.roi.return_on_investment)}};
    const Json operations = {
        {"actions", readout.agent_operations.actions},
        {"agent_actions", readout.agent_operations.agent_actions},
        {"human_actions", readout.agent_operations.human_actions},
        {"actions_by_autonomy",
         {{"A0", readout.agent_operations.autonomy_a0},
          {"A1", readout.agent_operations.autonomy_a1},
          {"A2", readout.agent_operations.autonomy_a2},
          {"A3", readout.agent_operations.autonomy_a3},
          {"unclassified", readout.agent_operations.autonomy_unclassified}}},
        {"approvals", readout.agent_operations.approvals},
        {"overrides", readout.agent_operations.overrides}};
    return {
        {"id", readout.definition.id},
        {"solution", readout.definition.spec.solution},
        {"version", readout.definition.version},
        {"generated_at", readout.generated_at},
        {"measurement", to_json(SolutionMeasurement{
            readout.definition.spec.measurement_design,
            readout.definition.spec.roi_model.outcome,
            readout.definition.spec.outcome_unit,
            readout.definition.spec.outcome_window_days})},
        {"sources",
         {{"project", readout.definition.spec.project},
          {"graph", readout.definition.spec.graph},
          {"experiment", readout.definition.spec.experiment}}},
        {"correctness_certificate", std::move(correctness)},
        {"incrementality", std::move(incrementality)},
        {"roi_statement", std::move(roi)},
        {"guardrail_panel", readout.guardrails},
        {"agent_operations", operations},
        {"audit_extract", readout.audit_extract}};
}

const Json& roi_readout_definition_schema() {
    static const Json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "zetta.RoiReadoutDefinition"},
        {"description",
         "One immutable version of a solution's ROI readout configuration (#1108)."},
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"id", "solution", "version", "project", "graph", "experiment",
          "roi_model", "created_by", "created"}},
        {"properties",
         {{"id", {{"type", "string"}, {"minLength", 1}}},
          {"solution", {{"type", "string"}, {"minLength", 1}}},
          {"version", {{"type", "integer"}, {"minimum", 1}}},
          {"project", {{"type", "string"}, {"minLength", 1}}},
          {"graph", {{"type", "string"}, {"minLength", 1}}},
          {"experiment", {{"type", "string"}, {"minLength", 1}}},
          {"measurement_design", {{"type", "string"},
              {"enum", {"holdout", "uplift_cells", "champion_challenger", "fva", "phased"}}}},
          {"outcome_window_days", {{"type", "integer"}, {"minimum", 1}}},
          {"outcome_unit", {{"type", "string"}}},
          {"roi_model", roi_model_schema()},
          {"created_by", {{"type", "string"}, {"minLength", 1}}},
          {"created", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const Json& roi_readout_schema() {
    static const Json schema = [] {
        const Json estimate = effect_estimate_schema();
        const Json integer = {{"type", "integer"}, {"minimum", 0}};
        Json value = {
            {"$schema", "http://json-schema.org/draft-07/schema#"},
            {"title", "zetta.RoiReadout"},
            {"description",
             "A versioned customer-visible correctness, incrementality, ROI, guardrail, operations, and audit readout (#1108)."},
            {"type", "object"},
            {"additionalProperties", false},
            {"required",
             {"id", "solution", "version", "generated_at", "sources",
              "correctness_certificate", "incrementality", "roi_statement",
              "guardrail_panel", "agent_operations", "audit_extract"}},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"measurement", solution_measurement_schema()},
              {"solution", {{"type", "string"}}},
              {"version", {{"type", "integer"}, {"minimum", 1}}},
              {"generated_at", {{"type", "string"}}},
              {"sources",
               {{"type", "object"},
                {"additionalProperties", false},
                {"required", {"project", "graph", "experiment"}},
                {"properties",
                 {{"project", {{"type", "string"}}},
                  {"graph", {{"type", "string"}}},
                  {"experiment", {{"type", "string"}}}}}}},
              {"correctness_certificate",
               {{"type", "object"},
                {"required", {"checks", "verdict", "block_deploy", "passed"}},
                {"properties",
                 {{"checks", {{"type", "array"}}},
                  {"verdict", {{"type", "string"}}},
                  {"block_deploy", {{"type", "boolean"}}},
                  {"passed", {{"type", "boolean"}}}}}}},
              {"incrementality",
               {{"type", "object"},
                {"additionalProperties", false},
                {"required",
                 {"claimable", "reason", "outcome", "confidence_level",
                  "holdout_samples", "cells"}},
                {"properties",
                 {{"claimable", {{"type", "boolean"}}},
                  {"reason", {{"type", "string"}}},
                  {"outcome", {{"type", "string"}}},
                  {"confidence_level", {{"type", "number"}}},
                  {"holdout_samples", integer},
                  {"cells",
                   {{"type", "array"},
                    {"items",
                     {{"type", "object"},
                      {"additionalProperties", false},
                      {"required", {"cell", "claimable", "reason"}},
                      {"properties",
                       {{"cell", {{"type", "string"}}},
                        {"claimable", {{"type", "boolean"}}},
                        {"reason", {{"type", "string"}}},
                        {"estimate", estimate}}}}}}},
                  {"combined", estimate}}}}},
              {"roi_statement",
               {{"type", "object"},
                {"additionalProperties", false},
                {"required",
                 {"claimable", "reason", "model", "incremental_outcomes",
                  "gross_value", "net_value", "return_on_investment"}},
                {"properties",
                 {{"claimable", {{"type", "boolean"}}},
                  {"reason", {{"type", "string"}}},
                  {"model", roi_model_schema()},
                  {"incremental_outcomes", optional_number_schema()},
                  {"gross_value", optional_number_schema()},
                  {"net_value", optional_number_schema()},
                  {"return_on_investment", optional_number_schema()}}}}},
              {"guardrail_panel", {{"type", "object"}}},
              {"agent_operations",
               {{"type", "object"},
                {"additionalProperties", false},
                {"required",
                 {"actions", "agent_actions", "human_actions",
                  "actions_by_autonomy", "approvals", "overrides"}},
                {"properties",
                 {{"actions", integer},
                  {"agent_actions", integer},
                  {"human_actions", integer},
                  {"actions_by_autonomy", {{"type", "object"}}},
                  {"approvals", integer},
                  {"overrides", integer}}}}},
              {"audit_extract",
               {{"type", "array"},
                {"items",
                 {{"type", "object"},
                  {"not", {{"required", {"value"}}}}}}}}}}};
        return value;
    }();
    return schema;
}

std::optional<RoiReadoutDefinition> parse_roi_readout_definition(
    const Json& value,
    std::string* error) {
    if (!validate(roi_readout_definition_schema(), value, error)) {
        return std::nullopt;
    }
    RoiReadoutDefinition definition;
    definition.id = value.at("id").get<std::string>();
    definition.version = value.at("version").get<std::uint64_t>();
    definition.spec.solution = value.at("solution").get<std::string>();
    definition.spec.project = value.at("project").get<std::string>();
    definition.spec.graph = value.at("graph").get<std::string>();
    definition.spec.experiment = value.at("experiment").get<std::string>();
    definition.spec.measurement_design = value.value("measurement_design", "holdout");
    definition.spec.outcome_window_days = value.value("outcome_window_days", 30);
    definition.spec.outcome_unit = value.value("outcome_unit", "");
    const Json& model = value.at("roi_model");
    definition.spec.roi_model.outcome = model.at("outcome").get<std::string>();
    definition.spec.roi_model.value_per_outcome =
        model.at("value_per_outcome").get<double>();
    definition.spec.roi_model.investment =
        model.at("investment").get<double>();
    definition.spec.roi_model.currency = model.at("currency").get<std::string>();
    definition.created_by = value.at("created_by").get<std::string>();
    definition.created = value.at("created").get<std::string>();
    if (!std::isfinite(definition.spec.roi_model.value_per_outcome)) {
        if (error != nullptr) {
            *error = "/roi_model/value_per_outcome: must be finite";
        }
        return std::nullopt;
    }
    if (!std::isfinite(definition.spec.roi_model.investment)) {
        if (error != nullptr) {
            *error = "/roi_model/investment: must be finite";
        }
        return std::nullopt;
    }
    return definition;
}

RoiReadoutStore::RoiReadoutStore(
    zetta::cluster::ReplicationManager& replication)
    : replication_(replication) {}

std::vector<RoiReadoutDefinition> RoiReadoutStore::versions(
    std::string_view solution) const {
    std::vector<RoiReadoutDefinition> found;
    // The `roi_readout:` key range of the shared immutable view (#1227).
    const std::shared_ptr<const Json> snapshot = replication_.shared_snapshot();
    zetta::catalog::for_each_with_prefix(
        *snapshot, zetta::catalog::kRoiReadoutPrefix,
        [&](const std::string&, const Json& value) {
            const auto definition = parse_roi_readout_definition(value);
            if (definition && definition->spec.solution == solution) {
                found.push_back(*definition);
            }
        });
    std::ranges::sort(
        found,
        [](const RoiReadoutDefinition& left,
           const RoiReadoutDefinition& right) {
            if (left.version != right.version) {
                return left.version < right.version;
            }
            if (left.created != right.created) {
                return left.created < right.created;
            }
            return left.id < right.id;
        });
    return found;
}

std::optional<RoiReadoutDefinition> RoiReadoutStore::configure(
    RoiReadoutSpec spec,
    std::string actor,
    std::string* error) {
    if (actor.empty()) {
        if (error != nullptr) {
            *error = "/created_by: must not be empty";
        }
        return std::nullopt;
    }
    const std::vector<RoiReadoutDefinition> existing = versions(spec.solution);
    RoiReadoutDefinition definition;
    definition.id = zetta::util::new_uuid();
    definition.version = existing.empty() ? 1 : existing.back().version + 1;
    definition.spec = std::move(spec);
    definition.created_by = std::move(actor);
    definition.created = zetta::util::now_iso8601();
    const Json value = to_json(definition);
    if (!parse_roi_readout_definition(value, error)) {
        return std::nullopt;
    }
    replication_.set(
        zetta::catalog::roi_readout_key(definition.id),
        value);
    return definition;
}

std::optional<RoiReadoutDefinition> RoiReadoutStore::get(
    std::string_view solution_or_id,
    std::string* error) const {
    {
        const std::shared_ptr<const Json> snapshot = replication_.shared_snapshot();
        const auto by_id = snapshot->find(
            zetta::catalog::roi_readout_key(std::string(solution_or_id)));
        if (by_id != snapshot->end()) {
            return parse_roi_readout_definition(*by_id, error);
        }
    }
    const std::vector<RoiReadoutDefinition> found = versions(solution_or_id);
    if (found.empty()) {
        if (error != nullptr) {
            *error = "no ROI readout configured for solution or id '" +
                     std::string(solution_or_id) + "'";
        }
        return std::nullopt;
    }
    return found.back();
}

std::optional<RoiReadout> generate_roi_readout(
    zetta::Node& node,
    zetta::cluster::ReplicationManager& replication,
    std::string_view solution_or_id,
    std::string* error) {
    RoiReadoutStore store{replication};
    const auto definition = store.get(solution_or_id, error);
    if (!definition) {
        return std::nullopt;
    }
    const auto project = resolve_project(
        node,
        replication,
        definition->spec.project);
    if (!project) {
        if (error != nullptr) {
            *error = "no project named or identified by '" +
                     definition->spec.project + "'";
        }
        return std::nullopt;
    }
    const auto graph = resolve_graph(project, definition->spec.graph);
    if (!graph) {
        if (error != nullptr) {
            *error = "no graph named or identified by '" +
                     definition->spec.graph + "' on project '" +
                     project->name() + "'";
        }
        return std::nullopt;
    }
    const zetta::pipeline::ValidationReport correctness =
        zetta::pipeline::default_gate().run(graph->graph());
    const auto evidence = collect_evidence(
        node,
        replication,
        *definition,
        error);
    if (!evidence) {
        return std::nullopt;
    }
    RoiReadout readout;
    readout.definition = *definition;
    readout.generated_at = zetta::util::now_iso8601();
    readout.correctness_report = correctness;
    readout.incrementality = build_incrementality(
        *definition,
        correctness,
        *evidence);
    readout.roi = build_roi_statement(*definition, readout.incrementality);
    readout.guardrails = guardrail_panel(
        node.find_monitor(definition->spec.solution));
    readout.agent_operations = evidence->operations;
    readout.audit_extract = evidence->audit_extract;
    return readout;
}

}  // namespace zetta::solutions
