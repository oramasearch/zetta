#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

// The concrete correctness checks for #309 — the first real semantic checks that
// plug into the CorrectnessGate's `Check` interface (gate.hpp) without touching the
// gate itself. They read a small, documented set of **annotations** from each
// step's free-form `attributes` bag (see schemas/graph_step_annotations.schema.json
// and docs/pipeline/gate-checks.md) and turn a mistyped or leaky graph into a clear-reason
// gate failure (RULE-1 — no deploy without a passing gate).
//
// Both validate the annotations they read against the JSON Schema at the boundary
// (`graph_step_annotations_schema()` below), so malformed annotations
// fail fast with a precise reason rather than being silently misread. A step that
// carries none of the relevant annotations is simply skipped (it passes), so
// these checks are opt-in per step and never penalise a graph that doesn't use
// them.
namespace zetta::pipeline {

// M-4's executable shape and mandatory correctness contract, including a
// temporal split, strictly past features and a seasonal-naive baseline.
class ForecastGraphCheck final : public Check {
  public:
    std::string name() const override;
    CheckResult evaluate(const ir::ExecutionGraph &graph) const override;
};

// M-7 (#1046): labels are evaluation-only, the budget is explicit, scales
// are robust, and KPI baselines use only prior observations.
class AnomalyGraphCheck final : public Check {
public:
  std::string name() const override {
    return "anomaly_correctness";
  }
  CheckResult evaluate(const ir::ExecutionGraph &graph) const override;
};

// Schema/type check (#309): every field a step `requires` from its inputs must be
// provided — with a matching type — by the `schema` (source profile) of one of
// the step's transitive data-flow ancestors. A required field that no upstream
// source provides, or that is provided under a different type, fails the gate.
// Type tokens are opaque strings compared case-insensitively.
// Methodology check (#984): a model step must name a methodology the engine can actually
// execute. Until `model_kind` rode the model step it was a string in a bag nothing read, so a
// graph could name `forecast` and run the churn path — the failure was SILENT, and a silent
// wrong pipeline is the product lying fluently. A loud refusal is the honest outcome.
//
// The set is deliberately narrow: what `src/core/ml/` implements TODAY, not what the catalogue
// specifies. It widens as each M-n lands (#984). A model step with no `model_kind` is skipped
// rather than failed — the annotation is opt-in, exactly like the other checks here — but a
// model step that names an UNKNOWN methodology fails, because the alternative is the agent
// proposing work the engine will silently not do.
// Temporal-validation check (#984, M-4). A methodology whose data is time-ordered must be
// validated over time and must NOT be calibrated.
//
// Both halves matter and the second is the counter-intuitive one. Calibration maps scores to
// probabilities by fitting on held-out rows — correct and necessary for churn. On a forecast it
// is a category error: there is no probability to calibrate, and fitting a calibrator over a
// time series held out by row is the same leak `temporal_split` exists to prevent, wearing a
// respectable name. So a forecasting graph that carries `calibrated: true` is not "extra rigour",
// it is the churn recipe applied to a problem it does not fit — precisely the confusion the
// corpus's neighbouring-solution negative control is trying to teach against.
//
// Opt-in like every check here: a graph whose model step names no methodology is skipped.
class TemporalValidationCheck final : public Check {
public:
    std::string name() const override { return "temporal_validation"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;

    // Methodologies whose data is time-ordered, so temporal validation is required and
    // calibration is refused.
    static bool is_time_ordered(std::string_view model_kind);
};

class MethodologyCheck final : public Check {
public:
    std::string name() const override { return "methodology"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;

    // The methodologies the engine can execute today. Widened by #984, never by a caller —
    // a per-call override is how "the engine supports it" becomes unfalsifiable.
    static const std::vector<std::string>& implemented();
};

class SchemaTypeCheck final : public Check {
public:
    std::string name() const override { return "schema_type"; }
    CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

// M-3 must keep entity/outcome roles out of features and execute its own
// globally fitted preprocessing and assignment/profile output graph.
class SegmentationCheck final : public Check {
  public:
    std::string name() const override;
    CheckResult evaluate(const ir::ExecutionGraph &graph) const override;
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

// The step-annotation JSON Schema (draft-07) — the code-owned source of truth;
// schemas/graph_step_annotations.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& graph_step_annotations_schema();

} // namespace zetta::pipeline
