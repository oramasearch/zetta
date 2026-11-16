// SPDX-License-Identifier: MIT

#include "zetta/solutions/monitor.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/util/timestamp.hpp"

#include "../catalog.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <system_error>
#include <utility>

#include <nlohmann/json-schema.hpp>

namespace zetta::solutions {
namespace fs = std::filesystem;

namespace {

constexpr double kDistributionEpsilon = 1e-6;
constexpr double kProbabilityEpsilon = 1e-15;

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.SolutionMonitor",
      "type": "object",
      "definitions": {
        "distribution": {
          "type": "object",
          "required": ["name", "boundaries", "expected", "observed", "psi", "breached", "observed_at"],
          "additionalProperties": false,
          "properties": {
            "name": { "type": "string", "minLength": 1 },
            "boundaries": { "type": "array", "items": { "type": "number" } },
            "expected": { "type": "array", "minItems": 1, "items": { "type": "number", "minimum": 0 } },
            "observed": { "type": "array", "items": { "type": "number", "minimum": 0 } },
            "psi": { "type": "number", "minimum": 0 },
            "breached": { "type": "boolean" },
            "observed_at": { "type": "string" }
          }
        },
        "bound": {
          "type": "object",
          "required": ["name"],
          "additionalProperties": false,
          "properties": {
            "name": { "type": "string", "minLength": 1 },
            "minimum": { "type": "number" },
            "maximum": { "type": "number" }
          },
          "anyOf": [
            { "required": ["minimum"] },
            { "required": ["maximum"] }
          ]
        },
        "guardrail": {
          "type": "object",
          "required": ["bound", "breached", "observed_at"],
          "additionalProperties": false,
          "properties": {
            "bound": { "$ref": "#/definitions/bound" },
            "value": { "type": "number" },
            "breached": { "type": "boolean" },
            "observed_at": { "type": "string" }
          }
        },
        "breach": {
          "type": "object",
          "required": ["monitor_id", "solution", "kind", "metric", "value", "observed_at", "retrain_requested"],
          "additionalProperties": false,
          "properties": {
            "monitor_id": { "type": "string" },
            "solution": { "type": "string", "minLength": 1 },
            "kind": { "type": "string", "enum": ["input_psi", "score_psi", "calibration", "guardrail"] },
            "metric": { "type": "string", "minLength": 1 },
            "value": { "type": "number" },
            "minimum": { "type": "number" },
            "maximum": { "type": "number" },
            "observed_at": { "type": "string" },
            "retrain_requested": { "type": "boolean" }
          }
        }
      },
      "required": ["solution", "inputs", "psi_threshold", "calibration_error_threshold", "calibration_bins", "calibration", "cohorts", "guardrails", "breaches", "retrain_on_breach", "retrain_requested", "created", "updated"],
      "additionalProperties": false,
      "properties": {
        "id": { "type": "string" },
        "solution": { "type": "string", "minLength": 1 },
        "inputs": { "type": "array", "items": { "$ref": "#/definitions/distribution" } },
        "scores": { "$ref": "#/definitions/distribution" },
        "psi_threshold": { "type": "number", "exclusiveMinimum": 0 },
        "calibration_error_threshold": { "type": "number", "minimum": 0, "maximum": 1 },
        "calibration_bins": { "type": "integer", "minimum": 1 },
        "calibration": {
          "type": "object",
          "required": ["samples", "expected_calibration_error", "brier_score", "breached", "observed_at"],
          "additionalProperties": false,
          "properties": {
            "samples": { "type": "integer", "minimum": 0 },
            "expected_calibration_error": { "type": "number", "minimum": 0, "maximum": 1 },
            "brier_score": { "type": "number", "minimum": 0, "maximum": 1 },
            "breached": { "type": "boolean" },
            "observed_at": { "type": "string" }
          }
        },
        "cohorts": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["window", "cohort", "samples", "accuracy", "brier_score", "log_loss", "closed_at"],
            "additionalProperties": false,
            "properties": {
              "window": { "type": "string", "minLength": 1 },
              "cohort": { "type": "string", "minLength": 1 },
              "samples": { "type": "integer", "minimum": 1 },
              "accuracy": { "type": "number", "minimum": 0, "maximum": 1 },
              "brier_score": { "type": "number", "minimum": 0, "maximum": 1 },
              "log_loss": { "type": "number", "minimum": 0 },
              "closed_at": { "type": "string" }
            }
          }
        },
        "guardrails": { "type": "array", "items": { "$ref": "#/definitions/guardrail" } },
        "breaches": { "type": "array", "items": { "$ref": "#/definitions/breach" } },
        "retrain_on_breach": { "type": "boolean" },
        "retrain_requested": { "type": "boolean" },
        "created": { "type": "string" },
        "updated": { "type": "string" }
      }
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json&,
               const std::string& message) override {
        if (failed) {
            return;
        }
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }

    bool failed = false;
    std::string reason;
};

