// SPDX-License-Identifier: MIT

#include "zetta/ml/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

// Probability calibration (Platt scaling + isotonic regression) for the histogram
// GBDT. The design + the why live in the header; this file is the numerical
// reference. See docs/ml/calibration.md.
namespace zetta::ml {

namespace {

using compute::ComputeStatus;

// Numerically stable logistic sigmoid (the gbdt.cpp helper, kept local).
double sigmoid(double x) {
    if (x >= 0.0) {
        const double z = std::exp(-x);
        return 1.0 / (1.0 + z);
    }
    const double z = std::exp(x);
    return z / (1.0 + z);
}

// --- the calibration JSON Schema (pboettch draft-07, like the GBDT model body) ---

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ml.Calibrator",
      "type": "object",
      "properties": {
        "method": { "type": "string", "enum": ["platt", "isotonic"] },
        "platt_a": { "type": "number" },
        "platt_b": { "type": "number" },
        "iso_bounds": { "type": "array", "items": { "type": "number" } },
        "iso_values": {
          "type": "array",
          "items": { "type": "number", "minimum": 0, "maximum": 1 }
        }
      },
      "required": ["method"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure without letting the validator throw across
// the library boundary (the run.cpp / gbdt.cpp pattern).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

constexpr double kProbEpsilon = 1e-12;  // clamps calibrated probabilities off 0/1

// --- Platt scaling -----------------------------------------------------------

// Fit `sigmoid(a*raw + b)` to the labels by the robust Newton method of Lin, Lin &
// Weng (2007), "A Note on Platt's Probabilistic Outputs for Support Vector
// Machines". We fit in Platt's own convention p(y=1) = 1/(1 + exp(A*f + B)) — which
// has the published, line-searched pseudocode — then convert to our convention
// (sigmoid(a*f + b)) via a = -A, b = -B. The target smoothing
// (hi = (N+ +1)/(N+ +2), lo = 1/(N- +2)) keeps a tiny held-out set from pinning the
// fit at the extremes.
void fit_platt(const std::vector<double>& scores,
               const std::vector<double>& labels, double& out_a, double& out_b) {
    const std::size_t count = scores.size();
    double positives = 0.0;
    for (const double label : labels) positives += label >= 0.5 ? 1.0 : 0.0;
    const double negatives = static_cast<double>(count) - positives;

    const double hi_target = (positives + 1.0) / (positives + 2.0);
    const double lo_target = 1.0 / (negatives + 2.0);
    std::vector<double> target(count);
    for (std::size_t i = 0; i < count; ++i) {
        target[i] = labels[i] >= 0.5 ? hi_target : lo_target;
    }

    double a_param = 0.0;  // Platt's A
    double b_param = std::log((negatives + 1.0) / (positives + 1.0));  // Platt's B

    constexpr double kMinStep = 1e-10;
    constexpr double kSigma = 1e-12;  // Hessian regularization
    constexpr int kMaxIterations = 100;

    // The objective (negative log-likelihood), evaluated in a stable form.
    const auto objective = [&](double a, double b) {
        double total = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const double z = scores[i] * a + b;
            if (z >= 0.0) {
                total += target[i] * z + std::log1p(std::exp(-z));
            } else {
                total += (target[i] - 1.0) * z + std::log1p(std::exp(z));
            }
        }
        return total;
    };

    double value = objective(a_param, b_param);
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        double h11 = kSigma;
        double h22 = kSigma;
        double h21 = 0.0;
        double g1 = 0.0;
        double g2 = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const double z = scores[i] * a_param + b_param;
            double p;  // p(y=1) in Platt's convention
            double q;  // 1 - p
            if (z >= 0.0) {
                const double e = std::exp(-z);
                p = e / (1.0 + e);
                q = 1.0 / (1.0 + e);
            } else {
                const double e = std::exp(z);
                p = 1.0 / (1.0 + e);
                q = e / (1.0 + e);
            }
            const double weight = p * q;
            h11 += scores[i] * scores[i] * weight;
            h22 += weight;
            h21 += scores[i] * weight;
            const double residual = target[i] - p;
            g1 += scores[i] * residual;
            g2 += residual;
        }
        if (std::abs(g1) < 1e-5 && std::abs(g2) < 1e-5) break;

