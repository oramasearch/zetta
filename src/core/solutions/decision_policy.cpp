#include "zetta/solutions/decision_policy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/replication.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

namespace zetta::solutions {

namespace {

using Json = nlohmann::json;

const Json* dotted_value(const Json& object, std::string_view path);

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

bool validate_policy_config(const Json& config, std::string* error) {
    if (!validate(decision_policy_config_schema(), config, error)) {
        return false;
    }
    for (const auto& [path, bound] :
         config.value("tunable_bounds", Json::object()).items()) {
        if (bound.at("minimum").get<double>() > bound.at("maximum").get<double>()) {
            if (error != nullptr) {
                *error = "/tunable_bounds/" + path +
                         ": minimum must not exceed maximum";
            }
            return false;
        }
        const Json* value = dotted_value(config, path);
        if (value == nullptr || !value->is_number()) {
            if (error != nullptr) {
                *error = "/tunable_bounds/" + path +
                         ": path must name a configured numeric parameter";
            }
            return false;
        }
        const double configured = value->get<double>();
        const double minimum = bound.at("minimum").get<double>();
        const double maximum = bound.at("maximum").get<double>();
        if (configured < minimum || configured > maximum) {
            if (error != nullptr) {
                *error = "/tunable_bounds/" + path +
                         ": configured value must be inside declared bounds";
            }
            return false;
        }
    }
    return true;
}

const Json& rule_schema() {
    static const Json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"field", "operator"}},
        {"properties",
         {{"field", {{"type", "string"}, {"minLength", 1}}},
          {"operator",
           {{"type", "string"},
            {"enum", {"eq", "neq", "lt", "lte", "gt", "gte", "in", "not_in", "exists"}}}},
          {"value", Json::object()}}}};
    return schema;
}

const Json& bound_schema() {
    static const Json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"minimum", "maximum"}},
        {"properties",
         {{"minimum", {{"type", "number"}}},
          {"maximum", {{"type", "number"}}}}}};
    return schema;
}

const Json* dotted_value(const Json& object, std::string_view path) {
    const Json* value = &object;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const std::size_t end = path.find('.', begin);
        const std::string part(path.substr(
            begin,
            end == std::string_view::npos ? path.size() - begin : end - begin));
        if (!value->is_object()) {
            return nullptr;
        }
        const auto found = value->find(part);
        if (found == value->end()) {
            return nullptr;
        }
        value = &*found;
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return value;
}

bool numeric_compare(const Json& left, const Json& right, std::string_view operation) {
    if (!left.is_number() || !right.is_number()) {
        return false;
    }
    const double lhs = left.get<double>();
    const double rhs = right.get<double>();
    if (operation == "lt") {
        return lhs < rhs;
    }
    if (operation == "lte") {
        return lhs <= rhs;
    }
    if (operation == "gt") {
        return lhs > rhs;
    }
    return lhs >= rhs;
}

bool matches_rule(const Json& evidence, const Json& rule) {
    const Json* actual = dotted_value(evidence, rule.at("field").get<std::string>());
    const std::string operation = rule.at("operator").get<std::string>();
    if (operation == "exists") {
        return actual != nullptr;
    }
    if (actual == nullptr || !rule.contains("value")) {
        return false;
    }
    const Json& expected = rule.at("value");
    if (operation == "eq") {
        return *actual == expected;
    }
    if (operation == "neq") {
        return *actual != expected;
    }
    if (operation == "lt" || operation == "lte" || operation == "gt" || operation == "gte") {
        return numeric_compare(*actual, expected, operation);
    }
    if (!expected.is_array()) {
        return false;
    }
    const bool found = std::ranges::find(expected, *actual) != expected.end();
    return operation == "in" ? found : !found;
}