bool finite(double value) {
    return std::isfinite(value);
}

bool validate_baseline(DistributionBaseline& baseline,
                       std::string_view path,
                       std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) {
            *error = std::string(path) + ": " + std::move(message);
        }
        return false;
    };
    if (baseline.name.empty()) {
        return fail("name must not be empty");
    }
    if (baseline.expected.empty()) {
        return fail("expected must contain at least one bin");
    }
    if (baseline.boundaries.size() + 1 != baseline.expected.size()) {
        return fail("boundaries must contain one fewer value than expected");
    }
    if (std::ranges::any_of(
            baseline.boundaries,
            [](double value) { return !finite(value); })) {
        return fail("boundaries must be finite");
    }
    if (!std::ranges::is_sorted(baseline.boundaries) ||
        std::adjacent_find(baseline.boundaries.begin(),
                           baseline.boundaries.end()) != baseline.boundaries.end()) {
        return fail("boundaries must be strictly increasing");
    }
    if (std::ranges::any_of(
            baseline.expected,
            [](double value) { return !finite(value) || value < 0.0; })) {
        return fail("expected shares must be finite and non-negative");
    }
    const double total = std::accumulate(
        baseline.expected.begin(),
        baseline.expected.end(),
        0.0);
    if (!(total > 0.0) || !finite(total)) {
        return fail("expected shares must have positive mass");
    }
    for (double& share : baseline.expected) {
        share /= total;
    }
    return true;
}

std::optional<std::vector<double>> distribution_for(
    const DistributionBaseline& baseline,
    std::span<const double> values,
    std::string* error) {
    if (values.empty()) {
        if (error != nullptr) {
            *error = "values must not be empty";
        }
        return std::nullopt;
    }
    std::vector<double> counts(baseline.expected.size(), 0.0);
    for (double value : values) {
        if (!finite(value)) {
            if (error != nullptr) {
                *error = "values must be finite";
            }
            return std::nullopt;
        }
        const auto found = std::lower_bound(
            baseline.boundaries.begin(),
            baseline.boundaries.end(),
            value);
        const std::size_t bin = static_cast<std::size_t>(
            std::distance(baseline.boundaries.begin(), found));
        counts[bin] += 1.0;
    }
    for (double& count : counts) {
        count /= static_cast<double>(values.size());
    }
    return counts;
}

nlohmann::json baseline_json(const DistributionBaseline& baseline) {
    return {
        {"name", baseline.name},
        {"boundaries", baseline.boundaries},
        {"expected", baseline.expected},
    };
}

nlohmann::json distribution_json(const DistributionState& state) {
    nlohmann::json result = baseline_json(state.baseline);
    result["observed"] = state.observed;
    result["psi"] = state.psi;
    result["breached"] = state.breached;
    result["observed_at"] = state.observed_at;
    return result;
}

nlohmann::json bound_json(const MetricBound& bound) {
    nlohmann::json result = {{"name", bound.name}};
    if (bound.minimum) {
        result["minimum"] = *bound.minimum;
    }
    if (bound.maximum) {
        result["maximum"] = *bound.maximum;
    }
    return result;
}

