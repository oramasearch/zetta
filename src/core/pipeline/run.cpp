#include "zetta/pipeline/run.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/util/timestamp.hpp"

#include "../catalog.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <mutex>
#include <numeric>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::pipeline {
namespace fs = std::filesystem;

namespace {

// Keep a confidence score inside the [0, 1] range the schema (and the lineage
// contract) require, so our own writes always round-trip through validation.
double clamp_confidence(double confidence) {
    return std::clamp(confidence, 0.0, 1.0);
}

constexpr std::size_t kMetricPointsCap = 10'000;
constexpr std::size_t kMetricPointsAfterCompaction = 5'000;

bool lower_is_better(std::string_view name) {
    static constexpr std::string_view fragments[] = {
        "loss", "error", "rmse", "mae", "mape", "latency", "perplexity"};
    return std::ranges::any_of(fragments, [&](std::string_view fragment) {
        return name.find(fragment) != std::string_view::npos;
    });
}

std::vector<std::size_t> lttb_indices(const std::vector<double>& values,
                                      std::size_t threshold) {
    const std::size_t count = values.size();
    if (threshold >= count || threshold == 0) {
        std::vector<std::size_t> all(count);
        std::iota(all.begin(), all.end(), 0);
        return all;
    }
    if (threshold < 3) {
        return {0, count - 1};
    }

    std::vector<std::size_t> selected;
    selected.reserve(threshold);
    selected.push_back(0);
    const double bucket_width =
        static_cast<double>(count - 2) / static_cast<double>(threshold - 2);
    std::size_t anchor = 0;
    for (std::size_t bucket = 0; bucket < threshold - 2; ++bucket) {
        const std::size_t average_begin = std::min(
            count - 1,
            static_cast<std::size_t>(std::floor((bucket + 1) * bucket_width)) + 1);
        const std::size_t average_end = std::min(
            count,
            static_cast<std::size_t>(std::floor((bucket + 2) * bucket_width)) + 1);
        double average_x = 0.0;
        double average_y = 0.0;
        const std::size_t average_count =
            std::max<std::size_t>(1, average_end - average_begin);
        for (std::size_t index = average_begin; index < average_end; ++index) {
            average_x += static_cast<double>(index);
            average_y += values[index];
        }
        average_x /= static_cast<double>(average_count);
        average_y /= static_cast<double>(average_count);

        const std::size_t range_begin = std::min(
            count - 1,
            static_cast<std::size_t>(std::floor(bucket * bucket_width)) + 1);
        const std::size_t range_end = std::min(
            count - 1,
            static_cast<std::size_t>(std::floor((bucket + 1) * bucket_width)) + 1);
        double largest_area = -1.0;
        std::size_t next_anchor = range_begin;
        for (std::size_t index = range_begin; index <= range_end; ++index) {
            const double area = std::abs(
                (static_cast<double>(anchor) - average_x) *
                    (values[index] - values[anchor]) -
                (static_cast<double>(anchor) - static_cast<double>(index)) *
                    (average_y - values[anchor]));
            if (area > largest_area) {
                largest_area = area;
                next_anchor = index;
            }
        }
        selected.push_back(next_anchor);
        anchor = next_anchor;
    }
    selected.push_back(count - 1);
    return selected;
}

std::vector<std::size_t> envelope_lttb_indices(
    const std::vector<double>& values, std::size_t threshold) {
    if (values.size() <= threshold) {
        return lttb_indices(values, values.size());
    }
    const auto minimum = static_cast<std::size_t>(
        std::distance(values.begin(), std::ranges::min_element(values)));
    const auto maximum = static_cast<std::size_t>(
        std::distance(values.begin(), std::ranges::max_element(values)));
    const std::set<std::size_t> required = {0, values.size() - 1, minimum, maximum};
    std::set<std::size_t> selected(required);
    for (const std::size_t index : lttb_indices(values, threshold)) {
        selected.insert(index);
    }
    while (selected.size() > threshold) {
        auto removable = std::find_if(selected.rbegin(), selected.rend(),
                                      [&](std::size_t index) {
                                          return !required.contains(index);
                                      });
        if (removable == selected.rend()) {
            break;
        }
        selected.erase(*removable);
    }
    if (selected.size() < threshold) {
        for (std::size_t index = 0;
             index < values.size() && selected.size() < threshold; ++index) {
            selected.insert(index);
        }
    }
    return {selected.begin(), selected.end()};
}

void retain_indices(nlohmann::json& series,
                    const std::vector<std::size_t>& indices) {
    nlohmann::json steps = nlohmann::json::array();
    nlohmann::json values = nlohmann::json::array();
    nlohmann::json times = nlohmann::json::array();
    for (const std::size_t index : indices) {
        steps.push_back(series["steps"][index]);
        values.push_back(series["values"][index]);
        times.push_back(series["times"][index]);
    }
    series["steps"] = std::move(steps);
    series["values"] = std::move(values);
    series["times"] = std::move(times);
}

bool validate_metric_columns(const nlohmann::json& metrics,
                             std::string* error) {
    for (const auto& [name, series] : metrics.at("series").items()) {
        const std::size_t steps = series.at("steps").size();
        if (series.at("values").size() != steps ||
            series.at("times").size() != steps) {
            if (error != nullptr) {
                *error = "/metrics/series/" + name +
                         ": steps, values and times must have equal lengths";
            }
            return false;
        }
    }
    return true;
}

// The Run JSON Schema (draft-07). One declarative source of truth for the value
// shape carried by run.json (with an optional `id`) and the `run:<uuid>` CRDT
// value (without it). `status`/`kind` are closed enums and `confidence` is
// bounded, so malformed records fail validation at the boundary.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.Run",
      "type": "object",
      "properties": {
        "id":         { "type": "string" },
        "project":    { "type": "string" },
        "graph":      { "type": "string" },
        "deployment": { "type": "string" },
        "status":  { "type": "string",
                     "enum": ["pending", "running", "succeeded", "failed"] },
        "created": { "type": "string" },
        "updated": { "type": "string" },
        "trace_id": { "type": "string" },
        "metrics_suppressed": { "type": "string",
                                "enum": ["differential_privacy"] },
        "features": {
          "type": "object",
          "required": ["used", "excluded"],
          "additionalProperties": false,
          "properties": {
            "used": { "type": "array", "items": {
              "type": "object", "required": ["name", "source", "transform", "why"],
              "additionalProperties": false,
              "properties": {
                "name": { "type": "string", "minLength": 1 },
                "source": { "type": "array", "minItems": 1,
                            "items": { "type": "string", "minLength": 1 } },
                "transform": { "type": "string", "minLength": 1 },
                "why": { "type": "string", "minLength": 1 }
              }
            } },
            "excluded": { "type": "array", "items": {
              "type": "object", "required": ["name", "why"],
              "additionalProperties": false,
              "properties": {
                "name": { "type": "string", "minLength": 1 },
                "why": { "type": "string", "minLength": 1 }
              }
            } }
          }
        },
        "metrics": {
          "type": "object",
          "required": ["params", "series", "summary", "artifacts"],
          "additionalProperties": false,
          "properties": {
            "params": {
              "type": "object",
              "patternProperties": {
                "^[A-Za-z0-9_.-]{1,128}$": {
                  "type": ["string", "number", "integer", "boolean", "null"]
                }
              },
              "additionalProperties": false
            },
            "series": {
              "type": "object",
              "patternProperties": {
                "^[A-Za-z0-9_.-]{1,128}$": {
                  "type": "object",
                  "required": ["steps", "values", "times"],
                  "additionalProperties": false,
                  "properties": {
                    "steps": {"type": "array", "items": {"type": "integer", "minimum": 0}},
                    "values": {"type": "array", "items": {"type": "number"}},
                    "times": {"type": "array", "items": {"type": "string"}}
                  }
                }
              },
              "additionalProperties": false
            },
            "summary": {
              "type": "object",
              "patternProperties": {
                "^[A-Za-z0-9_.-]{1,128}$": {
                  "type": "object",
                  "required": ["final", "final_step", "best", "best_step", "direction"],
                  "additionalProperties": false,
                  "properties": {
                    "final": {"type": "number"},
                    "final_step": {"type": "integer", "minimum": 0},
                    "best": {"type": "number"},
                    "best_step": {"type": "integer", "minimum": 0},
                    "direction": {"type": "string", "enum": ["min", "max"]}
                  }
                }
              },
              "additionalProperties": false
            },
            "artifacts": {
              "type": "array",
              "items": {
                "type": "object",
                "required": ["type", "name", "time", "data"],
                "additionalProperties": false,
                "properties": {
                  "type": {"type": "string", "enum": ["confusion_matrix", "roc_curve", "pr_curve", "calibration", "feature_importance", "sample_predictions"]},
                  "name": {"type": "string", "minLength": 1},
                  "time": {"type": "string"},
                  "data": {"type": "object"}
                },
                "allOf": [
                  {
                    "if": {"properties": {"type": {"const": "confusion_matrix"}}},
                    "then": {"properties": {"data": {
                      "required": ["labels", "matrix"],
                      "additionalProperties": false,
                      "properties": {
                        "labels": {"type": "array", "items": {"type": "string"}},
                        "matrix": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}}
                      }
                    }}}
                  },
                  {
                    "if": {"properties": {"type": {"enum": ["roc_curve", "pr_curve"]}}},
                    "then": {"properties": {"data": {
                      "required": ["x", "y", "thresholds"],
                      "additionalProperties": false,
                      "properties": {
                        "x": {"type": "array", "items": {"type": "number"}},
                        "y": {"type": "array", "items": {"type": "number"}},
                        "thresholds": {"type": "array", "items": {"type": "number"}}
                      }
                    }}}
                  },
                  {
                    "if": {"properties": {"type": {"const": "calibration"}}},
                    "then": {"properties": {"data": {
                      "required": ["predicted", "observed", "counts"],
                      "additionalProperties": false,
                      "properties": {
                        "predicted": {"type": "array", "items": {"type": "number"}},
                        "observed": {"type": "array", "items": {"type": "number"}},
                        "counts": {"type": "array", "items": {"type": "integer", "minimum": 0}}
                      }
                    }}}
                  },
                  {
                    "if": {"properties": {"type": {"const": "feature_importance"}}},
                    "then": {"properties": {"data": {
                      "required": ["features", "values"],
                      "additionalProperties": false,
                      "properties": {
                        "features": {"type": "array", "items": {"type": "string"}},
                        "values": {"type": "array", "items": {"type": "number"}}
                      }
                    }}}
                  },
                  {
                    "if": {"properties": {"type": {"const": "sample_predictions"}}},
                    "then": {"properties": {"data": {
                      "required": ["rows"],
                      "additionalProperties": false,
                      "properties": {
                        "rows": {"type": "array", "items": {
                          "type": "object",
                          "patternProperties": {"^.{1,128}$": {"type": ["string", "number", "integer", "boolean", "null"]}},
                          "additionalProperties": false
                        }}
                      }
                    }}}
                  }
                ]
              }
            }
          }
        },
        "logs": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "time":    { "type": "string" },
              "level":   { "type": "string" },
              "message": { "type": "string" }
            },
            "required": ["message"],
            "additionalProperties": false
          }
        },
        "outputs": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "kind": { "type": "string",
                        "enum": ["dataset", "model", "index", "report"] },
              "name": { "type": "string" },
              "ref":  { "type": "string" }
            },
            "required": ["kind"],
            "additionalProperties": false
          }
        },
        "lineage": {
          "type": "object",
          "properties": {
            "sources":    { "type": "array", "items": { "type": "string" } },
            "steps":      { "type": "array", "items": { "type": "string" } },
            "freshness":  { "type": "string" },
            "confidence": { "type": "number", "minimum": 0, "maximum": 1 }
          },
          "additionalProperties": false
        }
      },
      "required": ["status"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
// validate() is then cheap and runs at every boundary.
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. Using the error-handler overload of
// validate() (rather than the throwing one) keeps validation failures from
// crossing the validator-library boundary as exceptions — the library's
// recommended path, and robust regardless of how each side is compiled.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

std::string_view to_string(RunStatus status) {
    switch (status) {
        case RunStatus::Pending:   return "pending";
        case RunStatus::Running:   return "running";
        case RunStatus::Succeeded: return "succeeded";
        case RunStatus::Failed:    return "failed";
    }
    return "pending";
}

std::optional<RunStatus> run_status_from_string(std::string_view name) {
    if (name == "pending")   return RunStatus::Pending;
    if (name == "running")   return RunStatus::Running;
    if (name == "succeeded") return RunStatus::Succeeded;
    if (name == "failed")    return RunStatus::Failed;
    return std::nullopt;
}

nlohmann::json to_json(const RunValue& value) {
    nlohmann::json logs = nlohmann::json::array();
    for (const RunLogEntry& entry : value.logs) {
        logs.push_back({{"time", entry.time},
                        {"level", entry.level},
                        {"message", entry.message}});
    }
    nlohmann::json outputs = nlohmann::json::array();
    for (const RunOutput& output : value.outputs) {
        outputs.push_back({{"kind", output.kind},
                           {"name", output.name},
                           {"ref", output.ref}});
    }
    nlohmann::json output{
        {"project", value.project},
        {"graph", value.graph},
        {"deployment", value.deployment},
        {"status", to_string(value.status)},
        {"created", value.created},
        {"updated", value.updated},
        {"trace_id", value.trace_id},
        {"features", value.features},
        {"metrics", value.metrics},
        {"logs", std::move(logs)},
        {"outputs", std::move(outputs)},
        {"lineage",
         {{"sources", value.lineage.sources},
          {"steps", value.lineage.steps},
          {"freshness", value.lineage.freshness},
          {"confidence", clamp_confidence(value.lineage.confidence)}}}};
    if (!value.metrics_suppressed.empty()) {
        output["metrics_suppressed"] = value.metrics_suppressed;
    }
    return output;
}

const nlohmann::json& run_value_schema() { return schema_document(); }

const nlohmann::json& run_features_schema() {
    return schema_document().at("properties").at("features");
}

bool validate_run_value(const nlohmann::json& value, std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<RunValue> parse_run_value(const nlohmann::json& value,
                                        std::string* error) {
    if (!validate_run_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the status/kind enums, so the
    // extraction below is total: missing optional fields fall back to defaults.
    RunValue parsed;
    parsed.project = value.value("project", std::string{});
    parsed.graph = value.value("graph", std::string{});
    parsed.deployment = value.value("deployment", std::string{});
    parsed.status =
        run_status_from_string(value.value("status", std::string{"pending"}))
            .value_or(RunStatus::Pending);
    parsed.created = value.value("created", std::string{});
    parsed.updated = value.value("updated", std::string{});
    parsed.trace_id = value.value("trace_id", std::string{});
    parsed.features = value.value("features", parsed.features);
    parsed.metrics = value.value("metrics", parsed.metrics);
    parsed.metrics_suppressed =
        value.value("metrics_suppressed", std::string{});
    if (!validate_metric_columns(parsed.metrics, error)) {
        return std::nullopt;
    }

    if (const auto logs = value.find("logs"); logs != value.end()) {
        for (const auto& entry : *logs) {
            parsed.logs.push_back({entry.value("time", std::string{}),
                                   entry.value("level", std::string{}),
                                   entry.value("message", std::string{})});
        }
    }
    if (const auto outputs = value.find("outputs"); outputs != value.end()) {
        for (const auto& output : *outputs) {
            parsed.outputs.push_back({output.value("kind", std::string{}),
                                      output.value("name", std::string{}),
                                      output.value("ref", std::string{})});
        }
    }
    if (const auto lineage = value.find("lineage"); lineage != value.end()) {
        parsed.lineage.sources =
            lineage->value("sources", std::vector<std::string>{});
        parsed.lineage.steps =
            lineage->value("steps", std::vector<std::string>{});
        parsed.lineage.freshness = lineage->value("freshness", std::string{});
        parsed.lineage.confidence =
            clamp_confidence(lineage->value("confidence", 0.0));
    }
    return parsed;
}

// ----- Run -------------------------------------------------------------------

Run::Run(std::string id, RunValue value, fs::path dir,
         zetta::cluster::ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {
    value_.lineage.confidence = clamp_confidence(value_.lineage.confidence);
}

void Run::set_status(RunStatus status) {
    value_.status = status;
    capture_and_persist();
}

void Run::add_log(RunLogEntry entry) {
    value_.logs.push_back(std::move(entry));
    capture_and_persist();
}

void Run::set_outputs(std::vector<RunOutput> outputs) {
    value_.outputs = std::move(outputs);
    capture_and_persist();
}

void Run::set_lineage(RunLineage lineage) {
    lineage.confidence = clamp_confidence(lineage.confidence);
    value_.lineage = std::move(lineage);
    capture_and_persist();
}

void Run::set_trace_id(std::string trace_id) {
    value_.trace_id = std::move(trace_id);
    capture_and_persist();
}

bool Run::set_features(nlohmann::json features, std::string* error) {
    auto candidate = metadata();
    candidate["features"] = features;
    if (!validate_run_value(candidate, error)) {
        return false;
    }
    value_.features = std::move(features);
    capture_and_persist();
    return true;
}

void Run::suppress_metrics_for_privacy() {
    value_.metrics_suppressed = "differential_privacy";
    capture_and_persist();
}

bool Run::log_metrics(std::uint64_t step,
                      const std::map<std::string, double>& values,
                      std::string time, std::string* error) {
    if (values.empty()) {
        if (error != nullptr) {
            *error = "/values: at least one metric is required";
        }
        return false;
    }
    if (time.empty()) {
        time = zetta::util::now_iso8601();
    }
    nlohmann::json candidate = value_.metrics;
    for (const auto& [name, value] : values) {
        if (name.empty() || !std::isfinite(value)) {
            if (error != nullptr) {
                *error = name.empty() ? "/values: metric names may not be empty"
                                      : "/values/" + name + ": value must be finite";
            }
            return false;
        }
        nlohmann::json& series = candidate["series"][name];
        if (series.is_null()) {
            series = {{"steps", nlohmann::json::array()},
                      {"values", nlohmann::json::array()},
                      {"times", nlohmann::json::array()}};
        }
        if (!series["steps"].empty() &&
            step <= series["steps"].back().get<std::uint64_t>()) {
            if (error != nullptr) {
                *error = "/step: must be greater than the last step for metric '" +
                         name + "'";
            }
            return false;
        }
        series["steps"].push_back(step);
        series["values"].push_back(value);
        series["times"].push_back(time);
        nlohmann::json& summary = candidate["summary"][name];
        const bool minimize = lower_is_better(name);
        if (summary.is_null()) {
            summary = {{"final", value},
                       {"final_step", step},
                       {"best", value},
                       {"best_step", step},
                       {"direction", minimize ? "min" : "max"}};
        } else {
            summary["final"] = value;
            summary["final_step"] = step;
            const double best = summary["best"].get<double>();
            if ((minimize && value < best) || (!minimize && value > best)) {
                summary["best"] = value;
                summary["best_step"] = step;
            }
        }
        if (series["values"].size() > kMetricPointsCap) {
            const std::vector<double> stored =
                series["values"].get<std::vector<double>>();
            retain_indices(series, envelope_lttb_indices(
                                       stored, kMetricPointsAfterCompaction));
        }
    }
    nlohmann::json run_candidate = metadata();
    run_candidate["metrics"] = candidate;
    if (!validate_run_value(run_candidate, error) ||
        !validate_metric_columns(candidate, error)) {
        return false;
    }
    value_.metrics = std::move(candidate);
    capture_and_persist();
    return true;
}

bool Run::log_params(nlohmann::json params, std::string* error) {
    if (!params.is_object() || params.empty()) {
        if (error != nullptr) {
            *error = "/params: a non-empty object is required";
        }
        return false;
    }
    if (!value_.metrics.at("params").empty()) {
        if (error != nullptr) {
            *error = "/params: run parameters have already been recorded";
        }
        return false;
    }
    nlohmann::json run_candidate = metadata();
    run_candidate["metrics"]["params"] = params;
    if (!validate_run_value(run_candidate, error)) {
        return false;
    }
    value_.metrics["params"] = std::move(params);
    capture_and_persist();
    return true;
}

bool Run::log_artifact(nlohmann::json artifact, std::string* error) {
    if (!artifact.is_object()) {
        if (error != nullptr) {
            *error = "/artifact: an object is required";
        }
        return false;
    }
    if (!artifact.contains("time")) {
        artifact["time"] = zetta::util::now_iso8601();
    }
    nlohmann::json run_candidate = metadata();
    run_candidate["metrics"]["artifacts"].push_back(artifact);
    if (!validate_run_value(run_candidate, error)) {
        return false;
    }
    value_.metrics["artifacts"].push_back(std::move(artifact));
    capture_and_persist();
    return true;
}

nlohmann::json Run::get_metrics(
    const std::vector<std::string>& names, std::uint64_t from_step,
    std::optional<std::size_t> max_points) const {
    const std::set<std::string> selected_names(names.begin(), names.end());
    nlohmann::json series_output = nlohmann::json::array();
    nlohmann::json summary_output = nlohmann::json::object();
    for (const auto& [name, stored] : value_.metrics.at("series").items()) {
        if (!selected_names.empty() && !selected_names.contains(name)) {
            continue;
        }
        std::vector<std::uint64_t> steps;
        std::vector<double> values;
        std::vector<std::string> times;
        for (std::size_t index = 0; index < stored.at("steps").size(); ++index) {
            const std::uint64_t step = stored.at("steps")[index].get<std::uint64_t>();
            if (step < from_step) {
                continue;
            }
            steps.push_back(step);
            values.push_back(stored.at("values")[index].get<double>());
            times.push_back(stored.at("times")[index].get<std::string>());
        }
        std::vector<std::size_t> indices(values.size());
        std::iota(indices.begin(), indices.end(), 0);
        if (max_points && values.size() > *max_points) {
            indices = envelope_lttb_indices(values, *max_points);
        }
        nlohmann::json points = nlohmann::json::array();
        for (const std::size_t index : indices) {
            points.push_back({{"step", steps[index]},
                              {"value", values[index]},
                              {"time", times[index]}});
        }
        series_output.push_back({{"name", name}, {"points", std::move(points)}});
        summary_output[name] = value_.metrics.at("summary").at(name);
    }
    nlohmann::json output = {{"run", id_},
            {"params", value_.metrics.at("params")},
            {"summary", std::move(summary_output)},
            {"artifacts", value_.metrics.at("artifacts")},
            {"series", std::move(series_output)}};
    if (!value_.metrics_suppressed.empty()) {
        output["metrics_suppressed"] = value_.metrics_suppressed;
    }
    return output;
}

void Run::apply_value(RunValue value) {
    value.lineage.confidence = clamp_confidence(value.lineage.confidence);
    value_ = std::move(value);
    persist_meta();  // projection only — no capture (avoids a feedback loop)
}

void Run::capture_and_persist() {
    value_.updated = zetta::util::now_iso8601();
    // CRDT first (the source of truth), then the on-disk projection.
    if (replication_) replication_->set(catalog::run_key(id_), metadata());
    persist_meta();
}

nlohmann::json Run::metadata() const { return to_json(value_); }

void Run::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "run.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::pipeline
