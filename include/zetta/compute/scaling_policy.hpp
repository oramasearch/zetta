// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::compute {

// The job objective / reward model (ENT-34, ADR-29; part of the native
// distributed accelerated compute pass for UC-1 churn — ARC-11/#264). A
// `ScalingPolicy` is the per-job **reward objective the autoscaler optimizes**:
// run as cheaply as possible, finish by a runtime/latency target, or size the
// fleet to the shape of the data — all inside hard **bounds** (a min/max node
// envelope and a cost cap).
//
// This slice is a **pure value type + boundary validation** plus the objective
// evaluation, modeled exactly like its sibling compute-pass value types
// (zetta::cloud::NodeCapability / zetta::cloud::ProvisionedNode in provisioner.hpp): JSON
// parse/serialize with a from_json that rejects malformed input at the boundary.
// A ComputeJob carries a ScalingPolicy by id (`scaling_policy`); persisting /
// replicating policies as their own entity and wiring `evaluate()` into the live
// autoscaler / job coordinator are separate, later slices. See
// docs/compute/scaling-policy.md.

// What an autoscaler optimizes for a job. A closed enum; the lowercase name is
// BOTH the display label AND the stored JSON representation, so it is a durable
// on-disk identifier — append new objectives, never reorder.
enum class ScalingObjective {
    MinimizeCost,   // "minimize_cost" — run as cheaply as possible (favor min_nodes)
    RuntimeTarget,  // "runtime_target" — finish by `target_seconds` (runtime/latency)
    DataShape,      // "data_shape" — size the fleet to the work/data shape
};

// Canonical lowercase name of an objective ("minimize_cost" / "runtime_target" /
// "data_shape"). Both the display label and the stored representation.
std::string_view to_string(ScalingObjective objective);

// Parse a stored/canonical objective name back to the enum; false (out untouched)
// on an unknown spelling — the boundary check for the enum.
bool scaling_objective_from_string(std::string_view text, ScalingObjective& out);

// The hard envelope the autoscaler must stay inside, whatever the objective wants.
// A job needs at least one node, so `min_nodes` is >= 1 and `max_nodes` >=
// `min_nodes`. `cost_cap` is a spend ceiling in abstract cost units (e.g.
// node-seconds or dollars); 0 means **unlimited**. The cost cap is enforced as a
// hard constraint in evaluate(): breaching it sheds nodes back to the floor no
// matter the objective.
struct ScalingBounds {
    std::uint32_t min_nodes = 1;  // floor (>= 1)
    std::uint32_t max_nodes = 1;  // ceiling (>= min_nodes)
    double cost_cap = 0.0;        // spend ceiling; 0 = unlimited

    bool operator==(const ScalingBounds&) const = default;
};

// A live sample of a running job the autoscaler scores the policy against. The
// three signals the acceptance criterion names — progress, elapsed, cost — plus
// the current node allocation (so a decision can recommend a scale up/down/hold).
struct ScalingSample {
    double progress = 0.0;          // fraction of the job complete, [0, 1]
    double elapsed_seconds = 0.0;   // wall-clock since the job started
    double cost = 0.0;              // accumulated cost so far (same units as cost_cap)
    std::uint32_t nodes = 0;        // nodes currently allocated to the job

    bool operator==(const ScalingSample&) const = default;
};

// What the autoscaler should do next, relative to the sample's current `nodes`.
enum class ScalingAction { ScaleDown, Hold, ScaleUp };

// Canonical lowercase name of an action ("scale_down" / "hold" / "scale_up").
std::string_view to_string(ScalingAction action);

// The outcome of scoring a policy against a sample. `reward` is the scalar
// objective value (HIGHER is better — the autoscaler maximizes it);
// `recommended_nodes` is the next node count (always within the bounds, and
// forced down to `min_nodes` when the cost cap is breached); `action` is that
// recommendation expressed relative to `sample.nodes`; `within_cost_cap` is false
// once cost exceeds a set cap; `reason` is a human-readable explanation.
struct ScalingDecision {
    ScalingAction action = ScalingAction::Hold;
    std::uint32_t recommended_nodes = 0;
    double reward = 0.0;
    bool within_cost_cap = true;
    std::string reason;

    bool operator==(const ScalingDecision&) const = default;
};

// A per-job reward objective + bounds (ENT-34 / ADR-29). The pure value type the
// autoscaler reads; see the file header for scope.
struct ScalingPolicy {
    ScalingObjective objective = ScalingObjective::MinimizeCost;
    ScalingBounds bounds;
    double target_seconds = 0.0;  // RuntimeTarget runtime/latency target; 0 = unset

    // Clamp a node count into the bounds' [min_nodes, max_nodes] envelope.
    std::uint32_t clamp_nodes(std::uint32_t nodes) const;

    // Score this policy against a live sample → the autoscaler's next move. Pure
    // and deterministic (same policy + sample → same decision). The cost cap is a
    // hard constraint applied first: breaching it sheds to `min_nodes` regardless
    // of objective. Otherwise each objective has its own reward + recommendation:
    //   - MinimizeCost : hold at `min_nodes`; reward = -cost (cheaper is better).
    //   - RuntimeTarget: project total runtime (elapsed / progress), scale the
    //                    fleet toward hitting `target_seconds` (linear-speedup
    //                    heuristic); reward = -|projected - target| (on time = 0).
    //   - DataShape    : size the fleet to the remaining work (1 - progress) across
    //                    the node envelope; reward = -|nodes - desired| (matched = 0).
    ScalingDecision evaluate(const ScalingSample& sample) const;

    // The JSON value shape:
    //   { "objective": "<objective>",
    //     "bounds": { "min_nodes": <u32>, "max_nodes": <u32>, "cost_cap": <num>? },
    //     "target_seconds": <num>? }
    // `cost_cap` is omitted when 0 (unlimited); `target_seconds` is omitted when 0
    // (unset). Mirrors schemas/scaling_policy.schema.json — keep them in sync.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a malformed
    // policy fails fast instead of silently widening what the autoscaler will do.
    // Returns std::nullopt on anything malformed: not an object, a missing/
    // wrong-typed field, an unknown objective, min_nodes < 1, max_nodes <
    // min_nodes, a negative cost_cap/target_seconds, or a RuntimeTarget objective
    // with no positive target_seconds (a runtime target with no target is
    // meaningless).
    static std::optional<ScalingPolicy> from_json(const nlohmann::json& value);

    bool operator==(const ScalingPolicy&) const = default;
};

// The ScalingPolicy JSON Schema (draft 2020-12) — the code-owned source of
// truth; schemas/scaling_policy.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& scaling_policy_schema();

}  // namespace zetta::compute