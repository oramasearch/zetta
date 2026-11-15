// SPDX-License-Identifier: MIT

#include "zetta/compute/job_autoscaler.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_job.hpp"

namespace zetta::compute {

namespace {

// Accept a JSON integer that is non-negative, whether nlohmann stored it signed (a
// plain `2` literal) or unsigned (what to_json writes). Returns false if it is not
// an integer or is negative — the boundary check for a count field. The same shape
// as the helpers in node_capability.cpp / burst_approval_gate.cpp (kept local so
// the config parser is self-contained).
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

}  // namespace

nlohmann::json AutoscalerConfig::to_json() const {
    return nlohmann::json{{"node_spec", node_spec.to_json()},
                          {"confirm_ticks", confirm_ticks},
                          {"shed_on_cost_breach", shed_on_cost_breach}};
}

std::optional<AutoscalerConfig> AutoscalerConfig::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    AutoscalerConfig config;

    if (value.contains("node_spec")) {
        std::optional<zetta::cloud::NodeCapability> spec =
            zetta::cloud::NodeCapability::from_json(value["node_spec"]);
        if (!spec) return std::nullopt;
        config.node_spec = *spec;
    }

    if (value.contains("confirm_ticks")) {
        std::uint64_t confirm_ticks = 0;
        if (!as_nonneg_u64(value["confirm_ticks"], confirm_ticks)) {
            return std::nullopt;
        }
        // A confirmation window of 0 ticks is meaningless (it would never confirm);
        // the loop needs at least one same-direction tick to act.
        if (confirm_ticks < 1 ||
            confirm_ticks > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        config.confirm_ticks = static_cast<std::uint32_t>(confirm_ticks);
    }

    if (value.contains("shed_on_cost_breach")) {
        if (!value["shed_on_cost_breach"].is_boolean()) return std::nullopt;
        config.shed_on_cost_breach = value["shed_on_cost_breach"].get<bool>();
    }

    return config;
}

// The AutoscalerConfig JSON Schema (draft 2020-12) — the code-owned source of
// truth; schemas/autoscaler_config.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& autoscaler_config_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/autoscaler_config.schema.json",
  "title": "AutoscalerConfig",
  "description": "How the autoscaler control loop (ADR-29 / ARC-11, #371) is tuned: the per-node capability floor for provisioned nodes, the hysteresis confirmation window, and whether a cost-cap breach sheds immediately. The loop observes a running ComputeJob's progress, scores it with the job's ScalingPolicy, and adds/removes nodes through the Provisioner seam within the policy bounds. Mirrored by zetta::AutoscalerConfig in include/zetta/job_autoscaler.hpp and enforced by AutoscalerConfig::from_json() in src/core/job_autoscaler.cpp — keep them in sync. Omitted fields take their defaults; a malformed config is rejected at the boundary so the loop never silently changes how it behaves.",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "node_spec": {
      "description": "The per-node capability floor each provisioned node must meet (passed in the ProvisionRequest on a scale-up). A default (no-floor) capability is satisfied by every node. See node_capability.schema.json.",
      "$ref": "#/$defs/node_capability"
    },
    "confirm_ticks": {
      "description": "Hysteresis: how many consecutive ticks must recommend the same direction before the loop acts. The anti-thrash guard. Must be >= 1 (1 disables the multi-tick confirmation). Defaults to 2.",
      "type": "integer",
      "minimum": 1
    },
    "shed_on_cost_breach": {
      "description": "When the policy reports the cost cap breached, shed immediately — bypass confirm_ticks. The cost cap is a hard safety constraint. Defaults to true.",
      "type": "boolean"
    }
  },
  "$defs": {
    "node_capability": {
      "description": "What a provisioned node must provide — CPU cores, memory, and an optional GPU (type + throughput). Mirrors node_capability.schema.json.",
      "type": "object",
      "required": ["cores", "memory_bytes"],
      "additionalProperties": false,
      "properties": {
        "cores": {
          "description": "Logical CPU cores the node provides.",
          "type": "integer",
          "minimum": 0
        },
        "memory_bytes": {
          "description": "Total memory in bytes.",
          "type": "integer",
          "minimum": 0
        },
        "gpu": {
          "description": "The node's accelerator. Optional; absent means CPU-only (equivalent to kind 'none').",
          "type": "object",
          "required": ["kind"],
          "additionalProperties": false,
          "properties": {
            "kind": {
              "description": "The accelerator family. Append-only enum (the lowercase name is the stored identifier).",
              "type": "string",
              "enum": ["none", "cuda", "rocm", "metal"]
            },
            "throughput_tflops": {
              "description": "Coarse throughput in teraFLOPS. 0 when there is no GPU.",
              "type": "number",
              "minimum": 0
            },
            "model": {
              "description": "Optional free-form accelerator model name (e.g. 'A100', 'H100', 'MI300X').",
              "type": "string"
            }
          }
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

JobAutoscaler::JobAutoscaler(const ComputeJob& job, ScalingPolicy policy,
                             zetta::cloud::Provisioner& provisioner, ObservationSource observe,
                             AutoscalerConfig config)
    : job_(job),
      policy_(std::move(policy)),
      provisioner_(provisioner),
      observe_(std::move(observe)),
      config_(std::move(config)) {}

std::uint32_t JobAutoscaler::nodes() const {
    const std::size_t count = provisioner_.provisioned().size();
    // The fleet cannot realistically exceed the field width; clamp defensively so
    // a pathological backend never wraps the node count.
    return static_cast<std::uint32_t>(
        std::min<std::size_t>(count, std::numeric_limits<std::uint32_t>::max()));
}

AutoscaleTick JobAutoscaler::tick() {
    ++total_ticks_;

    AutoscaleTick result;
    const std::uint32_t before = nodes();
    result.nodes_before = before;
    result.nodes_after = before;

    // Only steer a running job. A queued / terminal job is a no-op: hold the fleet
    // and reset the hysteresis streak so a later run starts clean.
    result.job_running = job_.status() == JobStatus::Running;
    if (!result.job_running) {
        streak_ = 0;
        streak_action_ = ScalingAction::Hold;
        result.detail = "job not running; holding";
        return result;
    }

    const JobObservation observation = observe_ ? observe_() : JobObservation{};
    result.sample = ScalingSample{observation.progress, observation.elapsed_seconds,
                                  observation.cost, before};
    result.decision = policy_.evaluate(result.sample);

    const ScalingAction want = result.decision.action;
    const bool cost_breach = !result.decision.within_cost_cap;

    // --- Hysteresis -----------------------------------------------------------
    // Decide whether the recommendation is confirmed enough to act on. A cost-cap
    // breach (a hard safety constraint) bypasses the confirmation window when
    // configured; otherwise a direction must be recommended on `confirm_ticks`
    // consecutive ticks before the loop moves the fleet.
    bool confirmed = false;
    if (cost_breach && config_.shed_on_cost_breach) {
        confirmed = want != ScalingAction::Hold;
        streak_ = 0;  // an immediate action resets the window
        streak_action_ = ScalingAction::Hold;
    } else if (want == ScalingAction::Hold) {
        streak_ = 0;  // the policy is content — clear any pending move
        streak_action_ = ScalingAction::Hold;
    } else {
        if (want == streak_action_) {
            ++streak_;
        } else {
            streak_action_ = want;
            streak_ = 1;
        }
        const std::uint32_t need = std::max<std::uint32_t>(1, config_.confirm_ticks);
        if (streak_ >= need) {
            confirmed = true;
            streak_ = 0;  // reset after acting so the next move re-confirms
            streak_action_ = ScalingAction::Hold;
        }
    }

    if (!confirmed) {
        result.confirmed_action = ScalingAction::Hold;
        if (want == ScalingAction::Hold) {
            result.detail = result.decision.reason;
        } else {
            const std::uint32_t need =
                std::max<std::uint32_t>(1, config_.confirm_ticks);
            result.detail = std::string(to_string(want)) + " pending (" +
                            std::to_string(streak_) + "/" + std::to_string(need) +
                            "): " + result.decision.reason;
        }
        return result;
    }

    // --- Act toward the recommendation ---------------------------------------
    result.confirmed_action = want;
    const std::uint32_t target = result.decision.recommended_nodes;

    if (target > before) {
        // Grow: ask the backend to ensure `target` nodes total (best-effort).
        const zetta::cloud::ProvisionResult provisioned =
            provisioner_.provision(zetta::cloud::ProvisionRequest{target, config_.node_spec});
        result.detail = provisioned.detail;
    } else if (target < before) {
        // Shrink: release the surplus, tearing down from the tail of the fleet
        // (LIFO — most-recently-listed first). teardown is idempotent and may be a
        // no-op for given hardware, which is reported, not failed.
        const std::vector<zetta::cloud::ProvisionedNode> fleet = provisioner_.provisioned();
        const std::uint32_t to_release = before - target;
        std::uint32_t released = 0;
        for (auto node = fleet.rbegin();
             node != fleet.rend() && released < to_release; ++node) {
            if (provisioner_.teardown(node->node_id)) ++released;
        }
        if (released == to_release) {
            result.detail = "released " + std::to_string(released) + " node(s)";
        } else {
            result.detail =
                "best-effort: released " + std::to_string(released) + " of " +
                std::to_string(to_release) +
                " node(s) (given hardware cannot release)";
        }
    } else {
        // recommended == current with a non-Hold action should not happen (the
        // policy derives the action from the recommendation), but guard anyway.
        result.confirmed_action = ScalingAction::Hold;
        result.detail = result.decision.reason;
    }

    // Re-read the live allocation: the source of truth for what actually changed
    // (and counts effective moves, so a best-effort no-op is not miscounted).
    result.nodes_after = nodes();
    result.acted = result.nodes_after != before;
    if (result.nodes_after > before) {
        ++total_scale_ups_;
    } else if (result.nodes_after < before) {
        ++total_scale_downs_;
    }

    return result;
}

}  // namespace zetta::compute