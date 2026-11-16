// SPDX-License-Identifier: MIT

#include "zetta/compute/scaling_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

namespace zetta::compute {

std::string_view to_string(ScalingObjective objective) {
    switch (objective) {
        case ScalingObjective::MinimizeCost:  return "minimize_cost";
        case ScalingObjective::RuntimeTarget: return "runtime_target";
        case ScalingObjective::DataShape:     return "data_shape";
    }
    return "minimize_cost";
}

bool scaling_objective_from_string(std::string_view text, ScalingObjective& out) {
    constexpr std::array<ScalingObjective, 3> objectives{
        ScalingObjective::MinimizeCost, ScalingObjective::RuntimeTarget,
        ScalingObjective::DataShape};
    for (const ScalingObjective objective : objectives) {
        if (to_string(objective) == text) {
            out = objective;
            return true;
        }
    }
    return false;
}

std::string_view to_string(ScalingAction action) {
    switch (action) {
        case ScalingAction::ScaleDown: return "scale_down";
        case ScalingAction::Hold:      return "hold";
        case ScalingAction::ScaleUp:   return "scale_up";
    }
    return "hold";
}

namespace {

// Accept a JSON integer that is non-negative, whether nlohmann stored it signed (a
// plain `4` literal) or unsigned (what to_json writes). Returns false if it is not
// an integer or is negative — the boundary check for a node-count field. (Same
// shape as the helper in node_capability.cpp.)
bool as_nonneg_u64(const nlohmann::json& value, std::uint64_t& out) {
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {  // signed-but-non-negative
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value < 0) return false;
        out = static_cast<std::uint64_t>(signed_value);
        return true;
    }
    return false;
}

// Compact, noise-free rendering of a double for reason strings ("300" not
// "300.000000", "1.5" not "1.500000"). Default ostream precision is 6 significant
// digits, which is plenty for an explanation.
std::string num(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

// Express a recommendation relative to the current allocation.
ScalingAction action_for(std::uint32_t current, std::uint32_t recommended) {
    if (recommended > current) return ScalingAction::ScaleUp;
    if (recommended < current) return ScalingAction::ScaleDown;
    return ScalingAction::Hold;
}

}  // namespace

std::uint32_t ScalingPolicy::clamp_nodes(std::uint32_t nodes) const {
    if (nodes < bounds.min_nodes) return bounds.min_nodes;
    if (nodes > bounds.max_nodes) return bounds.max_nodes;
    return nodes;
}

ScalingDecision ScalingPolicy::evaluate(const ScalingSample& sample) const {
    ScalingDecision decision;
    decision.within_cost_cap =
        bounds.cost_cap <= 0.0 || sample.cost <= bounds.cost_cap;

    // Hard constraint first: a breached cost cap overrides the objective — shed
    // nodes back to the floor to stop spending, whatever the objective wants.
    if (!decision.within_cost_cap) {
        decision.recommended_nodes = bounds.min_nodes;
        decision.reward = -sample.cost;  // overspend penalizes every objective
        decision.reason = "cost cap exceeded (" + num(sample.cost) + " > " +
                          num(bounds.cost_cap) + "): shedding to min_nodes";
        decision.action = action_for(sample.nodes, decision.recommended_nodes);
        return decision;
    }

    switch (objective) {
        case ScalingObjective::MinimizeCost: {
            // Cheapest viable: hold at the floor; reward falls as cost grows.
            decision.recommended_nodes = bounds.min_nodes;
            decision.reward = -sample.cost;
            decision.reason = "minimize cost: hold at min_nodes";
            break;
        }
        case ScalingObjective::RuntimeTarget: {
            if (sample.progress <= 0.0) {
                // No throughput signal yet — hold where we are, no reward.
                const std::uint32_t base =
                    sample.nodes == 0 ? bounds.min_nodes : sample.nodes;
                decision.recommended_nodes = clamp_nodes(base);
                decision.reward = 0.0;
                decision.reason = "runtime target: awaiting first progress sample";
                break;
            }
            // Project total runtime assuming the current rate holds, then scale
            // the fleet linearly toward hitting the target (a linear-speedup
            // heuristic: behind → add nodes, ahead → release them).
            const double projected = sample.elapsed_seconds / sample.progress;
            const double base = sample.nodes == 0
                                    ? static_cast<double>(bounds.min_nodes)
                                    : static_cast<double>(sample.nodes);
            const double desired = target_seconds > 0.0
                                       ? std::ceil(base * projected / target_seconds)
                                       : base;
            decision.recommended_nodes =
                clamp_nodes(static_cast<std::uint32_t>(std::max(1.0, desired)));
            decision.reward = -std::abs(projected - target_seconds);
            decision.reason = "runtime target: projected " + num(projected) +
                              "s vs target " + num(target_seconds) + "s";
            break;
        }
        case ScalingObjective::DataShape: {
            // Size the fleet to the remaining work (1 - progress) across the node
            // envelope: lots left → toward max_nodes, nearly done → toward min.
            const double remaining = std::clamp(1.0 - sample.progress, 0.0, 1.0);
            const double span =
                static_cast<double>(bounds.max_nodes - bounds.min_nodes);
            const double desired =
                static_cast<double>(bounds.min_nodes) + std::ceil(span * remaining);
            decision.recommended_nodes =
                clamp_nodes(static_cast<std::uint32_t>(desired));
            decision.reward =
                -std::abs(static_cast<double>(sample.nodes) - desired);
            decision.reason = "data shape: " + num(remaining * 100.0) +
                              "% work remaining";
            break;
        }
    }

    decision.action = action_for(sample.nodes, decision.recommended_nodes);
    return decision;
}

nlohmann::json ScalingPolicy::to_json() const {
    nlohmann::json bounds_json{{"min_nodes", bounds.min_nodes},
                               {"max_nodes", bounds.max_nodes}};
    if (bounds.cost_cap > 0.0) bounds_json["cost_cap"] = bounds.cost_cap;

    nlohmann::json document{{"objective", to_string(objective)},
                            {"bounds", bounds_json}};
    if (target_seconds > 0.0) document["target_seconds"] = target_seconds;
    return document;
}

std::optional<ScalingPolicy> ScalingPolicy::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    if (!value.contains("objective") || !value["objective"].is_string()) {
        return std::nullopt;
    }
    ScalingObjective objective{};
    if (!scaling_objective_from_string(value["objective"].get<std::string>(),
                                       objective)) {
        return std::nullopt;
    }