std::string json_text(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

double number_or(const Json& object, std::string_view key, double fallback) {
    const auto found = object.find(std::string(key));
    if (found == object.end() || !found->is_number()) {
        return fallback;
    }
    return found->get<double>();
}

std::size_t count_or(const Json& object, std::string_view key) {
    const double value = number_or(object, key, 0.0);
    if (value <= 0.0) {
        return 0;
    }
    return static_cast<std::size_t>(value);
}

double committed_spend(
    const Json& snapshot,
    const std::string& policy_id,
    std::uint64_t version) {
    double spend = 0.0;
    for (const auto& [key, value] : snapshot.items()) {
        if (!key.starts_with(std::string(zetta::catalog::kDecisionPrefix))) {
            continue;
        }
        const auto decision = parse_decision_record(value);
        if (!decision || decision->policy_id != policy_id || decision->policy_version != version) {
            continue;
        }
        if (decision->status == "allowed" || decision->status == "pending_approval" ||
            decision->status == "approved") {
            spend += decision->cost;
        }
    }
    return spend;
}

bool apply_tuning(
    Json& config,
    const Json& tuning,
    std::string* error) {
    const Json& bounds = config.value("tunable_bounds", Json::object());
    for (const auto& [path, value] : tuning.items()) {
        const auto bound = bounds.find(path);
        if (bound == bounds.end()) {
            if (error != nullptr) {
                *error = "/tuning/" + path + ": parameter is not agent-tunable";
            }
            return false;
        }
        if (!value.is_number()) {
            if (error != nullptr) {
                *error = "/tuning/" + path + ": value must be numeric";
            }
            return false;
        }
        const double tuned = value.get<double>();
        const double minimum = bound->at("minimum").get<double>();
        const double maximum = bound->at("maximum").get<double>();
        if (tuned < minimum || tuned > maximum) {
            if (error != nullptr) {
                *error = "/tuning/" + path + ": value is outside declared bounds";
            }
            return false;
        }
        Json* target = &config;
        std::size_t begin = 0;
        while (begin <= path.size()) {
            const std::size_t end = path.find('.', begin);
            const std::string part = path.substr(
                begin,
                end == std::string::npos ? std::string::npos : end - begin);
            if (end == std::string::npos) {
                (*target)[part] = value;
                break;
            }
            target = &(*target)[part];
            begin = end + 1;
        }
    }
    return true;
}

DecisionRecord evaluate(
    const DecisionPolicy& policy,
    const Json& entity,
    double spend) {
    const PolicyVersion& current = *policy.current();
    const Json& config = current.config;
    DecisionRecord decision;
    decision.id = zetta::util::new_uuid();
    decision.policy_id = policy.id;
    decision.policy_version = current.version;
    decision.entity_id = json_text(entity.at("entity_id"));
    decision.score = entity.at("score").get<double>();
    decision.cost = number_or(entity, "cost", 0.0);
    decision.autonomy = config.value("autonomy", std::string("A0"));
    decision.holdout = entity.value("holdout", false);
    decision.created = zetta::util::now_iso8601();
    decision.evidence = entity;

    if (config.value("kill_switch", false)) {
        decision.status = "blocked";
        decision.reasons.push_back("policy kill-switch is active");
        return decision;
    }

    for (const Json& rule : config.value("exclusion_rules", Json::array())) {
        if (matches_rule(entity, rule)) {
            decision.status = "blocked";
            decision.reasons.push_back("an exclusion rule matched: " + rule.at("field").get<std::string>());
            return decision;
        }
    }
    for (const Json& rule : config.value("eligibility_rules", Json::array())) {
        if (!matches_rule(entity, rule)) {
            decision.status = "blocked";
            decision.reasons.push_back("an eligibility rule failed: " + rule.at("field").get<std::string>());
            return decision;
        }
    }

    const std::size_t contacts = count_or(entity, "contacts");
    const std::size_t contact_cap = config.value("contact_cap", std::numeric_limits<std::size_t>::max());
    if (contacts >= contact_cap) {
        decision.status = "blocked";
        decision.reasons.push_back("contact cap reached");
        return decision;
    }
    const Json frequency = config.value("frequency_cap", Json::object());
    const std::size_t recent_actions = count_or(entity, "recent_actions");
    const std::size_t maximum_actions = frequency.value(
        "max_actions",
        std::numeric_limits<std::size_t>::max());
    if (recent_actions >= maximum_actions) {
        decision.status = "blocked";
        decision.reasons.push_back("frequency cap reached");
        return decision;
    }

    const Json budget = config.value("budget", Json::object());
    const double budget_limit = budget.value("limit", std::numeric_limits<double>::infinity());
    if (spend + decision.cost > budget_limit) {
        decision.status = "blocked";
        decision.reasons.push_back("budget exhausted");
        return decision;
    }

    const double threshold = config.value("threshold", -std::numeric_limits<double>::infinity());
    if (decision.score < threshold) {
        decision.status = "blocked";
        decision.reasons.push_back("score is below the policy threshold");
        return decision;
    }

    double selected_minimum = -std::numeric_limits<double>::infinity();
    for (const Json& tier : config.value("tiers", Json::array())) {
        const double minimum = tier.at("minimum_score").get<double>();
        if (decision.score >= minimum && minimum >= selected_minimum) {
            selected_minimum = minimum;
            decision.tier = tier.at("name").get<std::string>();
            decision.action = tier.at("action").get<std::string>();
        }
    }
    if (decision.action.empty()) {
        decision.action = entity.value("action", std::string{});
    }

    if (decision.holdout) {
        decision.status = "blocked";
        decision.reasons.push_back(
            "entity is in a holdout cell and must not be delivered");
        return decision;
    }

    if (decision.autonomy == "A0") {
        decision.status = "manual";
        decision.reasons.push_back("A0 records a recommendation without authorizing action");
        return decision;
    }
    if (decision.autonomy == "A1") {
        decision.status = "pending_approval";
        decision.reasons.push_back("A1 requires approval before action");
        return decision;
    }
    decision.status = "allowed";
    decision.actionable = true;
    decision.reasons.push_back(decision.autonomy + " permits action within policy constraints");
    return decision;
}

void audit(
    zetta::governance::AuditSink* sink,
    zetta::governance::AuditAction action,
    const std::string& actor,
    const std::string& target,
    const std::string& detail) {
    if (sink == nullptr) {
        return;
    }
    sink->record_audit_event({action, actor, target, true, detail, {}});
}

}  // namespace

