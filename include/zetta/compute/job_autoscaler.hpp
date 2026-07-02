#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/compute/scaling_policy.hpp"

namespace zetta {

class ComputeJob;  // include/zetta/compute/compute_job.hpp — the running job we steer.

// The **autoscaler control loop** (ADR-29 / ARC-11, #371) — the piece that
// finally *acts on* the reward objective. The [ScalingPolicy](scaling-policy.md)
// slice computes a `ScalingDecision`; this loop closes it: each tick it observes a
// running [ComputeJob](compute-jobs.md)'s live signals, scores them with the
// policy, and **adds/removes nodes through the [Provisioner](provisioner.md)
// seam** — within the policy's bounds, with hysteresis so it does not thrash.
//
// **Tick-driven, no owned thread.** Like the deployment controller's
// `reconcile()`, the loop is one `tick()` call the caller drives from its
// heartbeat (the job coordinator, the headless/headed loop). That keeps it
// deterministic and unit-testable: a test ticks it by hand and asserts the fleet.
//
// **Seam-faithful actions.** To grow it calls `provision(count = target)` (the
// "ensure `target` nodes total" semantic every backend implements — set the pool
// size); to shrink it calls `teardown(node_id)` on the surplus nodes. It reads the
// current allocation from `provisioner.provisioned()` **every tick**, so it
// reflects live, **elastic membership** for free — a node that left the cluster
// stops appearing and the loop re-provisions toward the target on its own.
//
// **Given hardware degrades gracefully.** With the given-hardware
// `LocalProvisioner`, `provision` is bounded by the existing cluster and
// `teardown` is a no-op — so a scale-up is best-effort and a scale-down keeps the
// nodes (max utilization of fixed hardware). The loop surfaces that in the tick
// `detail` rather than failing.
//
// This slice introduces no new replicated state: the policy is passed in by value
// (a `ComputeJob` still references it by id only; persisting policies as their own
// entity is a later slice), and the only JSON boundary is the AutoscalerConfig DTO
// below. See docs/compute/job-autoscaler.md.

// One observation of the running job — the live signals the policy is scored
// against. The current node count is deliberately NOT here: the loop reads that
// itself from the Provisioner's live fleet (so it always reflects membership),
// then fills it into the ScalingSample. Supplied by an ObservationSource callable
// so how progress/cost is actually measured (the job coordinator, a meter) stays
// outside the loop — the same injection style as LocalProvisioner's FleetSource.
struct JobObservation {
    double progress = 0.0;         // fraction of the job complete, [0, 1]
    double elapsed_seconds = 0.0;  // wall-clock since the job started
    double cost = 0.0;             // accumulated cost so far (same units as cost_cap)

    bool operator==(const JobObservation&) const = default;
};

// How the control loop is tuned. A config DTO with a **validated JSON boundary**
// like the other compute-pass value types (NodeCapability / BurstCap): so a future
// slice can persist / replicate / GUI-edit it, and a malformed config fails fast.
struct AutoscalerConfig {
    // The per-node capability floor each provisioned node must meet (passed in the
    // ProvisionRequest on a scale-up). A default (no-floor) NodeCapability is
    // satisfied by every node.
    NodeCapability node_spec;

    // Hysteresis: how many **consecutive** ticks must recommend the **same**
    // direction before the loop acts on it. The anti-thrash guard — a one-off
    // recommendation (or an alternating up/down) never moves the fleet. Must be
    // >= 1; 1 disables the multi-tick confirmation (act on the first recommendation
    // each tick, still never mid-tick). Defaults to 2.
    std::uint32_t confirm_ticks = 2;

    // When the policy reports the cost cap breached (`within_cost_cap == false`),
    // shed **immediately** — bypass `confirm_ticks`. The cost cap is a hard safety
    // constraint, so we do not wait out the hysteresis window before cutting spend.
    // Defaults to true.
    bool shed_on_cost_breach = true;

    bool operator==(const AutoscalerConfig&) const = default;

    // The JSON value shape:
    //   { "node_spec": <NodeCapability>, "confirm_ticks": <u32 >= 1>,
    //     "shed_on_cost_breach": <bool> }
    // Mirrors schemas/autoscaler_config.schema.json — keep them in sync.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a malformed
    // config fails fast instead of silently changing how the loop behaves. Returns
    // std::nullopt on anything malformed: not an object, an invalid `node_spec`, a
    // `confirm_ticks` that is not an integer >= 1, or a non-boolean
    // `shed_on_cost_breach`. Omitted fields take their defaults.
    static std::optional<AutoscalerConfig> from_json(const nlohmann::json& value);
};

// What one control-loop tick did — everything an observer (a test, the GUI, a log)
// needs to see the decision and the action it produced.
struct AutoscaleTick {
    bool job_running = false;  // was the job Running this tick? (else the loop holds)
    ScalingSample sample;      // the observation + the read node count, as scored
    ScalingDecision decision;  // policy.evaluate(sample) — the raw recommendation

    // The direction actually taken after hysteresis: Hold when the loop held
    // (awaiting confirmation, or the policy said hold), else the confirmed move.
    ScalingAction confirmed_action = ScalingAction::Hold;
    bool acted = false;             // did the fleet size actually change this tick?
    std::uint32_t nodes_before = 0;  // allocation read at the start of the tick
    std::uint32_t nodes_after = 0;   // allocation after any provision/teardown
    std::string detail;              // human-readable explanation of the outcome
};

// The per-job autoscaler control loop. Construct one over a running ComputeJob, its
// resolved ScalingPolicy, a Provisioner, and an observation source; drive it by
// calling tick() each heartbeat. Holds no thread and no replicated state — it is a
// pure steering loop over the seams it is given. The ComputeJob and Provisioner
// must outlive the autoscaler.
class JobAutoscaler {
public:
    // The source of a live observation, consulted on each tick (so the loop always
    // scores the latest progress/cost). A null source observes a zeroed sample.
    using ObservationSource = std::function<JobObservation()>;

    JobAutoscaler(const ComputeJob& job, ScalingPolicy policy,
                  Provisioner& provisioner, ObservationSource observe,
                  AutoscalerConfig config = {});

    // Run one control-loop tick: read the allocation, observe, score the policy,
    // apply hysteresis, and — if a move is confirmed — grow (provision) or shrink
    // (teardown) the fleet toward the recommendation. A no-op (a Hold tick) when
    // the job is not Running. Returns what it did.
    AutoscaleTick tick();

    // The current fleet size (provisioner.provisioned().size()), reflecting live
    // membership.
    std::uint32_t nodes() const;

    const ScalingPolicy& policy() const { return policy_; }
    const AutoscalerConfig& config() const { return config_; }

    // Cumulative observability across all ticks.
    std::uint64_t total_ticks() const { return total_ticks_; }
    std::uint64_t total_scale_ups() const { return total_scale_ups_; }
    std::uint64_t total_scale_downs() const { return total_scale_downs_; }

private:
    const ComputeJob& job_;
    ScalingPolicy policy_;
    Provisioner& provisioner_;
    ObservationSource observe_;
    AutoscalerConfig config_;

    // Hysteresis state: the direction currently being confirmed and how many
    // consecutive ticks have recommended it.
    ScalingAction streak_action_ = ScalingAction::Hold;
    std::uint32_t streak_ = 0;

    std::uint64_t total_ticks_ = 0;
    std::uint64_t total_scale_ups_ = 0;
    std::uint64_t total_scale_downs_ = 0;
};

}  // namespace zetta
