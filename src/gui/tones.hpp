// SPDX-License-Identifier: MIT

#pragma once

// The ONE place an engine status maps to a visual tone (#636). Panels never
// pick status colours ad hoc: they call `tone_of(status)` and render with
// `tone_color(...)` (app_internal.hpp), so the same state always looks the
// same everywhere. Adding a status-bearing enum = adding ONE overload here.
//
// Tone semantics (the palette itself lives in app_internal.hpp):
//   Ok     green  · succeeded / passed / healthy
//   Active green  · live / open / scaling up (the dot-active green)
//   Warn   amber  · in flight or needs attention (running, warn, shedding)
//   Error  red    · failed / errored
//   Muted  quiet  · pending / queued / skipped / neutral

#include <string_view>

#include "gui/app_internal.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/scaling_policy.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/pipeline/graph_plan.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/run_progress.hpp"
#include "zetta/pipeline/validation.hpp"
#include "zetta/testkit/test_suite.hpp"

namespace zetta::gui {

// A run along its lifecycle: pending (muted) → running (amber) →
// succeeded (green) | failed (red).
constexpr Tone tone_of(zetta::pipeline::RunStatus status) {
    switch (status) {
        case zetta::pipeline::RunStatus::Pending:   return Tone::Muted;
        case zetta::pipeline::RunStatus::Running:   return Tone::Warn;
        case zetta::pipeline::RunStatus::Succeeded: return Tone::Ok;
        case zetta::pipeline::RunStatus::Failed:    return Tone::Error;
    }
    return Tone::Muted;
}

// One graph step inside a run (#626, the workspace live-run overlay) — the
// run lifecycle, per step: pending muted, running amber, succeeded green,
// failed red. Deliberately identical to tone_of(RunStatus) so a step reads
// exactly like the run it belongs to.
constexpr Tone tone_of(zetta::pipeline::StepRunStatus status) {
    switch (status) {
        case zetta::pipeline::StepRunStatus::Pending:   return Tone::Muted;
        case zetta::pipeline::StepRunStatus::Running:   return Tone::Warn;
        case zetta::pipeline::StepRunStatus::Succeeded: return Tone::Ok;
        case zetta::pipeline::StepRunStatus::Failed:    return Tone::Error;
    }
    return Tone::Muted;
}

// A compute job: queued (muted) → running (amber) → succeeded | failed.
// Deliberately the same shape as runs — the two lifecycles read identically.
constexpr Tone tone_of(zetta::compute::JobStatus status) {
    switch (status) {
        case zetta::compute::JobStatus::Queued:    return Tone::Muted;
        case zetta::compute::JobStatus::Running:   return Tone::Warn;
        case zetta::compute::JobStatus::Succeeded: return Tone::Ok;
        case zetta::compute::JobStatus::Failed:    return Tone::Error;
    }
    return Tone::Muted;
}

// The autoscaler's intent: scale-up green (adding capacity), hold neutral,
// scale-down amber (shedding nodes).
constexpr Tone tone_of(zetta::compute::ScalingAction action) {
    switch (action) {
        case zetta::compute::ScalingAction::ScaleUp:   return Tone::Active;
        case zetta::compute::ScalingAction::Hold:      return Tone::Muted;
        case zetta::compute::ScalingAction::ScaleDown: return Tone::Warn;
    }
    return Tone::Muted;
}

// A correctness-gate check: pass green, warn amber, fail red; a check that did
// not apply is quiet — it verified nothing, so it never looks like a pass.
constexpr Tone tone_of(zetta::pipeline::CheckStatus status) {
    switch (status) {
        case zetta::pipeline::CheckStatus::NotApplicable: return Tone::Muted;
        case zetta::pipeline::CheckStatus::Pass: return Tone::Ok;
        case zetta::pipeline::CheckStatus::Warn: return Tone::Warn;
        case zetta::pipeline::CheckStatus::Fail: return Tone::Error;
    }
    return Tone::Error;
}

// An in-app test-suite outcome: pass green, fail red, error amber (the
// harness broke, not the assertion), skipped quiet.
constexpr Tone tone_of(zetta::testkit::TestOutcome outcome) {
    switch (outcome) {
        case zetta::testkit::TestOutcome::Pass:    return Tone::Ok;
        case zetta::testkit::TestOutcome::Fail:    return Tone::Error;
        case zetta::testkit::TestOutcome::Skipped: return Tone::Muted;
        case zetta::testkit::TestOutcome::Error:   return Tone::Warn;
    }
    return Tone::Muted;
}

// A cluster node's sync state (the "node sync" status the Cluster view's node
// boxes, its detail pane, and the graph workspace's placement canvas all
// share): self is the live local replica (active green), a peer with no
// learned status yet is neutral, in sync is green, behind is amber.
constexpr Tone tone_of(const zetta::cluster::NodeStatus& node) {
    if (node.is_self) return Tone::Active;
    if (!node.replication.known) return Tone::Muted;
    return node.replication.in_sync ? Tone::Ok : Tone::Warn;
}

// A step's projected cluster placement (#591): near-data green (compute runs
// where the data lives — the efficient case), coordinator amber (a single-node
// funnel point — attention, not distributed), fan-out and all-nodes neutral
// (distributed compute / replicated loads, nothing to flag).
constexpr Tone tone_of(zetta::pipeline::StepPlacement placement) {
    switch (placement) {
        case zetta::pipeline::StepPlacement::NearData:    return Tone::Active;
        case zetta::pipeline::StepPlacement::FanOut:      return Tone::Muted;
        case zetta::pipeline::StepPlacement::AllNodes:    return Tone::Muted;
        case zetta::pipeline::StepPlacement::Coordinator: return Tone::Warn;
    }
    return Tone::Muted;
}

// An activity-feed entry's outcome (#622): ok green; denied and failed both
// red (governance refusing vs the engine failing — the label carries the
// difference); info (created / queued, no outcome yet) quiet.
constexpr Tone tone_of(zetta::observability::ActivityOutcome outcome) {
    switch (outcome) {
        case zetta::observability::ActivityOutcome::Ok:     return Tone::Ok;
        case zetta::observability::ActivityOutcome::Denied: return Tone::Error;
        case zetta::observability::ActivityOutcome::Failed: return Tone::Error;
        case zetta::observability::ActivityOutcome::Info:   return Tone::Muted;
    }
    return Tone::Muted;
}

// A log line's severity (the run-log wire strings): error red, warn amber,
// anything else neutral.
inline Tone tone_of_log_level(std::string_view level) {
    if (level == "error") return Tone::Error;
    if (level == "warn")  return Tone::Warn;
    return Tone::Muted;
}

// A span / trace outcome (#640): a recorded span either succeeded or carries
// the engine's error — there is no in-flight state in the ring (spans record
// on finish), so the mapping is binary.
inline Tone tone_of_span(bool ok) { return ok ? Tone::Ok : Tone::Error; }

// A load/utilization percentage (0..100) on the shared gauge thresholds: green
// under moderate load, amber as it climbs, red when saturated. The Cluster
// view's gauges keep their richer rendering (eased bars, sparklines) but take
// their fill colour from here, so a "hot" node and a "hot" gauge agree.
constexpr Tone tone_of_utilization(float percent) {
    if (percent >= 85.0f) return Tone::Error;
    if (percent >= 60.0f) return Tone::Warn;
    return Tone::Active;
}

// Compile-time locks on the acceptance rule "the same state always looks the
// same": the terminal states of every lifecycle land on the same tones.
static_assert(tone_of(zetta::pipeline::RunStatus::Succeeded) == Tone::Ok);
static_assert(tone_of(zetta::compute::JobStatus::Succeeded) == Tone::Ok);
static_assert(tone_of(zetta::pipeline::CheckStatus::Pass) == Tone::Ok);
static_assert(tone_of(zetta::testkit::TestOutcome::Pass) == Tone::Ok);
static_assert(tone_of(zetta::pipeline::RunStatus::Failed) == Tone::Error);
static_assert(tone_of(zetta::compute::JobStatus::Failed) == Tone::Error);
static_assert(tone_of(zetta::pipeline::CheckStatus::Fail) == Tone::Error);
static_assert(tone_of(zetta::pipeline::CheckStatus::NotApplicable) == Tone::Muted);
static_assert(tone_of(zetta::testkit::TestOutcome::Fail) == Tone::Error);
static_assert(tone_of(zetta::pipeline::RunStatus::Running) == Tone::Warn);
static_assert(tone_of(zetta::compute::JobStatus::Running) == Tone::Warn);
static_assert(tone_of(zetta::pipeline::RunStatus::Pending) == Tone::Muted);
static_assert(tone_of(zetta::compute::JobStatus::Queued) == Tone::Muted);
static_assert(tone_of_utilization(90.0f) == Tone::Error);
static_assert(tone_of_utilization(70.0f) == Tone::Warn);
static_assert(tone_of_utilization(30.0f) == Tone::Active);

}  // namespace zetta::gui