std::string_view to_string(AutonomyClass autonomy) {
    switch (autonomy) {
        case AutonomyClass::A0:
            return "A0";
        case AutonomyClass::A1:
            return "A1";
        case AutonomyClass::A2:
            return "A2";
        case AutonomyClass::A3:
            return "A3";
    }
    return "A0";
}

std::optional<AutonomyClass> autonomy_class_from(std::string_view value) {
    if (value == "A0") {
        return AutonomyClass::A0;
    }
    if (value == "A1") {
        return AutonomyClass::A1;
    }
    if (value == "A2") {
        return AutonomyClass::A2;
    }
    if (value == "A3") {
        return AutonomyClass::A3;
    }
    return std::nullopt;
}

const PolicyVersion* DecisionPolicy::current() const {
    if (versions.empty()) {
        return nullptr;
    }
    return &versions.back();
}

Json to_json(const PolicyVersion& version) {
    return {{"version", version.version},
            {"config", version.config},
            {"actor", version.actor},
            {"reason", version.reason},
            {"created", version.created}};
}

Json to_json(const DecisionPolicy& policy) {
    Json versions = Json::array();
    for (const PolicyVersion& version : policy.versions) {
        versions.push_back(to_json(version));
    }
    return {{"id", policy.id},
            {"name", policy.name},
            {"current_version", policy.current() == nullptr ? 0 : policy.current()->version},
            {"versions", std::move(versions)},
            {"created", policy.created},
            {"updated", policy.updated}};
}

Json to_json(const DecisionRecord& decision) {
    return {{"id", decision.id},
            {"policy_id", decision.policy_id},
            {"policy_version", decision.policy_version},
            {"entity_id", decision.entity_id},
            {"score", decision.score},
            {"cost", decision.cost},
            {"tier", decision.tier},
            {"experiment", decision.experiment},
            {"cell", decision.cell},
            {"action", decision.action},
            {"autonomy", decision.autonomy},
            {"status", decision.status},
            {"holdout", decision.holdout},
            {"actionable", decision.actionable},
            {"reasons", decision.reasons},
            {"evidence", decision.evidence},
            {"created", decision.created},
            {"approved_at", decision.approved_at},
            {"approved_by", decision.approved_by}};
}