    if (!value.contains("bounds") || !value["bounds"].is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& bounds_json = value["bounds"];

    std::uint64_t min_nodes = 0;
    if (!bounds_json.contains("min_nodes") ||
        !as_nonneg_u64(bounds_json["min_nodes"], min_nodes)) {
        return std::nullopt;
    }
    std::uint64_t max_nodes = 0;
    if (!bounds_json.contains("max_nodes") ||
        !as_nonneg_u64(bounds_json["max_nodes"], max_nodes)) {
        return std::nullopt;
    }
    // A job needs at least one node, the envelope must be non-empty, and the
    // ceiling must fit the field width.
    if (min_nodes < 1 || max_nodes < min_nodes ||
        max_nodes > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }

    double cost_cap = 0.0;
    if (bounds_json.contains("cost_cap")) {
        if (!bounds_json["cost_cap"].is_number()) return std::nullopt;
        cost_cap = bounds_json["cost_cap"].get<double>();
        if (cost_cap < 0.0) return std::nullopt;
    }

    double target_seconds = 0.0;
    if (value.contains("target_seconds")) {
        if (!value["target_seconds"].is_number()) return std::nullopt;
        target_seconds = value["target_seconds"].get<double>();
        if (target_seconds < 0.0) return std::nullopt;
    }
    // A runtime-target policy without a positive target is meaningless.
    if (objective == ScalingObjective::RuntimeTarget && target_seconds <= 0.0) {
        return std::nullopt;
    }

    ScalingPolicy policy;
    policy.objective = objective;
    policy.bounds = ScalingBounds{static_cast<std::uint32_t>(min_nodes),
                                  static_cast<std::uint32_t>(max_nodes), cost_cap};
    policy.target_seconds = target_seconds;
    return policy;
}

// The ScalingPolicy JSON Schema (draft 2020-12) — the code-owned source of
// truth; schemas/scaling_policy.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& scaling_policy_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/scaling_policy.schema.json",
  "title": "ScalingPolicy",
  "description": "The job objective / reward model (ENT-34, ADR-29; part of the native distributed accelerated compute pass for UC-1 churn — ARC-11 / #264). A per-job reward objective an autoscaler optimizes — minimize cost, hit a runtime/latency target, or size the fleet to the data shape — inside hard bounds (a min/max node envelope and a cost cap). A pure value type carried on a ComputeJob by id (scaling_policy in compute_job.schema.json). Mirrored by zetta::ScalingPolicy in include/zetta/scaling_policy.hpp and enforced by ScalingPolicy::from_json() in src/core/scaling_policy.cpp — keep them in sync. A malformed policy is rejected at the boundary so the autoscaler never silently widens what it will do.",
  "type": "object",
  "required": ["objective", "bounds"],
  "additionalProperties": false,
  "properties": {
    "objective": {
      "description": "What the autoscaler optimizes. Append-only enum (the lowercase name is the stored identifier).",
      "type": "string",
      "enum": ["minimize_cost", "runtime_target", "data_shape"]
    },
    "bounds": {
      "description": "The hard envelope the autoscaler stays inside, whatever the objective wants.",
      "$ref": "#/$defs/bounds"
    },
    "target_seconds": {
      "description": "The runtime/latency target in seconds for the 'runtime_target' objective — the wall-clock the autoscaler sizes the fleet to hit. Required (> 0) when objective is 'runtime_target'; omitted/0 (unset) otherwise.",
      "type": "number",
      "minimum": 0
    }
  },
  "$defs": {
    "bounds": {
      "description": "The min/max node envelope and the cost cap. A job needs at least one node, so min_nodes >= 1 and max_nodes >= min_nodes. cost_cap is a spend ceiling in abstract cost units (e.g. node-seconds or dollars); 0 (or omitted) means unlimited.",
      "type": "object",
      "required": ["min_nodes", "max_nodes"],
      "additionalProperties": false,
      "properties": {
        "min_nodes": {
          "description": "Floor on the node count (>= 1).",
          "type": "integer",
          "minimum": 1
        },
        "max_nodes": {
          "description": "Ceiling on the node count (>= min_nodes).",
          "type": "integer",
          "minimum": 1
        },
        "cost_cap": {
          "description": "Spend ceiling in abstract cost units. 0 (or omitted) means unlimited. Breaching it sheds the fleet back to min_nodes regardless of objective.",
          "type": "number",
          "minimum": 0
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::compute