#pragma once

// The ONE place an engine enum or internal kind string maps to product
// language (#635). The engine's canonical lowercase wire names ("featurize",
// "ddf", "near_data") stay exactly what SQL, the JSON schemas, and the CRDT
// store — panels render these words instead. Adding a kind/status-bearing
// enum = adding ONE overload here, never a panel-local switch; a raw wire
// value may appear in the mono metadata strip, but never as primary text.
//
// Pairs with tones.hpp (the status → colour mapping): `tone_of` says how a
// state looks, `label_of` says what it is called. Pure (no ImGui) on purpose,
// like the rest of the GUI's presentation vocabulary.

#include <cctype>
#include <string>
#include <string_view>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/scaling_policy.hpp"
#include "zetta/index/search.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/graph_plan.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/run_progress.hpp"
#include "zetta/pipeline/validation.hpp"
#include "zetta/testkit/test_suite.hpp"

namespace zetta::gui {

// A compute job's kind: what the cluster is doing with the hardware, in the
// product's words. The wire names ("featurize" … "ddf") remain the SQL/JSON
// spellings.
constexpr std::string_view label_of(zetta::compute::JobKind kind) {
    switch (kind) {
        case zetta::compute::JobKind::Featurize: return "Feature build";
        case zetta::compute::JobKind::Train:     return "Model training";
        case zetta::compute::JobKind::Explain:   return "Model explanation";
        case zetta::compute::JobKind::Score:     return "Batch scoring";
        case zetta::compute::JobKind::Ddf:       return "Data transform";
    }
    return "Compute job";
}

// A compute job along its lifecycle. Same words as a run's — the two
// lifecycles deliberately read identically (see tones.hpp).
constexpr std::string_view label_of(zetta::compute::JobStatus status) {
    switch (status) {
        case zetta::compute::JobStatus::Queued:    return "Queued";
        case zetta::compute::JobStatus::Running:   return "Running";
        case zetta::compute::JobStatus::Succeeded: return "Succeeded";
        case zetta::compute::JobStatus::Failed:    return "Failed";
    }
    return "Queued";
}

constexpr std::string_view label_of(zetta::pipeline::RunStatus status) {
    switch (status) {
        case zetta::pipeline::RunStatus::Pending:   return "Pending";
        case zetta::pipeline::RunStatus::Running:   return "Running";
        case zetta::pipeline::RunStatus::Succeeded: return "Succeeded";
        case zetta::pipeline::RunStatus::Failed:    return "Failed";
    }
    return "Pending";
}

constexpr std::string_view label_of(zetta::pipeline::StepRunStatus status) {
    switch (status) {
        case zetta::pipeline::StepRunStatus::Pending:   return "Pending";
        case zetta::pipeline::StepRunStatus::Running:   return "Running";
        case zetta::pipeline::StepRunStatus::Succeeded: return "Succeeded";
        case zetta::pipeline::StepRunStatus::Failed:    return "Failed";
    }
    return "Pending";
}

// The pipeline stages (connect → clean → model → index → serve → act) are
// already product vocabulary; the label is the sentence-case form.
constexpr std::string_view label_of(zetta::ir::StepKind kind) {
    switch (kind) {
        case zetta::ir::StepKind::Connect: return "Connect";
        case zetta::ir::StepKind::Clean:   return "Clean";
        case zetta::ir::StepKind::Model:   return "Model";
        case zetta::ir::StepKind::Index:   return "Index";
        case zetta::ir::StepKind::Serve:   return "Serve";
        case zetta::ir::StepKind::Act:     return "Act";
    }
    return "Step";
}

constexpr std::string_view label_of(zetta::pipeline::DeploymentKind kind) {
    switch (kind) {
        case zetta::pipeline::DeploymentKind::Local:      return "Local";
        case zetta::pipeline::DeploymentKind::Kubernetes: return "Kubernetes";
        case zetta::pipeline::DeploymentKind::Vpc:        return "VPC";
        case zetta::pipeline::DeploymentKind::Cloud:      return "Cloud";
    }
    return "Local";
}

// Where a step's compute lands on the cluster. These stay technical concepts;
// the words are the readable forms and the panel pairs them with a
// term_hint(...) naming what each means (app_internal.hpp).
constexpr std::string_view label_of(zetta::pipeline::StepPlacement placement) {
    switch (placement) {
        case zetta::pipeline::StepPlacement::NearData:    return "Near data";
        case zetta::pipeline::StepPlacement::FanOut:      return "Fan-out";
        case zetta::pipeline::StepPlacement::AllNodes:    return "All nodes";
        case zetta::pipeline::StepPlacement::Coordinator: return "Coordinator";
    }
    return "Fan-out";
}

constexpr std::string_view label_of(zetta::pipeline::CheckStatus status) {
    switch (status) {
        case zetta::pipeline::CheckStatus::Pass: return "Pass";
        case zetta::pipeline::CheckStatus::Warn: return "Warning";
        case zetta::pipeline::CheckStatus::Fail: return "Fail";
    }
    return "Fail";
}

constexpr std::string_view label_of(zetta::testkit::TestOutcome outcome) {
    switch (outcome) {
        case zetta::testkit::TestOutcome::Pass:    return "Pass";
        case zetta::testkit::TestOutcome::Fail:    return "Fail";
        case zetta::testkit::TestOutcome::Skipped: return "Skipped";
        case zetta::testkit::TestOutcome::Error:   return "Error";
    }
    return "Error";
}

// How one pipeline step depends on another (the graph editor's edge picker).
constexpr std::string_view label_of(zetta::ir::EdgeKind kind) {
    switch (kind) {
        case zetta::ir::EdgeKind::DataFlow: return "Data flow";
        case zetta::ir::EdgeKind::Control:  return "Control";
    }
    return "Data flow";
}

// Which path a search took: the whole cluster answered (per-partition
// scatter-gather) or the degraded single-node index did.
constexpr std::string_view label_of(zetta::index::SearchPath path) {
    switch (path) {
        case zetta::index::SearchPath::Distributed:  return "Whole cluster";
        case zetta::index::SearchPath::Consolidated: return "Single node";
    }
    return "Whole cluster";
}

// The autoscaler's reward objective (the Jobs panel's scaling preview).
constexpr std::string_view label_of(zetta::compute::ScalingObjective objective) {
    switch (objective) {
        case zetta::compute::ScalingObjective::MinimizeCost:  return "Minimize cost";
        case zetta::compute::ScalingObjective::RuntimeTarget: return "Runtime target";
        case zetta::compute::ScalingObjective::DataShape:     return "Data shape";
    }
    return "Minimize cost";
}

constexpr std::string_view label_of(zetta::compute::ScalingAction action) {
    switch (action) {
        case zetta::compute::ScalingAction::ScaleUp:   return "Scale up";
        case zetta::compute::ScalingAction::Hold:      return "Hold";
        case zetta::compute::ScalingAction::ScaleDown: return "Scale down";
    }
    return "Hold";
}

constexpr std::string_view label_of(zetta::observability::ActivityOutcome outcome) {
    switch (outcome) {
        case zetta::observability::ActivityOutcome::Ok:     return "OK";
        case zetta::observability::ActivityOutcome::Denied: return "Denied";
        case zetta::observability::ActivityOutcome::Failed: return "Failed";
        case zetta::observability::ActivityOutcome::Info:   return "Info";
    }
    return "Info";
}

// The product word for a free-text kind token the engine hands the GUI as a
// string (a run output's "dataset" / "model" / "index", a job row already
// flattened to its wire name, a cluster-animation flow kind). Known tokens map
// to their product words; anything unknown degrades gracefully — underscores
// become spaces and the first letter is capitalized — so a new wire name is
// never rendered raw even before it gets a proper mapping here.
inline std::string humanize_kind(std::string_view raw) {
    // Wire names with a product word that isn't just a casing fix.
    if (raw == "ddf")       return "Data transform";
    if (raw == "featurize") return "Feature build";
    if (raw == "train")     return "Model training";
    if (raw == "explain")   return "Model explanation";
    if (raw == "score")     return "Batch scoring";
    std::string label{raw};
    for (char& character : label) {
        if (character == '_' || character == '-') character = ' ';
    }
    if (!label.empty()) {
        label.front() = static_cast<char>(
            std::toupper(static_cast<unsigned char>(label.front())));
    }
    return label;
}

// Compile-time locks on the acceptance rule "no raw enum spelling rendered as
// primary text": the wire names that read as engine-speak have product words.
static_assert(label_of(zetta::compute::JobKind::Ddf) == "Data transform");
static_assert(label_of(zetta::compute::JobKind::Featurize) == "Feature build");
static_assert(label_of(zetta::pipeline::StepPlacement::NearData) == "Near data");
static_assert(label_of(zetta::pipeline::RunStatus::Succeeded) == "Succeeded");
static_assert(label_of(zetta::pipeline::DeploymentKind::Vpc) == "VPC");

}  // namespace zetta::gui
