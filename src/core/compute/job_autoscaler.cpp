#include "zetta/compute/job_autoscaler.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "zetta/compute/compute_job.hpp"

namespace zetta {

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
        std::optional<NodeCapability> spec =
            NodeCapability::from_json(value["node_spec"]);
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

JobAutoscaler::JobAutoscaler(const ComputeJob& job, ScalingPolicy policy,
                             Provisioner& provisioner, ObservationSource observe,
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
        const ProvisionResult provisioned =
            provisioner_.provision(ProvisionRequest{target, config_.node_spec});
        result.detail = provisioned.detail;
    } else if (target < before) {
        // Shrink: release the surplus, tearing down from the tail of the fleet
        // (LIFO — most-recently-listed first). teardown is idempotent and may be a
        // no-op for given hardware, which is reported, not failed.
        const std::vector<ProvisionedNode> fleet = provisioner_.provisioned();
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

}  // namespace zetta