DistributionBaseline parse_baseline(const nlohmann::json& value) {
    DistributionBaseline baseline;
    baseline.name = value.at("name").get<std::string>();
    baseline.boundaries = value.at("boundaries").get<std::vector<double>>();
    baseline.expected = value.at("expected").get<std::vector<double>>();
    return baseline;
}

DistributionState parse_distribution(const nlohmann::json& value) {
    DistributionState state;
    state.baseline = parse_baseline(value);
    state.observed = value.at("observed").get<std::vector<double>>();
    state.psi = value.at("psi").get<double>();
    state.breached = value.at("breached").get<bool>();
    state.observed_at = value.at("observed_at").get<std::string>();
    return state;
}

MetricBound parse_bound(const nlohmann::json& value) {
    MetricBound bound;
    bound.name = value.at("name").get<std::string>();
    if (value.contains("minimum")) {
        bound.minimum = value.at("minimum").get<double>();
    }
    if (value.contains("maximum")) {
        bound.maximum = value.at("maximum").get<double>();
    }
    return bound;
}

MonitorBreach parse_breach(const nlohmann::json& value) {
    MonitorBreach breach;
    breach.monitor_id = value.at("monitor_id").get<std::string>();
    breach.solution = value.at("solution").get<std::string>();
    breach.kind = value.at("kind").get<std::string>();
    breach.metric = value.at("metric").get<std::string>();
    breach.value = value.at("value").get<double>();
    if (value.contains("minimum")) {
        breach.minimum = value.at("minimum").get<double>();
    }
    if (value.contains("maximum")) {
        breach.maximum = value.at("maximum").get<double>();
    }
    breach.observed_at = value.at("observed_at").get<std::string>();
    breach.retrain_requested = value.at("retrain_requested").get<bool>();
    return breach;
}

bool valid_score(double score) {
    return finite(score) && score >= 0.0 && score <= 1.0;
}

}  // namespace

std::optional<double> population_stability_index(
    std::span<const double> expected,
    std::span<const double> observed) {
    if (expected.empty() || expected.size() != observed.size()) {
        return std::nullopt;
    }
    const auto invalid = [](double value) {
        return !finite(value) || value < 0.0;
    };
    if (std::ranges::any_of(expected, invalid) ||
        std::ranges::any_of(observed, invalid)) {
        return std::nullopt;
    }
    const double expected_total = std::accumulate(
        expected.begin(),
        expected.end(),
        0.0);
    const double observed_total = std::accumulate(
        observed.begin(),
        observed.end(),
        0.0);
    if (!(expected_total > 0.0) || !(observed_total > 0.0)) {
        return std::nullopt;
    }
    double result = 0.0;
    for (std::size_t bin = 0; bin < expected.size(); ++bin) {
        const double expected_share = std::max(
            expected[bin] / expected_total,
            kDistributionEpsilon);
        const double observed_share = std::max(
            observed[bin] / observed_total,
            kDistributionEpsilon);
        result += (observed_share - expected_share) *
                  std::log(observed_share / expected_share);
    }
    return result;
}

