#include "zetta/usecases/churn/entity_resolution_check.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::usecases::churn {

namespace {

// The attribute key a graph node uses to carry its entity-resolution result.
constexpr const char* kAttributeKey = "entity_resolution";

// The entity-resolution payload schema (draft-07). One declarative source of
// truth for the `entity_resolution` attribute shape, mirrored by the public file
// schemas/entity_resolution.schema.json — keep the two in sync. `entity` and
// `resolved` are required non-empty strings, so a malformed payload is rejected
// at the boundary (in evaluate, below) rather than silently mis-evaluated.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.EntityResolution",
      "type": "object",
      "required": ["records"],
      "additionalProperties": false,
      "properties": {
        "records": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["entity", "resolved"],
            "additionalProperties": false,
            "properties": {
              "mention":  { "type": "string" },
              "entity":   { "type": "string", "minLength": 1 },
              "resolved": { "type": "string", "minLength": 1 }
            }
          }
        }
      }
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step);
// validate() is then cheap and runs at every check.
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. The error-handler overload of
// validate() keeps validation failures from crossing the validator-library
// boundary as exceptions (matching src/core/run.cpp) — robust regardless of how
// each side is compiled, and faithful to zetta's no-exceptions convention.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

bool validate_payload(const nlohmann::json& payload, std::string& error) {
    collecting_error_handler handler;
    schema_validator().validate(payload, handler);
    if (handler.failed) {
        error = handler.reason;
        return false;
    }
    return true;
}

// A human label for a step in a reason string: its name if it has one, else its id.
std::string step_label(const ir::Node& node) {
    return node.name.empty() ? node.id : node.name;
}

// Join an ordered set of strings with ", " for a readable, deterministic reason.
std::string join(const std::set<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ", ";
        out += item;
    }
    return out;
}

std::string join(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += separator;
        out += item;
    }
    return out;
}

// The outcome of evaluating one entity-resolution step.
struct StepOutcome {
    zetta::pipeline::CheckStatus status = zetta::pipeline::CheckStatus::Pass;
    std::string reason;
    std::size_t entity_count = 0;   // distinct ground-truth entities in this step
    std::size_t account_count = 0;  // distinct resolved accounts in this step
};

// Evaluate one node's entity_resolution payload: validate it, then compare its
// partition by `resolved` against its partition by `entity`.
StepOutcome evaluate_step(const ir::Node& node) {
    const nlohmann::json& payload = node.attributes.at(kAttributeKey);

    std::string schema_error;
    if (!validate_payload(payload, schema_error)) {
        return {zetta::pipeline::CheckStatus::Fail,
                "step '" + step_label(node) +
                    "' has a malformed entity_resolution payload: " + schema_error,
                0, 0};
    }

    // Ordered containers → deterministic reasons (stable for tests + display).
    std::map<std::string, std::set<std::string>> account_to_entities;
    std::map<std::string, std::set<std::string>> entity_to_accounts;
    for (const nlohmann::json& record : payload.at("records")) {
        const auto entity = record.at("entity").get<std::string>();
        const auto resolved = record.at("resolved").get<std::string>();
        account_to_entities[resolved].insert(entity);
        entity_to_accounts[entity].insert(resolved);
    }

    StepOutcome outcome;
    outcome.entity_count = entity_to_accounts.size();
    outcome.account_count = account_to_entities.size();

    // Conflation (over-merge): one account holding more than one distinct entity.
    std::vector<std::string> conflations;
    for (const auto& [account, entities] : account_to_entities) {
        if (entities.size() > 1) {
            conflations.push_back("account '" + account + "' conflates " +
                                  std::to_string(entities.size()) +
                                  " entities (" + join(entities) + ")");
        }
    }
    if (!conflations.empty()) {
        outcome.status = zetta::pipeline::CheckStatus::Fail;
        outcome.reason =
            "step '" + step_label(node) + "': " + join(conflations, "; ");
        return outcome;
    }

    // Fragmentation (under-merge): one entity split across several accounts.
    std::vector<std::string> fragments;
    for (const auto& [entity, accounts] : entity_to_accounts) {
        if (accounts.size() > 1) {
            fragments.push_back("entity '" + entity + "' is split across " +
                                std::to_string(accounts.size()) +
                                " accounts (" + join(accounts) + ")");
        }
    }
    if (!fragments.empty()) {
        outcome.status = zetta::pipeline::CheckStatus::Warn;
        outcome.reason =
            "step '" + step_label(node) + "': " + join(fragments, "; ");
        return outcome;
    }

    return outcome;  // Pass
}

}  // namespace

zetta::pipeline::CheckResult EntityResolutionCheck::evaluate(const ir::ExecutionGraph& graph) const {
    std::vector<const ir::Node*> steps;
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->attributes.is_object() &&
            node->attributes.contains(kAttributeKey)) {
            steps.push_back(node.get());
        }
    }
    if (steps.empty()) {
        return {name(), zetta::pipeline::CheckStatus::Pass, "no entity-resolution step to evaluate"};
    }

    zetta::pipeline::CheckStatus worst = zetta::pipeline::CheckStatus::Pass;
    std::vector<std::string> fail_reasons;
    std::vector<std::string> warn_reasons;
    std::size_t total_entities = 0;
    std::size_t total_accounts = 0;
    for (const ir::Node* node : steps) {
        const StepOutcome outcome = evaluate_step(*node);
        total_entities += outcome.entity_count;
        total_accounts += outcome.account_count;
        if (outcome.status == zetta::pipeline::CheckStatus::Fail) {
            fail_reasons.push_back(outcome.reason);
            worst = zetta::pipeline::CheckStatus::Fail;
        } else if (outcome.status == zetta::pipeline::CheckStatus::Warn) {
            warn_reasons.push_back(outcome.reason);
            if (worst == zetta::pipeline::CheckStatus::Pass) worst = zetta::pipeline::CheckStatus::Warn;
        }
    }

    if (worst == zetta::pipeline::CheckStatus::Fail) {
        return {name(), zetta::pipeline::CheckStatus::Fail,
                "entity resolution conflates separate accounts — " +
                    join(fail_reasons, "; ")};
    }
    if (worst == zetta::pipeline::CheckStatus::Warn) {
        return {name(), zetta::pipeline::CheckStatus::Warn,
                "entity resolution under-merges — " + join(warn_reasons, "; ")};
    }
    return {name(), zetta::pipeline::CheckStatus::Pass,
            std::to_string(total_entities) + " entities resolved across " +
                std::to_string(total_accounts) + " accounts in " +
                std::to_string(steps.size()) + " step(s); no conflation"};
}

}  // namespace zetta::usecases::churn