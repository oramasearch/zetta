#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_status.hpp"

// Probability calibration for the histogram GBDT (FEAT-6, #391; part of the native
// distributed accelerated-compute pass for UC-1 churn, #264). The trainer
// (gbdt.hpp) orders accounts well (a strong AUC) but its raw probabilities
// `sigmoid(raw)` are not necessarily **calibrated** — among accounts scored at risk
// 0.7, the realized churn fraction may not be 0.7. Calibration fits a monotone map
// `raw score -> calibrated probability` on a **held-out** split so the risk scores
// are trustworthy, the way a *calibrated* XGBoost is the reference. AUC is
// preserved (the map is monotone), while reliability (Brier / ECE) improves.
//
// Two methods, both standard:
//
//   * **Platt scaling** — a two-parameter logistic fit `sigmoid(a*raw + b)`. Robust
//     on small calibration sets (only two parameters) but assumes a sigmoidal
//     distortion. Fit by Newton's method with Platt's target smoothing
//     (Lin, Lin & Weng 2007), so it does not overfit a tiny held-out set.
//   * **Isotonic regression** — a non-parametric, non-decreasing step function fit
//     by the Pool-Adjacent-Violators Algorithm (PAVA). More flexible (it can fix a
//     non-sigmoidal distortion) but needs more data to avoid overfitting.
//
// **Calibrate on the raw score (logit).** Both methods take the model's
// `predict_raw` value as input, not the probability. For Platt this is the classic
// formulation; for isotonic it is equivalent (the probability is a monotone
// transform of the raw score, and isotonic is invariant to monotone reparametrization
// of its input). With Platt `a=1, b=0` the map is the identity, so an uncalibrated
// model reduces to plain `sigmoid(raw)`.
//
// Host-side + non-throwing (zetta-core's no-exceptions convention): the entry points
// return a `compute::ComputeStatus` / `std::optional`. The JSON boundary mirrors
// `schemas/calibration.schema.json` (validated with the pboettch draft-07 validator,
// like the GBDT model body it rides inside). See docs/calibration.md.
namespace zetta::ml {

// Which calibration map to fit.
enum class CalibrationMethod { Platt, Isotonic };

// The lowercase wire name of a method ("platt" / "isotonic") and its inverse.
std::string to_string(CalibrationMethod method);
std::optional<CalibrationMethod> calibration_method_from_string(
    const std::string& name);

// A fitted calibration map: a model raw score (logit) -> a calibrated probability.
// One of the two methods is active; the other method's fields are unused. Default
// is the Platt identity (a=1, b=0), which maps `raw -> sigmoid(raw)`.
struct Calibrator {
    CalibrationMethod method = CalibrationMethod::Platt;

    // Platt: calibrated probability = sigmoid(platt_a * raw + platt_b).
    double platt_a = 1.0;
    double platt_b = 0.0;

    // Isotonic: a right-continuous, non-decreasing step function. `iso_bounds` are
    // the (non-decreasing) left edges of each segment in raw-score space and
    // `iso_values` the calibrated probability of each segment (non-decreasing, in
    // [0,1]); the two share a length. calibrate(raw) returns the value of the last
    // segment whose bound <= raw (the first segment's value below the first bound).
    std::vector<double> iso_bounds;
    std::vector<double> iso_values;

    // Map one model raw score to a calibrated probability in [0,1].
    double calibrate(double raw_score) const;

    // Whether this is the Platt identity (the no-op calibrator) — `sigmoid(raw)`.
    bool is_identity() const noexcept;

    // The JSON form (mirrors schemas/calibration.schema.json): the method tag plus
    // only the active method's parameters.
    nlohmann::json to_json() const;

    // Parse + validate at the boundary (the pboettch draft-07 validator over the
    // calibration schema, then the structural rules the schema can't express:
    // the isotonic arrays share a non-empty length, the bounds are non-decreasing,
    // and the values are non-decreasing in [0,1]). std::nullopt on any failure,
    // with a path-qualified reason in `error`.
    static std::optional<Calibrator> from_json(const nlohmann::json& value,
                                               std::string* error = nullptr);

    // A flat double encoding for the compute plane (gathered/broadcast through the
    // collectives when fitting inside a distributed train job — no new wire format).
    // Platt -> [0, a, b]; isotonic -> [1, K, bounds..., values...]. An empty vector
    // means "no calibrator" (a failed/declined fit); from_wire returns nullopt for
    // it (and for any malformed encoding).
    std::vector<double> to_wire() const;
    static std::optional<Calibrator> from_wire(const std::vector<double>& wire);
};

// The calibration JSON Schema (draft-07), exposed for tests/introspection.
const nlohmann::json& calibration_schema();

// The outcome of fitting a calibrator.
struct CalibrationResult {
    Calibrator calibrator;
    compute::ComputeStatus status;
    bool ok() const noexcept { return status.ok(); }
};

// Fit a calibrator on a held-out set: `raw_scores[i]` the model's raw score
// (logit) for held-out row i, `labels[i]` its {0,1} target. Non-throwing; fails
// (a failing ComputeStatus) when the inputs disagree in length, are empty, or carry
// a single label class (calibration is undefined without both classes).
CalibrationResult fit_calibrator(const std::vector<double>& raw_scores,
                                 const std::vector<double>& labels,
                                 CalibrationMethod method);

// --- Reliability metrics -----------------------------------------------------

// Calibration quality over predicted probabilities + {0,1} labels (lower is
// better for both).
struct CalibrationMetrics {
    double brier = 0.0;  // mean squared error of probability vs label
    double ece = 0.0;    // expected calibration error (binned |confidence-accuracy|)

    // The JSON form: { "brier", "ece" }.
    nlohmann::json to_json() const;
};

// Brier score (mean (p - y)^2) + expected calibration error (equal-width bins over
// [0,1]; each bin contributes |mean probability - mean label| weighted by its share
// of the samples). `bins` is clamped to at least 1. Empty input -> zeros.
CalibrationMetrics calibration_metrics(const std::vector<double>& probabilities,
                                       const std::vector<double>& labels,
                                       int bins = 10);

}  // namespace zetta::ml
