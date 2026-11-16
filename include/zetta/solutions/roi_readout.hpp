// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/validation.hpp"

namespace zetta {
class Node;
}

namespace zetta::cluster {
class ReplicationManager;
}

namespace zetta::solutions {

// Customer-supplied economics for one measured outcome. The generator never
// supplies defaults for these numbers: they arrive through solution
// configuration and are stored in every immutable readout-definition version.
struct RoiModel {
    std::string outcome;
    double value_per_outcome = 0.0;
    double investment = 0.0;
    std::string currency;
};

struct RoiReadoutSpec {
    std::string solution;
    std::string project;
    std::string graph;
    std::string experiment;
    RoiModel roi_model;
    std::string measurement_design = "holdout";
    int outcome_window_days = 30;
    std::string outcome_unit;
};

// One immutable configuration version stored as `roi_readout:<uuid>` in the
// replicated Node catalog. New customer economics or source references create
// a new version rather than rewriting the evidence behind an older readout.
struct RoiReadoutDefinition {
    std::string id;
    std::uint64_t version = 0;
    RoiReadoutSpec spec;
    std::string created_by;
    std::string created;
};

struct EffectEstimate {
    std::uint64_t samples = 0;
    double effect = 0.0;
    double confidence_interval_lower = 0.0;
    double confidence_interval_upper = 0.0;
};

struct CellIncrementality {
    std::string cell;
    bool claimable = false;
    std::string reason;
    std::optional<EffectEstimate> estimate;
};

struct IncrementalityReadout {
    bool claimable = false;
    std::string reason;
    std::string outcome;
    double confidence_level = 0.95;
    std::uint64_t holdout_samples = 0;
    std::vector<CellIncrementality> cells;
    std::optional<EffectEstimate> combined;
};

struct RoiStatement {
    bool claimable = false;
    std::string reason;
    RoiModel model;
    std::optional<double> incremental_outcomes;
    std::optional<double> gross_value;
    std::optional<double> net_value;
    std::optional<double> return_on_investment;
};

struct AgentOperationsReport {
    std::uint64_t actions = 0;
    std::uint64_t agent_actions = 0;
    std::uint64_t human_actions = 0;
    std::uint64_t autonomy_a0 = 0;
    std::uint64_t autonomy_a1 = 0;
    std::uint64_t autonomy_a2 = 0;
    std::uint64_t autonomy_a3 = 0;
    std::uint64_t autonomy_unclassified = 0;
    std::uint64_t approvals = 0;
    std::uint64_t overrides = 0;
};

struct RoiReadout {
    RoiReadoutDefinition definition;
    std::string generated_at;
    zetta::pipeline::ValidationReport correctness_report;
    IncrementalityReadout incrementality;
    RoiStatement roi;
    nlohmann::json guardrails = nlohmann::json::object();
    AgentOperationsReport agent_operations;
    nlohmann::json audit_extract = nlohmann::json::array();
};

nlohmann::json to_json(const RoiModel& model);
nlohmann::json to_json(const RoiReadoutDefinition& definition);
nlohmann::json to_json(const RoiReadout& readout);

const nlohmann::json& roi_readout_definition_schema();
const nlohmann::json& roi_readout_schema();
std::optional<RoiReadoutDefinition> parse_roi_readout_definition(
    const nlohmann::json& value,
    std::string* error = nullptr);

// Owner-thread store for the versioned configuration behind the read-only
// capability. The Node catalog is the source of truth; there is no second
// local copy to reconcile.
class RoiReadoutStore {
public:
    explicit RoiReadoutStore(
        zetta::cluster::ReplicationManager& replication);

    std::optional<RoiReadoutDefinition> configure(
        RoiReadoutSpec spec,
        std::string actor,
        std::string* error = nullptr);
    std::optional<RoiReadoutDefinition> get(
        std::string_view solution_or_id,
        std::string* error = nullptr) const;
    std::vector<RoiReadoutDefinition> versions(
        std::string_view solution) const;

private:
    zetta::cluster::ReplicationManager& replication_;
};

// Assemble a customer-visible readout from live canonical sources. The call is
// read-only: it does not append ledger data, alter assignments, or persist the
// generated view.
std::optional<RoiReadout> generate_roi_readout(
    zetta::Node& node,
    zetta::cluster::ReplicationManager& replication,
    std::string_view solution_or_id,
    std::string* error = nullptr);

}  // namespace zetta::solutions
