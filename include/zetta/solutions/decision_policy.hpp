#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {
class Node;
}

namespace zetta::cluster {
class ReplicationManager;
}

namespace zetta::governance {
class AuditSink;
}

namespace zetta::solutions {

// Versioned decision policy and durable decision records (#1104).
// See docs/solutions/decision-policy.md.
enum class AutonomyClass {
    A0,
    A1,
    A2,
    A3,
};

std::string_view to_string(AutonomyClass autonomy);
std::optional<AutonomyClass> autonomy_class_from(std::string_view value);

struct PolicyVersion {
    std::uint64_t version = 0;
    nlohmann::json config = nlohmann::json::object();
    std::string actor;
    std::string reason;
    std::string created;
};

struct DecisionPolicy {
    std::string id;
    std::string name;
    std::vector<PolicyVersion> versions;
    std::string created;
    std::string updated;

    const PolicyVersion* current() const;
};

struct DecisionRecord {
    std::string id;
    std::string policy_id;
    std::uint64_t policy_version = 0;
    std::string entity_id;
    double score = 0.0;
    double cost = 0.0;
    std::string tier;
    std::string action;
    std::string autonomy;
    std::string status;
    bool holdout = false;
    bool actionable = false;
    std::vector<std::string> reasons;
    nlohmann::json evidence = nlohmann::json::object();
    std::string created;
    std::string approved_at;
    std::string approved_by;
};

nlohmann::json to_json(const PolicyVersion& version);
nlohmann::json to_json(const DecisionPolicy& policy);
nlohmann::json to_json(const DecisionRecord& decision);

const nlohmann::json& decision_policy_config_schema();
const nlohmann::json& decision_policy_schema();
const nlohmann::json& decision_record_schema();

std::optional<DecisionPolicy> parse_decision_policy(
    const nlohmann::json& value,
    std::string* error = nullptr);
std::optional<DecisionRecord> parse_decision_record(
    const nlohmann::json& value,
    std::string* error = nullptr);

class DecisionPolicyStore {
public:
    DecisionPolicyStore(
        zetta::cluster::ReplicationManager& replication,
        zetta::governance::AuditSink* audit = nullptr);

    std::optional<DecisionPolicy> create(
        std::string name,
        nlohmann::json config,
        std::string actor,
        std::string* error = nullptr);
    std::optional<DecisionPolicy> get(const std::string& id) const;
    std::optional<DecisionPolicy> update(
        const std::string& id,
        std::optional<nlohmann::json> replacement,
        std::optional<nlohmann::json> tuning,
        std::string actor,
        std::string reason,
        std::string* error = nullptr);

    std::optional<DecisionRecord> decide(
        const std::string& policy_id,
        nlohmann::json entity,
        std::string actor,
        std::string* error = nullptr);
    std::optional<DecisionRecord> get_decision(
        const std::string& decision_id) const;
    std::optional<DecisionRecord> approve(
        const std::string& decision_id,
        std::string actor,
        std::string* error = nullptr);

    // Empty when a recorded decision may be delivered now. This re-checks the
    // latest policy kill-switch immediately before execution while preserving
    // the decision's original policy version for auditability.
    std::optional<std::string> delivery_blocker(
        const DecisionRecord& decision) const;

private:
    zetta::cluster::ReplicationManager& replication_;
    zetta::governance::AuditSink* audit_;
};

struct BatchDecisionOptions {
    std::string database;
    std::string table;
    std::string output_database = "decision_results";
    std::string output_table;
    std::string entity_column = "entity_id";
    std::string score_column = "score";
    std::string cost_column = "cost";
    std::string contacts_column = "contacts";
    std::string recent_actions_column = "recent_actions";
    std::string holdout_column = "holdout";
};

struct BatchDecisionResult {
    std::string table;
    std::size_t rows = 0;
    std::size_t actionable = 0;
    std::size_t pending_approval = 0;
    std::size_t blocked = 0;
    std::string error;

    bool ok() const {
        return error.empty();
    }
};

BatchDecisionResult decide_table(
    zetta::Node& node,
    DecisionPolicyStore& store,
    const std::string& policy_id,
    const BatchDecisionOptions& options,
    const std::string& actor);

}  // namespace zetta::solutions