std::optional<MonitorValue> make_monitor_value(
    MonitorSpec spec,
    std::string created,
    std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<MonitorValue> {
        if (error != nullptr) {
            *error = std::move(reason);
        }
        return std::nullopt;
    };
    if (spec.solution.empty()) {
        return fail("solution: must not be empty");
    }
    if (!finite(spec.psi_threshold) || spec.psi_threshold <= 0.0) {
        return fail("psi_threshold: must be finite and positive");
    }
    if (!finite(spec.calibration_error_threshold) ||
        spec.calibration_error_threshold < 0.0 ||
        spec.calibration_error_threshold > 1.0) {
        return fail("calibration_error_threshold: must be in [0, 1]");
    }
    if (spec.calibration_bins == 0) {
        return fail("calibration_bins: must be positive");
    }

    MonitorValue value;
    value.solution = std::move(spec.solution);
    value.psi_threshold = spec.psi_threshold;
    value.calibration_error_threshold = spec.calibration_error_threshold;
    value.calibration_bins = spec.calibration_bins;
    value.retrain_on_breach = spec.retrain_on_breach;
    value.created = std::move(created);
    value.updated = value.created;

    std::map<std::string, bool> names;
    for (std::size_t index = 0; index < spec.inputs.size(); ++index) {
        DistributionBaseline baseline = std::move(spec.inputs[index]);
        const std::string path = "inputs[" + std::to_string(index) + "]";
        if (!validate_baseline(baseline, path, error)) {
            return std::nullopt;
        }
        if (!names.emplace(baseline.name, true).second) {
            return fail(path + ": duplicate input name '" + baseline.name + "'");
        }
        value.inputs.push_back(DistributionState{.baseline = std::move(baseline)});
    }
    if (spec.scores) {
        DistributionBaseline baseline = std::move(*spec.scores);
        if (!validate_baseline(baseline, "scores", error)) {
            return std::nullopt;
        }
        value.scores = DistributionState{.baseline = std::move(baseline)};
    }

    names.clear();
    for (std::size_t index = 0; index < spec.guardrails.size(); ++index) {
        MetricBound bound = std::move(spec.guardrails[index]);
        const std::string path = "guardrails[" + std::to_string(index) + "]";
        if (bound.name.empty()) {
            return fail(path + ": name must not be empty");
        }
        if (!bound.minimum && !bound.maximum) {
            return fail(path + ": minimum or maximum is required");
        }
        if ((bound.minimum && !finite(*bound.minimum)) ||
            (bound.maximum && !finite(*bound.maximum))) {
            return fail(path + ": bounds must be finite");
        }
        if (bound.minimum && bound.maximum &&
            *bound.minimum > *bound.maximum) {
            return fail(path + ": minimum must not exceed maximum");
        }
        if (!names.emplace(bound.name, true).second) {
            return fail(path + ": duplicate guardrail name '" + bound.name + "'");
        }
        value.guardrails.push_back(GuardrailState{.bound = std::move(bound)});
    }
    return value;
}

nlohmann::json to_json(const MonitorBreach& breach) {
    nlohmann::json result = {
        {"monitor_id", breach.monitor_id},
        {"solution", breach.solution},
        {"kind", breach.kind},
        {"metric", breach.metric},
        {"value", breach.value},
        {"observed_at", breach.observed_at},
        {"retrain_requested", breach.retrain_requested},
    };
    if (breach.minimum) {
        result["minimum"] = *breach.minimum;
    }
    if (breach.maximum) {
        result["maximum"] = *breach.maximum;
    }
    return result;
}

nlohmann::json to_json(const MonitorValue& value) {
    nlohmann::json inputs = nlohmann::json::array();
    for (const DistributionState& input : value.inputs) {
        inputs.push_back(distribution_json(input));
    }
    nlohmann::json cohorts = nlohmann::json::array();
    for (const CohortPerformance& cohort : value.cohorts) {
        cohorts.push_back({
            {"window", cohort.window},
            {"cohort", cohort.cohort},
            {"samples", cohort.samples},
            {"accuracy", cohort.accuracy},
            {"brier_score", cohort.brier_score},
            {"log_loss", cohort.log_loss},
            {"closed_at", cohort.closed_at},
        });
    }
    nlohmann::json guardrails = nlohmann::json::array();
    for (const GuardrailState& guardrail : value.guardrails) {
        nlohmann::json item = {
            {"bound", bound_json(guardrail.bound)},
            {"breached", guardrail.breached},
            {"observed_at", guardrail.observed_at},
        };
        if (guardrail.value) {
            item["value"] = *guardrail.value;
        }
        guardrails.push_back(std::move(item));
    }
    nlohmann::json breaches = nlohmann::json::array();
    for (const MonitorBreach& breach : value.breaches) {
        breaches.push_back(to_json(breach));
    }
    nlohmann::json result = {
        {"solution", value.solution},
        {"inputs", std::move(inputs)},
        {"psi_threshold", value.psi_threshold},
        {"calibration_error_threshold", value.calibration_error_threshold},
        {"calibration_bins", value.calibration_bins},
        {"calibration",
         {{"samples", value.calibration.samples},
          {"expected_calibration_error",
           value.calibration.expected_calibration_error},
          {"brier_score", value.calibration.brier_score},
          {"breached", value.calibration.breached},
          {"observed_at", value.calibration.observed_at}}},
        {"cohorts", std::move(cohorts)},
        {"guardrails", std::move(guardrails)},
        {"breaches", std::move(breaches)},
        {"retrain_on_breach", value.retrain_on_breach},
        {"retrain_requested", value.retrain_requested},
        {"created", value.created},
        {"updated", value.updated},
    };
    if (value.scores) {
        result["scores"] = distribution_json(*value.scores);
    }
    return result;
}

