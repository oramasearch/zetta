#pragma once

#include <string>

#include "zetta/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/validation.hpp"

// The concrete correctness checks for #309 — the first real semantic checks that
// plug into the CorrectnessGate's `Check` seam (gate.hpp) without touching the
// gate itself. They read a small, documented set of **annotations** from each
// step's free-form `attributes` bag (see schemas/graph_step_annotations.schema.json
// and docs/pipeline/gate-checks.md) and turn a mistyped or leaky graph into a clear-reason
// gate failure (RULE-1 — no deploy without a passing gate).
//
// Both validate the annotations they read against the JSON Schema at the boundary
// (src/core/graph_checks.cpp holds the runtime copy), so malformed annotations
// fail fast with a precise reason rather than being silently misread. A step that
// carries none of the relevant annotations is simply skipped (it passes), so
// these checks are opt-in per step and never penalise a graph that doesn't use
// them.
namespace zetta {

// Schema/type check (#309): every field a step `requires` from its inputs must be
// provided — with a matching type — by the `schema` (source profile) of one of
// the step's transitive data-flow ancestors. A required field that no upstream
// source provides, or that is provided under a different type, fails the gate.
// Type tokens are opaque strings compared case-insensitively.
class SchemaTypeCheck final : public Check {
public:
    std::string name() const override { return "schema_type"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// Data-leakage / point-in-time check (#309): for an ML step that declares
// `features` (with an `as_of` cutoff), no feature may draw on data dated after
// the cutoff (`data_as_of` > `as_of` — a point-in-time violation) and no feature
// may be the target or be derived from it (`derived_from` / `leaks_target` /
// matching the `label` — target leakage). Either makes the model train on
// information it would not have at prediction time, so the gate fails.
class DataLeakageCheck final : public Check {
public:
    std::string name() const override { return "data_leakage"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

}  // namespace zetta