const Json& decision_policy_config_schema() {
    static const Json schema = [] {
        Json value = {
            {"$schema", "http://json-schema.org/draft-07/schema#"},
            {"title", "zetta.DecisionPolicyConfig"},
            {"type", "object"},
            {"additionalProperties", false},
            {"required", {"autonomy"}},
            {"properties",
             {{"kill_switch", {{"type", "boolean"}}},
              {"threshold", {{"type", "number"}}},
              {"tiers",
               {{"type", "array"},
                {"items",
                 {{"type", "object"},
                  {"additionalProperties", false},
                  {"required", {"name", "minimum_score", "action"}},
                  {"properties",
                   {{"name", {{"type", "string"}, {"minLength", 1}}},
                    {"minimum_score", {{"type", "number"}}},
                    {"action", {{"type", "string"}, {"minLength", 1}}}}}}}}},
              {"budget",
               {{"type", "object"},
                {"additionalProperties", false},
                {"required", {"limit"}},
                {"properties", {{"limit", {{"type", "number"}, {"minimum", 0}}}}}}},
              {"contact_cap", {{"type", "integer"}, {"minimum", 0}}},
              {"frequency_cap",
               {{"type", "object"},
                {"additionalProperties", false},
                {"required", {"max_actions", "window_seconds"}},
                {"properties",
                 {{"max_actions", {{"type", "integer"}, {"minimum", 0}}},
                  {"window_seconds", {{"type", "integer"}, {"minimum", 1}}}}}}},
              {"eligibility_rules", {{"type", "array"}, {"items", rule_schema()}}},
              {"exclusion_rules", {{"type", "array"}, {"items", rule_schema()}}},
              {"autonomy", {{"type", "string"}, {"enum", {"A0", "A1", "A2", "A3"}}}},
              {"tunable_bounds",
               {{"type", "object"}, {"additionalProperties", bound_schema()}}}}}};
        return value;
    }();
    return schema;
}

const Json& decision_policy_schema() {
    static const Json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "zetta.DecisionPolicy"},
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"id", "name", "current_version", "versions", "created", "updated"}},
        {"properties",
         {{"id", {{"type", "string"}, {"minLength", 1}}},
          {"name", {{"type", "string"}, {"minLength", 1}}},
          {"current_version", {{"type", "integer"}, {"minimum", 1}}},
          {"versions",
           {{"type", "array"},
            {"minItems", 1},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"version", "config", "actor", "reason", "created"}},
              {"properties",
               {{"version", {{"type", "integer"}, {"minimum", 1}}},
                {"config", decision_policy_config_schema()},
                {"actor", {{"type", "string"}}},
                {"reason", {{"type", "string"}}},
                {"created", {{"type", "string"}}}}}}}}},
          {"created", {{"type", "string"}}},
          {"updated", {{"type", "string"}}}}}};
    return schema;
}

const Json& decision_record_schema() {
    static const Json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "zetta.DecisionRecord"},
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"id", "policy_id", "policy_version", "entity_id", "score", "cost", "tier",
          "experiment", "cell", "action", "autonomy", "status", "actionable", "reasons",
          "evidence", "created", "approved_at", "approved_by"}},
        {"properties",
         {{"id", {{"type", "string"}, {"minLength", 1}}},
          {"policy_id", {{"type", "string"}, {"minLength", 1}}},
          {"policy_version", {{"type", "integer"}, {"minimum", 1}}},
          {"entity_id", {{"type", "string"}, {"minLength", 1}}},
          {"score", {{"type", "number"}}},
          {"cost", {{"type", "number"}, {"minimum", 0}}},
          {"tier", {{"type", "string"}}},
          {"experiment", {{"type", "string"}}},
          {"cell", {{"type", "string"}}},
          {"action", {{"type", "string"}}},
          {"autonomy", {{"type", "string"}, {"enum", {"A0", "A1", "A2", "A3"}}}},
          {"status", {{"type", "string"}, {"enum", {"blocked", "manual", "pending_approval", "allowed", "approved"}}}},
          {"holdout", {{"type", "boolean"}}},
          {"actionable", {{"type", "boolean"}}},
          {"reasons", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"evidence", {{"type", "object"}}},
          {"created", {{"type", "string"}}},
          {"approved_at", {{"type", "string"}}},
          {"approved_by", {{"type", "string"}}}}}};
    return schema;
}

std::optional<DecisionPolicy> parse_decision_policy(const Json& value, std::string* error) {
    if (!validate(decision_policy_schema(), value, error)) {
        return std::nullopt;
    }
    DecisionPolicy policy;
    policy.id = value.at("id").get<std::string>();
    policy.name = value.at("name").get<std::string>();
    std::uint64_t expected_version = 1;
    for (const Json& item : value.at("versions")) {
        const std::uint64_t version = item.at("version").get<std::uint64_t>();
        if (version != expected_version) {
            if (error != nullptr) {
                *error = "/versions: version numbers must be contiguous from 1";
            }
            return std::nullopt;
        }
        std::string config_error;
        if (!validate_policy_config(item.at("config"), &config_error)) {
            if (error != nullptr) {
                *error = "/versions/" + std::to_string(expected_version - 1) +
                         "/config" + config_error;
            }
            return std::nullopt;
        }
        policy.versions.push_back({item.at("version").get<std::uint64_t>(),
                                   item.at("config"),
                                   item.at("actor").get<std::string>(),
                                   item.at("reason").get<std::string>(),
                                   item.at("created").get<std::string>()});
        ++expected_version;
    }
    policy.created = value.at("created").get<std::string>();
    policy.updated = value.at("updated").get<std::string>();
    if (policy.current() == nullptr || policy.current()->version != value.at("current_version").get<std::uint64_t>()) {
        if (error != nullptr) {
            *error = "/current_version: must name the last immutable version";
        }
        return std::nullopt;
    }
    return policy;
}