const nlohmann::json& monitor_value_schema() {
    return schema_document();
}

bool validate_monitor_value(const nlohmann::json& value,
                            std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error != nullptr) {
            *error = handler.reason;
        }
        return false;
    }
    return true;
}

std::optional<MonitorValue> parse_monitor_value(
    const nlohmann::json& value,
    std::string* error) {
    if (!validate_monitor_value(value, error)) {
        return std::nullopt;
    }
    MonitorValue parsed;
    parsed.solution = value.at("solution").get<std::string>();
    for (const nlohmann::json& input : value.at("inputs")) {
        parsed.inputs.push_back(parse_distribution(input));
    }
    if (value.contains("scores")) {
        parsed.scores = parse_distribution(value.at("scores"));
    }
    parsed.psi_threshold = value.at("psi_threshold").get<double>();
    parsed.calibration_error_threshold =
        value.at("calibration_error_threshold").get<double>();
    parsed.calibration_bins = value.at("calibration_bins").get<std::size_t>();
    const nlohmann::json& calibration = value.at("calibration");
    parsed.calibration.samples = calibration.at("samples").get<std::size_t>();
    parsed.calibration.expected_calibration_error =
        calibration.at("expected_calibration_error").get<double>();
    parsed.calibration.brier_score =
        calibration.at("brier_score").get<double>();
    parsed.calibration.breached = calibration.at("breached").get<bool>();
    parsed.calibration.observed_at =
        calibration.at("observed_at").get<std::string>();
    for (const nlohmann::json& cohort : value.at("cohorts")) {
        parsed.cohorts.push_back(
            {cohort.at("window").get<std::string>(),
             cohort.at("cohort").get<std::string>(),
             cohort.at("samples").get<std::size_t>(),
             cohort.at("accuracy").get<double>(),
             cohort.at("brier_score").get<double>(),
             cohort.at("log_loss").get<double>(),
             cohort.at("closed_at").get<std::string>()});
    }
    for (const nlohmann::json& guardrail : value.at("guardrails")) {
        GuardrailState state;
        state.bound = parse_bound(guardrail.at("bound"));
        if (guardrail.contains("value")) {
            state.value = guardrail.at("value").get<double>();
        }
        state.breached = guardrail.at("breached").get<bool>();
        state.observed_at = guardrail.at("observed_at").get<std::string>();
        parsed.guardrails.push_back(std::move(state));
    }
    for (const nlohmann::json& breach : value.at("breaches")) {
        parsed.breaches.push_back(parse_breach(breach));
    }
    parsed.retrain_on_breach = value.at("retrain_on_breach").get<bool>();
    parsed.retrain_requested = value.at("retrain_requested").get<bool>();
    parsed.created = value.at("created").get<std::string>();
    parsed.updated = value.at("updated").get<std::string>();

    std::string semantic_error;
    for (DistributionState& input : parsed.inputs) {
        if (!validate_baseline(input.baseline, "inputs", &semantic_error) ||
            (!input.observed.empty() &&
             input.observed.size() != input.baseline.expected.size())) {
            if (error != nullptr) {
                *error = semantic_error.empty()
                             ? "inputs: observed bin count does not match expected"
                             : semantic_error;
            }
            return std::nullopt;
        }
    }
    if (parsed.scores &&
        (!validate_baseline(parsed.scores->baseline,
                            "scores",
                            &semantic_error) ||
         (!parsed.scores->observed.empty() &&
          parsed.scores->observed.size() !=
              parsed.scores->baseline.expected.size()))) {
        if (error != nullptr) {
            *error = semantic_error.empty()
                         ? "scores: observed bin count does not match expected"
                         : semantic_error;
        }
        return std::nullopt;
    }
    return parsed;
}