        const double det = h11 * h22 - h21 * h21;
        const double delta_a = -(h22 * g1 - h21 * g2) / det;
        const double delta_b = -(-h21 * g1 + h11 * g2) / det;
        const double directional = g1 * delta_a + g2 * delta_b;

        double step = 1.0;
        while (step >= kMinStep) {
            const double new_a = a_param + step * delta_a;
            const double new_b = b_param + step * delta_b;
            const double new_value = objective(new_a, new_b);
            if (new_value < value + 1e-4 * step * directional) {
                a_param = new_a;
                b_param = new_b;
                value = new_value;
                break;
            }
            step /= 2.0;
        }
        if (step < kMinStep) break;  // line search failed -> converged
    }

    // Convert Platt's p = sigmoid(-(A*f + B)) to our sigmoid(a*f + b).
    out_a = -a_param;
    out_b = -b_param;
}

// --- Isotonic regression (PAVA) ----------------------------------------------

// One pooled block during the pool-adjacent-violators sweep.
struct IsotonicBlock {
    double value;   // pooled mean label
    double weight;  // number of points
    double bound;   // the smallest score in the block (its left edge)
};

// Fit a non-decreasing step function score -> calibrated probability. Points are
// sorted by score; adjacent monotonicity violations are pooled to their
// weighted-mean label. The result is the unique isotonic regression of the labels
// on the scores. Output: `bounds` (each block's left edge, non-decreasing) +
// `values` (each block's calibrated probability, non-decreasing in [0,1]).
void fit_isotonic(const std::vector<double>& scores,
                  const std::vector<double>& labels,
                  std::vector<double>& bounds, std::vector<double>& values) {
    const std::size_t count = scores.size();
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    // Sort by score; tie-break by label so the order is deterministic (the pooled
    // value of a tied group is the same regardless, so this only fixes the layout).
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (scores[a] != scores[b]) return scores[a] < scores[b];
        return labels[a] < labels[b];
    });

    std::vector<IsotonicBlock> blocks;
    blocks.reserve(count);
    for (const std::size_t index : order) {
        blocks.push_back(IsotonicBlock{.value = labels[index] >= 0.5 ? 1.0 : 0.0,
                                       .weight = 1.0, .bound = scores[index]});
        // Pool while the previous block's value exceeds this one's (a violation).
        while (blocks.size() >= 2 &&
               blocks[blocks.size() - 2].value > blocks.back().value) {
            IsotonicBlock right = blocks.back();
            blocks.pop_back();
            IsotonicBlock& left = blocks.back();
            const double total_weight = left.weight + right.weight;
            left.value =
                (left.value * left.weight + right.value * right.weight) /
                total_weight;
            left.weight = total_weight;
            // bound stays the left block's (smaller) left edge.
        }
    }

    bounds.clear();
    values.clear();
    bounds.reserve(blocks.size());
    values.reserve(blocks.size());
    for (const IsotonicBlock& block : blocks) {
        bounds.push_back(block.bound);
        values.push_back(std::clamp(block.value, 0.0, 1.0));
    }
}

}  // namespace

// --- CalibrationMethod names -------------------------------------------------

std::string to_string(CalibrationMethod method) {
    switch (method) {
        case CalibrationMethod::Platt: return "platt";
        case CalibrationMethod::Isotonic: return "isotonic";
    }
    return "platt";
}

std::optional<CalibrationMethod> calibration_method_from_string(
    const std::string& name) {
    if (name == "platt") return CalibrationMethod::Platt;
    if (name == "isotonic") return CalibrationMethod::Isotonic;
    return std::nullopt;
}

// --- Calibrator --------------------------------------------------------------