std::optional<DecisionRecord> parse_decision_record(const Json& value, std::string* error) {
    if (!validate(decision_record_schema(), value, error)) {
        return std::nullopt;
    }
    DecisionRecord decision;
    decision.id = value.at("id").get<std::string>();
    decision.policy_id = value.at("policy_id").get<std::string>();
    decision.policy_version = value.at("policy_version").get<std::uint64_t>();
    decision.entity_id = value.at("entity_id").get<std::string>();
    decision.score = value.at("score").get<double>();
    decision.cost = value.at("cost").get<double>();
    decision.tier = value.at("tier").get<std::string>();
    decision.experiment = value.at("experiment").get<std::string>();
    decision.cell = value.at("cell").get<std::string>();
    decision.action = value.at("action").get<std::string>();
    decision.autonomy = value.at("autonomy").get<std::string>();
    decision.status = value.at("status").get<std::string>();
    decision.holdout = value.value("holdout", false);
    decision.actionable = value.at("actionable").get<bool>();
    decision.reasons = value.at("reasons").get<std::vector<std::string>>();
    decision.evidence = value.at("evidence");
    decision.created = value.at("created").get<std::string>();
    decision.approved_at = value.at("approved_at").get<std::string>();
    decision.approved_by = value.at("approved_by").get<std::string>();
    return decision;
}

DecisionPolicyStore::DecisionPolicyStore(
    zetta::cluster::ReplicationManager& replication,
    zetta::governance::AuditSink* audit_sink,
    ExperimentManager* experiment_manager)
    : replication_(replication),
      audit_(audit_sink),
      experiment_manager_(experiment_manager) {}

std::optional<DecisionPolicy> DecisionPolicyStore::create(
    std::string name,
    Json config,
    std::string actor,
    std::string* error) {
    if (name.empty()) {
        if (error != nullptr) {
            *error = "/name: must not be empty";
        }
        return std::nullopt;
    }
    if (!validate_policy_config(config, error)) {
        return std::nullopt;
    }
    const Json snapshot = replication_.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (!key.starts_with(std::string(zetta::catalog::kDecisionPolicyPrefix))) {
            continue;
        }
        const auto existing = parse_decision_policy(value);
        if (existing && existing->name == name) {
            if (error != nullptr) {
                *error = "/name: a decision policy with this name already exists";
            }
            return std::nullopt;
        }
    }
    const std::string now = zetta::util::now_iso8601();
    DecisionPolicy policy;
    policy.id = zetta::util::new_uuid();
    policy.name = std::move(name);
    policy.versions.push_back({1, std::move(config), actor, "initial version", now});
    policy.created = now;
    policy.updated = now;
    replication_.set(zetta::catalog::decision_policy_key(policy.id), to_json(policy));
    audit(audit_, zetta::governance::AuditAction::DecisionPolicyCreated, actor,
          policy.id, "created decision policy version 1");
    return policy;
}

std::optional<DecisionPolicy> DecisionPolicyStore::get(const std::string& id) const {
    const Json snapshot = replication_.snapshot();
    const auto found = snapshot.find(zetta::catalog::decision_policy_key(id));
    if (found == snapshot.end()) {
        return std::nullopt;
    }
    return parse_decision_policy(*found);
}

