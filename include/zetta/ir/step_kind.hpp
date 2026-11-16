// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <optional>
#include <string_view>

// The step-kind taxonomy for the ExecutionGraph IR (see execution_graph.hpp).
//
// connect → clean → model → index → serve → act
//
// These are the fixed stages an execution graph moves a data/AI system through.
// They are named here so every layer that touches the IR (compiler, gate,
// deploy, run, and the GUI) refers to the same taxonomy.
namespace zetta::ir {

// The stage a graph node belongs to. Declaration order IS the canonical
// authoring/pipeline order, so the underlying values increase along the
// pipeline (Connect = 0 … Act = 5). A graph need not use every kind, and the
// ordering between specific nodes is expressed by edges — this enum only names
// the stages; enforcing legal stage transitions is the gate's job (a later
// slice), not part of this data model.
enum class StepKind {
    Connect,  // pull data in from a source (database, file, API, stream)
    Clean,    // normalize / dedupe / validate / reshape the raw data
    Model,    // train, evaluate, or apply a model (ML / statistical / LLM)
    Index,    // build a search / vector / feature index over the results
    Serve,    // expose results behind a query / API / endpoint
    Act,      // take an action on the world (notify, write back, trigger)
};

// Every step kind in canonical pipeline order. Useful for walking the taxonomy
// itself — e.g. grouping a graph's nodes by stage for display.
inline constexpr std::array<StepKind, 6> all_step_kinds() {
    return {StepKind::Connect, StepKind::Clean, StepKind::Model,
            StepKind::Index,   StepKind::Serve, StepKind::Act};
}

// The lowercase canonical name of a step kind ("connect", "clean", …). Used for
// labels, log lines, and test output — and as the `kind` token in the IR's JSON
// form (see ir/serialization.hpp).
constexpr std::string_view to_string(StepKind kind) {
    switch (kind) {
        case StepKind::Connect: return "connect";
        case StepKind::Clean:   return "clean";
        case StepKind::Model:   return "model";
        case StepKind::Index:   return "index";
        case StepKind::Serve:   return "serve";
        case StepKind::Act:     return "act";
    }
    return "unknown";
}

// Parse a canonical step-kind name back to the enum; std::nullopt if unknown.
// The inverse of to_string(StepKind) — used both for building graphs from named
// stages and for deserializing the IR's JSON form (ir/serialization.hpp).
constexpr std::optional<StepKind> step_kind_from_name(std::string_view name) {
    for (const StepKind kind : all_step_kinds()) {
        if (to_string(kind) == name) return kind;
    }
    return std::nullopt;
}

}  // namespace zetta::ir