double Calibrator::calibrate(double raw_score) const {
    if (method == CalibrationMethod::Platt) {
        return std::clamp(sigmoid(platt_a * raw_score + platt_b), kProbEpsilon,
                          1.0 - kProbEpsilon);
    }
    // Isotonic: the value of the last segment whose bound <= raw_score (the first
    // segment's value when below the first bound). An empty table is the degenerate
    // identity-ish constant 0.5.
    if (iso_bounds.empty()) return 0.5;
    if (raw_score < iso_bounds.front()) return iso_values.front();
    // upper_bound finds the first bound strictly greater than raw_score; the
    // segment we want is the one just before it.
    const auto it =
        std::upper_bound(iso_bounds.begin(), iso_bounds.end(), raw_score);
    const std::size_t segment =
        static_cast<std::size_t>(it - iso_bounds.begin()) - 1;
    return iso_values[segment];
}

bool Calibrator::is_identity() const noexcept {
    return method == CalibrationMethod::Platt && platt_a == 1.0 &&
           platt_b == 0.0;
}

nlohmann::json Calibrator::to_json() const {
    nlohmann::json value;
    value["method"] = to_string(method);
    if (method == CalibrationMethod::Platt) {
        value["platt_a"] = platt_a;
        value["platt_b"] = platt_b;
    } else {
        value["iso_bounds"] = iso_bounds;
        value["iso_values"] = iso_values;
    }
    return value;
}

std::optional<Calibrator> Calibrator::from_json(const nlohmann::json& value,
                                                std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return std::nullopt;
    }
    Calibrator calibrator;
    const std::optional<CalibrationMethod> method =
        calibration_method_from_string(value.at("method").get<std::string>());
    if (!method) {  // unreachable past the enum schema, but keep the boundary total
        if (error) *error = "method: unknown calibration method";
        return std::nullopt;
    }
    calibrator.method = *method;
    if (calibrator.method == CalibrationMethod::Platt) {
        // Platt needs both parameters; default the identity if a producer omits
        // them is not allowed — require them so a half-written map can't load.
        if (!value.contains("platt_a") || !value.contains("platt_b")) {
            if (error) *error = "platt: platt_a and platt_b are required";
            return std::nullopt;
        }
        calibrator.platt_a = value.at("platt_a").get<double>();
        calibrator.platt_b = value.at("platt_b").get<double>();
        return calibrator;
    }
    // Isotonic: the arrays share a non-empty length, bounds non-decreasing, values
    // non-decreasing (the schema already pins values into [0,1]).
    if (!value.contains("iso_bounds") || !value.contains("iso_values")) {
        if (error) *error = "isotonic: iso_bounds and iso_values are required";
        return std::nullopt;
    }
    calibrator.iso_bounds = value.at("iso_bounds").get<std::vector<double>>();
    calibrator.iso_values = value.at("iso_values").get<std::vector<double>>();
    if (calibrator.iso_bounds.empty() ||
        calibrator.iso_bounds.size() != calibrator.iso_values.size()) {
        if (error)
            *error = "isotonic: iso_bounds and iso_values must share a non-empty "
                     "length";
        return std::nullopt;
    }
    for (std::size_t i = 1; i < calibrator.iso_bounds.size(); ++i) {
        if (calibrator.iso_bounds[i] < calibrator.iso_bounds[i - 1]) {
            if (error) *error = "isotonic: iso_bounds must be non-decreasing";
            return std::nullopt;
        }
        if (calibrator.iso_values[i] < calibrator.iso_values[i - 1]) {
            if (error) *error = "isotonic: iso_values must be non-decreasing";
            return std::nullopt;
        }
    }
    return calibrator;
}

const nlohmann::json& calibration_schema() { return schema_document(); }

std::vector<double> Calibrator::to_wire() const {
    if (method == CalibrationMethod::Platt) {
        return {0.0, platt_a, platt_b};
    }
    std::vector<double> wire;
    wire.reserve(2 + iso_bounds.size() + iso_values.size());
    wire.push_back(1.0);
    wire.push_back(static_cast<double>(iso_bounds.size()));
    wire.insert(wire.end(), iso_bounds.begin(), iso_bounds.end());
    wire.insert(wire.end(), iso_values.begin(), iso_values.end());
    return wire;
}

