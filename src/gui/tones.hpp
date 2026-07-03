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
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/scaling_policy.hpp"
#include "zetta/pipeline/run.hpp"
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

// A correctness-gate check: pass green, warn amber, fail red.
constexpr Tone tone_of(zetta::pipeline::CheckStatus status) {
    switch (status) {
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

// A log line's severity (the run-log wire strings): error red, warn amber,
// anything else neutral.
inline Tone tone_of_log_level(std::string_view level) {
    if (level == "error") return Tone::Error;
    if (level == "warn")  return Tone::Warn;
    return Tone::Muted;
}

}  // namespace zetta::gui