std::optional<DecisionPolicy> DecisionPolicyStore::update(
    const std::string& id,
    std::optional<Json> replacement,
    std::optional<Json> tuning,
    std::string actor,
    std::string reason,
    std::string* error) {
    std::optional<DecisionPolicy> policy = get(id);
    if (!policy) {
        if (error != nullptr) {
            *error = "no decision policy with id '" + id + "'";
        }
        return std::nullopt;
    }
    if (replacement.has_value() == tuning.has_value()) {
        if (error != nullptr) {
            *error = "/: provide exactly one of config or tuning";
        }
        return std::nullopt;
    }
    Json config = replacement ? std::move(*replacement) : policy->current()->config;
    if (tuning && !apply_tuning(config, *tuning, error)) {
        return std::nullopt;
    }
    if (!validate_policy_config(config, error)) {
        return std::nullopt;
    }
    const std::string now = zetta::util::now_iso8601();
    const std::uint64_t next = policy->current()->version + 1;
    policy->versions.push_back({next, std::move(config), actor, std::move(reason), now});
    policy->updated = now;
    replication_.set(zetta::catalog::decision_policy_key(id), to_json(*policy));
    audit(audit_, zetta::governance::AuditAction::DecisionPolicyUpdated, actor,
          id, "created decision policy version " + std::to_string(next));
    return policy;
}

std::optional<DecisionRecord> DecisionPolicyStore::decide(
    const std::string& policy_id,
    Json entity,
    std::string,
    std::string* error) {
    const std::optional<DecisionPolicy> policy = get(policy_id);
    if (!policy) {
        if (error != nullptr) {
            *error = "no decision policy with id '" + policy_id + "'";
        }
        return std::nullopt;
    }
    if (!entity.is_object() || !entity.contains("entity_id") || !entity.contains("score") ||
        !(entity.at("entity_id").is_string() || entity.at("entity_id").is_number()) ||
        !entity.at("score").is_number()) {
        if (error != nullptr) {
            *error = "/entity: requires string-or-number entity_id and numeric score";
        }
        return std::nullopt;
    }
    if (number_or(entity, "cost", 0.0) < 0.0) {
        if (error != nullptr) {
            *error = "/entity/cost: must not be negative";
        }
        return std::nullopt;
    }
    if (entity.contains("holdout") && !entity.at("holdout").is_boolean()) {
        if (error != nullptr) {
            *error = "/entity/holdout: must be a boolean";
        }
        return std::nullopt;
    }
    const Json snapshot = replication_.snapshot();
    double spend = 0.0;
    if (policy->current()->config.contains("budget")) {
        spend = committed_spend(snapshot, policy_id, policy->current()->version);
    }
    DecisionRecord decision = evaluate(*policy, entity, spend);
    if (entity.contains("experiment")) {
        if (experiment_manager_ == nullptr) {
            if (error != nullptr) {
                *error = "/entity/experiment: no experiment manager is available";
            }
            return std::nullopt;
        }
        if (!entity.at("experiment").is_string() ||
            entity.at("experiment").get_ref<const std::string&>().empty()) {
            if (error != nullptr) {
                *error = "/entity/experiment: must be a non-empty string";
            }
            return std::nullopt;
        }
        const auto experiment_decision = experiment_manager_->decide(
            entity.at("experiment").get_ref<const std::string&>(),
            decision.entity_id,
            decision.action,
            true,
            false,
            {},
            {},
            error);
        if (!experiment_decision) {
            return std::nullopt;
        }
        decision.experiment = experiment_decision->assignment.value.experiment;
        decision.cell = experiment_decision->assignment.value.allocation;
        if (!experiment_decision->allowed) {
            decision.action.clear();
            decision.actionable = false;
            decision.status = "blocked";
            decision.reasons.push_back(
                "experiment cell '" + decision.cell +
                "' protects model-driven action");
        }
    }
    replication_.set(zetta::catalog::decision_key(decision.id), to_json(decision));
    return decision;
}

std::optional<DecisionRecord> DecisionPolicyStore::get_decision(
    const std::string& decision_id) const {
    const Json snapshot = replication_.snapshot();
    const auto found = snapshot.find(zetta::catalog::decision_key(decision_id));
    if (found == snapshot.end()) {
        return std::nullopt;
    }
    return parse_decision_record(*found);
}

std::vector<DecisionRecord> DecisionPolicyStore::decisions() const {
    std::vector<DecisionRecord> records;
    const Json snapshot = replication_.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (!key.starts_with(std::string(zetta::catalog::kDecisionPrefix))) {
            continue;
        }
        if (const auto record = parse_decision_record(value)) {
            records.push_back(*record);
        }
    }
    std::ranges::sort(
        records,
        [](const DecisionRecord& left, const DecisionRecord& right) {
            if (left.created != right.created) {
                return left.created < right.created;
            }
            return left.id < right.id;
        });
    return records;
}