Monitor::Monitor(std::string id,
                 MonitorValue value,
                 fs::path dir,
                 zetta::cluster::ReplicationManager* replication,
                 BreachSink breach_sink)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication),
      breach_sink_(std::move(breach_sink)) {}

MonitorUpdate Monitor::observe_input(std::string_view name,
                                     std::span<const double> values) {
    const auto found = std::find_if(
        value_.inputs.begin(),
        value_.inputs.end(),
        [&](const DistributionState& state) {
            return state.baseline.name == name;
        });
    if (found == value_.inputs.end()) {
        return {.error = "no input baseline named '" + std::string(name) + "'"};
    }
    return update_distribution(*found, values, "input_psi");
}

MonitorUpdate Monitor::observe_scores(std::span<const double> scores) {
    if (!value_.scores) {
        return {.error = "this monitor has no score-distribution baseline"};
    }
    return update_distribution(*value_.scores, scores, "score_psi");
}

MonitorUpdate Monitor::update_distribution(
    DistributionState& state,
    std::span<const double> values,
    std::string_view kind) {
    std::string error;
    const std::optional<std::vector<double>> observed =
        distribution_for(state.baseline, values, &error);
    if (!observed) {
        return {.error = std::move(error)};
    }
    const std::optional<double> psi = population_stability_index(
        state.baseline.expected,
        *observed);
    if (!psi) {
        return {.error = "could not compute PSI from the distributions"};
    }
    const bool was_breached = state.breached;
    state.observed = *observed;
    state.psi = *psi;
    state.breached = state.psi > value_.psi_threshold;
    state.observed_at = zetta::util::now_iso8601();
    MonitorBreach breach;
    breach.monitor_id = id_;
    breach.solution = value_.solution;
    breach.kind = std::string(kind);
    breach.metric = state.baseline.name;
    breach.value = state.psi;
    breach.maximum = value_.psi_threshold;
    breach.observed_at = state.observed_at;
    return finish_update(was_breached, std::move(breach));
}

MonitorUpdate Monitor::observe_calibration(
    std::span<const ScoredLabel> labels) {
    if (labels.empty()) {
        return {.error = "labels must not be empty"};
    }
    std::vector<double> score_sum(value_.calibration_bins, 0.0);
    std::vector<double> label_sum(value_.calibration_bins, 0.0);
    std::vector<std::size_t> counts(value_.calibration_bins, 0);
    double brier = 0.0;
    for (const ScoredLabel& label : labels) {
        if (!valid_score(label.score)) {
            return {.error = "scores must be finite probabilities in [0, 1]"};
        }
        const std::size_t bin = std::min(
            static_cast<std::size_t>(
                label.score * static_cast<double>(value_.calibration_bins)),
            value_.calibration_bins - 1);
        score_sum[bin] += label.score;
        label_sum[bin] += label.label ? 1.0 : 0.0;
        ++counts[bin];
        const double difference = label.score - (label.label ? 1.0 : 0.0);
        brier += difference * difference;
    }
    double calibration_error = 0.0;
    for (std::size_t bin = 0; bin < counts.size(); ++bin) {
        if (counts[bin] == 0) {
            continue;
        }
        const double count = static_cast<double>(counts[bin]);
        calibration_error +=
            (count / static_cast<double>(labels.size())) *
            std::abs(score_sum[bin] / count - label_sum[bin] / count);
    }
    const bool was_breached = value_.calibration.breached;
    value_.calibration.samples = labels.size();
    value_.calibration.expected_calibration_error = calibration_error;
    value_.calibration.brier_score =
        brier / static_cast<double>(labels.size());
    value_.calibration.breached =
        calibration_error > value_.calibration_error_threshold;
    value_.calibration.observed_at = zetta::util::now_iso8601();
    MonitorBreach breach;
    breach.monitor_id = id_;
    breach.solution = value_.solution;
    breach.kind = "calibration";
    breach.metric = "expected_calibration_error";
    breach.value = calibration_error;
    breach.maximum = value_.calibration_error_threshold;
    breach.observed_at = value_.calibration.observed_at;
    return finish_update(was_breached, std::move(breach));
}

