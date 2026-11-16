// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::cluster {
class ReplicationManager;
}

namespace zetta::solutions {

// The mutually exclusive allocation tiers supported by the shared holdout /
// experiment manager (#1105). Holdout and below-line assignments are protected
// from model-driven actions by decide(); treatment and exploration assignments
// may receive the proposed action.
enum class ExperimentTier {
    Holdout,
    Treatment,
    Exploration,
    BelowLine,
};

std::string_view to_string(ExperimentTier tier);
std::optional<ExperimentTier> experiment_tier_from(std::string_view name);

struct ExperimentAllocation {
    std::string name;
    ExperimentTier tier = ExperimentTier::Treatment;
    double fraction = 0.0;
};

// Replicated experiment metadata stored under experiment:<uuid>. Fractions are
// absolute population shares and must total 1.0. Allocation names are unique.
struct ExperimentValue {
    std::string name;
    std::uint64_t seed = 0;
    std::vector<ExperimentAllocation> allocations;
    std::string created;
};

// One stable entity assignment stored independently under
// experiment_assignment:<experiment-uuid>:<entity-digest>. Independent keys
// let equal peers assign different entities concurrently without lost records.
// `propensity` is the configured probability of this allocation, so these rows
// can be joined to outcomes and consumed directly by native uplift training.
struct ExperimentAssignmentValue {
    std::string experiment;
    std::string entity;
    std::string allocation;
    ExperimentTier tier = ExperimentTier::Treatment;
    double propensity = 0.0;
    double bucket = 0.0;
    bool exempt = false;
    std::string exemption_reason;
    std::string exemption_actor;
    std::string assigned_at;
};

struct ExperimentRecord {
    std::string id;
    ExperimentValue value;
};

struct ExperimentAssignmentRecord {
    std::string id;
    ExperimentAssignmentValue value;
};

struct AllocationBalance {
    ExperimentAllocation allocation;
    std::uint64_t assignments = 0;
    double expected = 0.0;
    double deviation = 0.0;
    double tolerance = 0.0;
    bool balanced = true;
};

struct ExperimentReport {
    ExperimentRecord experiment;
    std::uint64_t assignments = 0;
    std::uint64_t exemptions = 0;
    double exemption_rate = 0.0;
    std::uint64_t holdout_assignments = 0;
    std::uint64_t holdout_exemptions = 0;
    double holdout_erosion_rate = 0.0;
    bool balanced = true;
    std::vector<AllocationBalance> allocations;
    std::vector<ExperimentAssignmentRecord> training_rows;
};

// The result returned immediately before an action integration executes. A
// suppressed decision has an empty `action`; callers must execute only the
// returned action, never the original proposal.
struct ExperimentDecision {
    ExperimentAssignmentRecord assignment;
    std::string proposed_action;
    std::string action;
    bool model_driven = true;
    bool allowed = false;
    std::string reason;
};

nlohmann::json to_json(const ExperimentValue& value);
nlohmann::json to_json(const ExperimentAssignmentValue& value);
nlohmann::json to_json(const ExperimentRecord& record);
nlohmann::json to_json(const ExperimentAssignmentRecord& record);
nlohmann::json to_json(const ExperimentReport& report);
nlohmann::json to_json(const ExperimentDecision& decision);

const nlohmann::json& experiment_value_schema();
const nlohmann::json& experiment_assignment_value_schema();

bool validate_experiment_value(const nlohmann::json& value,
                               std::string* error = nullptr);
bool validate_experiment_assignment_value(const nlohmann::json& value,
                                          std::string* error = nullptr);
std::optional<ExperimentValue> parse_experiment_value(
    const nlohmann::json& value, std::string* error = nullptr);
std::optional<ExperimentAssignmentValue> parse_experiment_assignment_value(
    const nlohmann::json& value, std::string* error = nullptr);

// Owner-thread component for experiment creation, deterministic assignment,
// action protection, replicated capture/reconciliation, and reporting. A Node
// owns one instance and exposes it through Node::experiment_manager().
class ExperimentManager {
public:
    ExperimentManager(
        std::filesystem::path data_dir,
        zetta::cluster::ReplicationManager* replication = nullptr);

    void load();
    void migrate_to_replication();
    // Null keys apply a full snapshot. Otherwise only the supplied keys and
    // children of a removed experiment are affected (#1086, #1128).
    void reconcile(const nlohmann::json& snapshot,
                   const std::vector<std::string>* changed_keys = nullptr);

    std::optional<ExperimentRecord> create(
        ExperimentValue value, std::string* error = nullptr);
    std::optional<ExperimentRecord> find(std::string_view reference) const;
    std::vector<ExperimentRecord> experiments() const;

    std::optional<ExperimentAssignmentRecord> assign(
        std::string_view experiment_reference,
        std::string entity,
        bool exempt = false,
        std::string exemption_reason = {},
        std::string exemption_actor = {},
        std::string* error = nullptr);

    std::optional<ExperimentDecision> decide(
        std::string_view experiment_reference,
        std::string entity,
        std::string proposed_action,
        bool model_driven = true,
        bool exempt = false,
        std::string exemption_reason = {},
        std::string exemption_actor = {},
        std::string* error = nullptr);

    std::optional<ExperimentReport> report(
        std::string_view experiment_reference,
        std::string* error = nullptr) const;

private:
    std::filesystem::path experiment_dir(std::string_view id) const;
    std::filesystem::path assignment_dir(std::string_view experiment_id) const;
    void persist_experiment(const ExperimentRecord& record) const;
    void persist_assignment(const ExperimentAssignmentRecord& record) const;
    void remove_assignment_file(const ExperimentAssignmentRecord& record) const;

    std::filesystem::path root_;
    zetta::cluster::ReplicationManager* replication_;
    std::map<std::string, ExperimentValue> experiments_;
    std::map<std::string, ExperimentAssignmentValue> assignments_;
};

}  // namespace zetta::solutions