std::optional<DecisionRecord> DecisionPolicyStore::approve(
    const std::string& decision_id,
    std::string actor,
    std::string* error) {
    std::optional<DecisionRecord> decision = get_decision(decision_id);
    if (!decision) {
        if (error != nullptr) {
            *error = "no decision with id '" + decision_id + "'";
        }
        return std::nullopt;
    }
    if (decision->autonomy != "A1" || decision->status != "pending_approval") {
        if (error != nullptr) {
            *error = "only a pending A1 decision can be approved";
        }
        return std::nullopt;
    }
    const std::optional<DecisionPolicy> policy = get(decision->policy_id);
    if (!policy) {
        if (error != nullptr) {
            *error = "decision policy no longer exists";
        }
        return std::nullopt;
    }
    if (policy->current()->config.value("kill_switch", false)) {
        if (error != nullptr) {
            *error = "policy kill-switch is active";
        }
        return std::nullopt;
    }
    decision->status = "approved";
    decision->actionable = true;
    decision->approved_at = zetta::util::now_iso8601();
    decision->approved_by = actor;
    decision->reasons.push_back("approved by " + actor);
    replication_.set(zetta::catalog::decision_key(decision_id), to_json(*decision));
    audit(audit_, zetta::governance::AuditAction::DecisionApproved, actor,
          decision_id, "approved A1 decision under policy version " +
                           std::to_string(decision->policy_version));
    return decision;
}

std::optional<std::string> DecisionPolicyStore::delivery_blocker(
    const DecisionRecord& decision) const {
    if (decision.holdout) {
        return "entity is in a holdout cell and must not be delivered";
    }
    if (!decision.actionable) {
        return "decision is not actionable";
    }
    const std::optional<DecisionPolicy> policy = get(decision.policy_id);
    if (!policy || policy->current() == nullptr) {
        return "decision policy no longer exists";
    }
    if (policy->current()->config.value("kill_switch", false)) {
        return "policy kill-switch is active";
    }
    return std::nullopt;
}

BatchDecisionResult decide_table(
    zetta::Node& node,
    DecisionPolicyStore& store,
    const std::string& policy_id,
    const BatchDecisionOptions& options,
    const std::string& actor) {
    BatchDecisionResult result;
    const auto input_database = node.find_database(options.database);
    const auto input_table = input_database ? input_database->find_table(options.table) : nullptr;
    if (!input_table) {
        result.error = "scored table not found: " + options.database + "." + options.table;
        return result;
    }
    auto output_database = node.find_database(options.output_database);
    if (!output_database) {
        output_database = node.create_database(options.output_database);
    }
    if (!output_database) {
        result.error = "could not create decision output database";
        return result;
    }
    const std::string output_name = options.output_table.empty()
                                        ? "decisions_" + zetta::util::new_uuid().substr(0, 8)
                                        : options.output_table;
    const auto output_table = output_database->create_table(output_name);
    if (!output_table) {
        result.error = "decision output table already exists: " +
                       options.output_database + "." + output_name;
        return result;
    }
    for (std::size_t index = 0; index < input_table->store().size(); ++index) {
        const auto row = input_table->store().at(index);
        if (!row || !row->contains(options.entity_column) || !row->contains(options.score_column)) {
            result.error = "scored row is missing entity or score column";
            return result;
        }
        Json entity = *row;
        entity["entity_id"] = row->at(options.entity_column);
        entity["score"] = row->at(options.score_column);
        if (row->contains(options.cost_column)) {
            entity["cost"] = row->at(options.cost_column);
        }
        if (row->contains(options.contacts_column)) {
            entity["contacts"] = row->at(options.contacts_column);
        }
        if (row->contains(options.recent_actions_column)) {
            entity["recent_actions"] = row->at(options.recent_actions_column);
        }
        if (row->contains(options.holdout_column)) {
            entity["holdout"] = row->at(options.holdout_column);
        }
        std::string error;
        const auto decision = store.decide(policy_id, std::move(entity), actor, &error);
        if (!decision) {
            result.error = error;
            return result;
        }
        output_table->add_row(to_json(*decision));
        ++result.rows;
        if (decision->actionable) {
            ++result.actionable;
        } else if (decision->status == "pending_approval") {
            ++result.pending_approval;
        } else {
            ++result.blocked;
        }
    }
    result.table = options.output_database + "." + output_name;
    return result;
}

}  // namespace zetta::solutions