std::optional<Calibrator> Calibrator::from_wire(
    const std::vector<double>& wire) {
    if (wire.empty()) return std::nullopt;  // "no calibrator"
    Calibrator calibrator;
    if (wire.front() == 0.0) {
        if (wire.size() != 3) return std::nullopt;
        calibrator.method = CalibrationMethod::Platt;
        calibrator.platt_a = wire[1];
        calibrator.platt_b = wire[2];
        return calibrator;
    }
    if (wire.front() == 1.0) {
        if (wire.size() < 2) return std::nullopt;
        const auto segments = static_cast<std::size_t>(wire[1]);
        if (segments == 0 || wire.size() != 2 + 2 * segments) return std::nullopt;
        calibrator.method = CalibrationMethod::Isotonic;
        calibrator.iso_bounds.assign(wire.begin() + 2, wire.begin() + 2 + segments);
        calibrator.iso_values.assign(wire.begin() + 2 + segments,
                                     wire.begin() + 2 + 2 * segments);
        return calibrator;
    }
    return std::nullopt;
}

// --- fit_calibrator ----------------------------------------------------------

CalibrationResult fit_calibrator(const std::vector<double>& raw_scores,
                                 const std::vector<double>& labels,
                                 CalibrationMethod method) {
    CalibrationResult result;
    if (raw_scores.size() != labels.size()) {
        result.status = ComputeStatus::invalid_argument(
            "fit_calibrator: scores and labels length mismatch");
        return result;
    }
    if (raw_scores.empty()) {
        result.status =
            ComputeStatus::invalid_argument("fit_calibrator: no held-out rows");
        return result;
    }
    std::size_t positives = 0;
    for (const double label : labels) {
        if (label >= 0.5) ++positives;
    }
    if (positives == 0 || positives == labels.size()) {
        result.status = ComputeStatus::invalid_argument(
            "fit_calibrator: held-out labels are a single class");
        return result;
    }

    result.calibrator.method = method;
    if (method == CalibrationMethod::Platt) {
        fit_platt(raw_scores, labels, result.calibrator.platt_a,
                  result.calibrator.platt_b);
    } else {
        fit_isotonic(raw_scores, labels, result.calibrator.iso_bounds,
                     result.calibrator.iso_values);
    }
    result.status = ComputeStatus::success();
    return result;
}

// --- metrics -----------------------------------------------------------------

nlohmann::json CalibrationMetrics::to_json() const {
    return nlohmann::json{{"brier", brier}, {"ece", ece}};
}

CalibrationMetrics calibration_metrics(const std::vector<double>& probabilities,
                                       const std::vector<double>& labels,
                                       int bins) {
    CalibrationMetrics metrics;
    const std::size_t count = std::min(probabilities.size(), labels.size());
    if (count == 0) return metrics;
    const std::size_t bin_count =
        static_cast<std::size_t>(std::max(1, bins));

    double brier = 0.0;
    std::vector<double> bin_prob(bin_count, 0.0);
    std::vector<double> bin_label(bin_count, 0.0);
    std::vector<std::size_t> bin_count_n(bin_count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const double p = std::clamp(probabilities[i], 0.0, 1.0);
        const double y = labels[i] >= 0.5 ? 1.0 : 0.0;
        brier += (p - y) * (p - y);
        std::size_t bin = static_cast<std::size_t>(p * static_cast<double>(bin_count));
        if (bin >= bin_count) bin = bin_count - 1;  // p == 1.0 lands in the last bin
        bin_prob[bin] += p;
        bin_label[bin] += y;
        ++bin_count_n[bin];
    }
    metrics.brier = brier / static_cast<double>(count);

    double ece = 0.0;
    for (std::size_t bin = 0; bin < bin_count; ++bin) {
        if (bin_count_n[bin] == 0) continue;
        const double size = static_cast<double>(bin_count_n[bin]);
        const double confidence = bin_prob[bin] / size;
        const double accuracy = bin_label[bin] / size;
        ece += (size / static_cast<double>(count)) *
               std::abs(confidence - accuracy);
    }
    metrics.ece = ece;
    return metrics;
}

}  // namespace zetta::ml