MonitorUpdate Monitor::close_label_window(
    std::string window,
    std::span<const ScoredLabel> labels) {
    if (window.empty()) {
        return {.error = "window must not be empty"};
    }
    if (labels.empty()) {
        return {.error = "labels must not be empty"};
    }
    std::map<std::string, std::vector<const ScoredLabel*>> grouped;
    for (const ScoredLabel& label : labels) {
        if (!valid_score(label.score)) {
            return {.error = "scores must be finite probabilities in [0, 1]"};
        }
        grouped[label.cohort.empty() ? "all" : label.cohort].push_back(&label);
    }
    const std::string closed_at = zetta::util::now_iso8601();
    for (const auto& [cohort_name, cohort_labels] : grouped) {
        double correct = 0.0;
        double brier = 0.0;
        double log_loss = 0.0;
        for (const ScoredLabel* label : cohort_labels) {
            const bool predicted = label->score >= 0.5;
            if (predicted == label->label) {
                correct += 1.0;
            }
            const double outcome = label->label ? 1.0 : 0.0;
            const double difference = label->score - outcome;
            brier += difference * difference;
            const double probability = std::clamp(
                label->score,
                kProbabilityEpsilon,
                1.0 - kProbabilityEpsilon);
            log_loss -= outcome * std::log(probability) +
                        (1.0 - outcome) * std::log(1.0 - probability);
        }
        const double count = static_cast<double>(cohort_labels.size());
        CohortPerformance performance{
            window,
            cohort_name,
            cohort_labels.size(),
            correct / count,
            brier / count,
            log_loss / count,
            closed_at,
        };
        const auto existing = std::find_if(
            value_.cohorts.begin(),
            value_.cohorts.end(),
            [&](const CohortPerformance& item) {
                return item.window == window && item.cohort == cohort_name;
            });
        if (existing == value_.cohorts.end()) {
            value_.cohorts.push_back(std::move(performance));
        } else {
            *existing = std::move(performance);
        }
    }
    capture_and_persist();
    return {
        .ok = true,
        .retrain_requested = value_.retrain_requested,
    };
}

MonitorUpdate Monitor::observe_guardrail(std::string_view name, double value) {
    if (!finite(value)) {
        return {.error = "guardrail value must be finite"};
    }
    const auto found = std::find_if(
        value_.guardrails.begin(),
        value_.guardrails.end(),
        [&](const GuardrailState& state) {
            return state.bound.name == name;
        });
    if (found == value_.guardrails.end()) {
        return {.error = "no guardrail bound named '" + std::string(name) + "'"};
    }
    const bool was_breached = found->breached;
    found->value = value;
    found->breached =
        (found->bound.minimum && value < *found->bound.minimum) ||
        (found->bound.maximum && value > *found->bound.maximum);
    found->observed_at = zetta::util::now_iso8601();
    MonitorBreach breach;
    breach.monitor_id = id_;
    breach.solution = value_.solution;
    breach.kind = "guardrail";
    breach.metric = found->bound.name;
    breach.value = value;
    breach.minimum = found->bound.minimum;
    breach.maximum = found->bound.maximum;
    breach.observed_at = found->observed_at;
    return finish_update(was_breached, std::move(breach));
}

MonitorUpdate Monitor::finish_update(bool was_breached,
                                     MonitorBreach breach) {
    const bool is_breached = std::ranges::any_of(
        value_.inputs,
        [&](const DistributionState& state) {
            return breach.kind == "input_psi" &&
                   state.baseline.name == breach.metric &&
                   state.breached;
        }) ||
        (value_.scores &&
         breach.kind == "score_psi" &&
         value_.scores->baseline.name == breach.metric &&
         value_.scores->breached) ||
        (breach.kind == "calibration" && value_.calibration.breached) ||
        std::ranges::any_of(
            value_.guardrails,
            [&](const GuardrailState& state) {
                return breach.kind == "guardrail" &&
                       state.bound.name == breach.metric &&
                       state.breached;
            });
    if (!was_breached && is_breached && value_.retrain_on_breach) {
        value_.retrain_requested = true;
    }
    rebuild_breaches();
    capture_and_persist();

    MonitorUpdate result{
        .ok = true,
        .retrain_requested = value_.retrain_requested,
    };
    if (!was_breached && is_breached) {
        const auto active = std::find_if(
            value_.breaches.begin(),
            value_.breaches.end(),
            [&](const MonitorBreach& item) {
                return item.kind == breach.kind && item.metric == breach.metric;
            });
        if (active != value_.breaches.end()) {
            result.new_breaches.push_back(*active);
            if (breach_sink_) {
                breach_sink_(*active);
            }
        }
    }
    return result;
}

void Monitor::rebuild_breaches() {
    value_.breaches.clear();
    const auto add_distribution = [&](const DistributionState& state,
                                      std::string kind) {
        if (!state.breached) {
            return;
        }
        value_.breaches.push_back(
            {id_,
             value_.solution,
             std::move(kind),
             state.baseline.name,
             state.psi,
             std::nullopt,
             value_.psi_threshold,
             state.observed_at,
             value_.retrain_requested});
    };
    for (const DistributionState& input : value_.inputs) {
        add_distribution(input, "input_psi");
    }
    if (value_.scores) {
        add_distribution(*value_.scores, "score_psi");
    }
    if (value_.calibration.breached) {
        value_.breaches.push_back(
            {id_,
             value_.solution,
             "calibration",
             "expected_calibration_error",
             value_.calibration.expected_calibration_error,
             std::nullopt,
             value_.calibration_error_threshold,
             value_.calibration.observed_at,
             value_.retrain_requested});
    }
    for (const GuardrailState& guardrail : value_.guardrails) {
        if (!guardrail.breached || !guardrail.value) {
            continue;
        }
        value_.breaches.push_back(
            {id_,
             value_.solution,
             "guardrail",
             guardrail.bound.name,
             *guardrail.value,
             guardrail.bound.minimum,
             guardrail.bound.maximum,
             guardrail.observed_at,
             value_.retrain_requested});
    }
}

void Monitor::acknowledge_retrain() {
    if (!value_.retrain_requested) {
        return;
    }
    value_.retrain_requested = false;
    rebuild_breaches();
    capture_and_persist();
}

void Monitor::apply_value(MonitorValue value) {
    value_ = std::move(value);
    persist_meta();
}

void Monitor::capture_and_persist() {
    value_.updated = zetta::util::now_iso8601();
    if (replication_ != nullptr) {
        replication_->set(catalog::monitor_key(id_), metadata());
    }
    persist_meta();
}

nlohmann::json Monitor::metadata() const {
    return to_json(value_);
}

void Monitor::persist_meta() const {
    std::error_code error;
    fs::create_directories(dir_, error);
    nlohmann::json document = metadata();
    document["id"] = id_;
    std::ofstream output(dir_ / "monitor.json", std::ios::trunc);
    output << document.dump(2) << '\n';
}

}  // namespace zetta::solutions
