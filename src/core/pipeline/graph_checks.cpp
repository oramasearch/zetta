#include "zetta/pipeline/graph_checks.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/solutions/snapshot.hpp"

namespace zetta::pipeline {

// The step-annotation JSON Schema (draft-07) — the code-owned source of truth
// for the shape of the correctness annotations both checks read out of a
// node's free-form `attributes` bag. Only the named fields are constrained —
// a node may carry any other attributes (`source`, `table`, …) — so a step
// that uses none of these is valid trivially.
// schemas/graph_step_annotations.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& graph_step_annotations_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/graph_step_annotations.schema.json",
  "title": "zetta.GraphStepAnnotations",
  "description": "The correctness annotations read by the gate's concrete checks: schema profiles, point-in-time metadata, and M-10 grounding and read-authorization requirements, plus streaming event/label ordering. Only the named fields are constrained; a node may carry any other attributes.",
  "type": "object",
  "properties": {
    "schema": {
      "description": "The fields this step OUTPUTS and their types — a source profile for a Connect step, the transformed shape for a Clean step. Field name -> type token (an opaque string, compared case-insensitively).",
      "type": "object",
      "additionalProperties": { "type": "string", "minLength": 1 }
    },
    "requires": {
      "description": "The fields this step EXPECTS from its data-flow inputs and the type each must have. Each is checked against the merged `schema` of the step's transitive data-flow ancestors: a missing field or a type mismatch fails the schema_type check.",
      "type": "object",
      "additionalProperties": { "type": "string", "minLength": 1 }
    },
    "as_of": {
      "description": "The point-in-time cutoff for an ML step: every feature must be computed only from data available at or before this instant. ISO-8601 UTC date or date-time (e.g. \"2024-01-01\" or \"2024-01-01T00:00:00Z\"). Required when `features` is present.",
      "type": "string",
      "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"
    },
    "source_kind": {
      "description": "Whether a terminal feature source is an immutable point-in-time snapshot or a live table. Feature lineage accepts only snapshot.",
      "type": "string",
      "enum": ["snapshot", "live"]
    },
    "snapshot_as_of": {
      "description": "The inclusive cutoff used to materialize a terminal snapshot source.",
      "type": "string",
      "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"
    },
    "feature_window_end": {
      "description": "The final instant any feature window may observe; it must not be after prediction as_of.",
      "type": "string",
      "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"
    },
    "label_window_start": {
      "description": "The first instant from which labels may be observed.",
      "type": "string",
      "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"
    },
    "blackout_gap_seconds": {
      "description": "Minimum interval between feature_window_end and label_window_start.",
      "type": "number",
      "minimum": 0
    },
    "label": {
      "description": "The name of the target/label column the model predicts. Used to detect target leakage (a feature that is, or is derived from, the label).",
      "type": "string",
      "minLength": 1
    },
    "duration_column": { "type": "string", "minLength": 1 },
    "event_indicator": { "type": "string", "minLength": 1 },
    "censoring_handled": { "type": "boolean" },
    "stop_after_event": { "type": "boolean" },
    "point_in_time_features": { "type": "boolean" },
    "temporal_validation": { "type": "boolean" },
    "treatment": {
      "description": "The binary treatment-assignment column for an uplift model.",
      "type": "string",
      "minLength": 1
    },
    "outcome": {
      "description": "The binary observed-outcome column for an uplift model.",
      "type": "string",
      "minLength": 1
    },
    "treatment_time": {
      "description": "The treatment instant; causal features with data_as_of after it are post-treatment and refused.",
      "type": "string",
      "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"
    },
    "overlap_lower": {
      "type": "number",
      "minimum": 0.05,
      "maximum": 0.95
    },
    "overlap_upper": {
      "type": "number",
      "minimum": 0.05,
      "maximum": 0.95
    },
    "consent_required": {
      "description": "Speech transcription must require explicit source consent.",
      "type": "boolean"
    },
    "consent_column": {
      "description": "The boolean source field that records speech-processing consent.",
      "type": "string",
      "minLength": 1
    },
    "redact_pii": {
      "description": "Speech transcript text is privacy-filtered before durable storage.",
      "type": "boolean"
    },
    "audio_local_only": {
      "description": "Audio samples remain on their owner node.",
      "type": "boolean"
    },
    "features": {
      "description": "The features an ML step consumes. Each is checked for point-in-time and target leakage against `as_of` / `label`.",
      "type": "array",
      "items": {
        "type": "object",
        "required": ["name"],
        "additionalProperties": false,
        "properties": {
          "name": {
            "description": "The feature's name.",
            "type": "string",
            "minLength": 1
          },
          "data_as_of": {
            "description": "The latest instant of data this feature draws on. If it is after the step's `as_of` cutoff, the feature leaks future data (a point-in-time violation). Same ISO-8601 UTC format as `as_of`.",
            "type": "string",
            "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"
          },
          "derived_from": {
            "description": "What the feature is computed from. The literal \"label\", or the `label` column name, marks the feature as target leakage.",
            "type": "string"
          },
          "leaks_target": {
            "description": "Explicit marker that the feature encodes the target — target leakage.",
            "type": "boolean"
          },
          "post_treatment": {
            "description": "The feature is observed after treatment and cannot be an uplift predictor.",
            "type": "boolean"
          }
        }
      }
    },
    "source_disjoint": {
      "description": "Vision models must split whole source groups between train and test.",
      "type": "boolean"
    },
    "source_column": {
      "description": "The camera, site, document, or other source-group column used for a vision holdout.",
      "type": "string",
      "minLength": 1
    },
    "citations_required": {
      "description": "Requires every grounded-answer sentence to cite retrieved evidence.",
      "type": "boolean"
    },
    "refuse_without_retrieval": {
      "description": "Requires the language path to refuse when no relevant chunk was retrieved.",
      "type": "boolean"
    },
    "governance_check": {
      "description": "Requires runtime authorization before language training reads text.",
      "type": "boolean"
    },
    "caller_read_required": {
      "description": "Marks a source whose selected columns all require caller read access.",
      "type": "boolean"
    },
    "event_time_ordered": {
      "description": "Streaming work is released in event-time order behind its watermark.",
      "type": "boolean"
    },
    "allowed_lateness": {
      "description": "How far behind the maximum observed event time an event may arrive before exclusion.",
      "type": "number",
      "minimum": 0
    },
    "future_event_policy": {
      "description": "How streaming work prevents future events from entering the current decision.",
      "type": "string",
      "enum": ["buffer_until_watermark"]
    },
    "label_time_policy": {
      "description": "A label may train the model only at or after its event time.",
      "type": "string",
      "enum": ["not_before_event_time"]
    },
    "prequential": {
      "description": "Metrics are computed test-then-train from the score made before an update.",
      "type": "boolean"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

namespace {

using nlohmann::json;

// A process-wide validator, compiled once from the schema (the expensive step);
// validate() is then cheap and runs at every step's attribute bag.
nlohmann::json_schema::json_validator& annotations_validator() {
    static nlohmann::json_schema::json_validator validator(
        graph_step_annotations_schema());
    return validator;
}

// Collects the first validation failure. The error-handler overload of validate()
// (rather than the throwing one) keeps validation failures from crossing the
// validator-library boundary as exceptions — the library's recommended path and
// robust regardless of how each side is compiled (mirrors src/core/run.cpp).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const json::json_pointer& pointer, const json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// Validate a step's attribute bag against the annotation schema at the boundary.
// Returns a human-readable reason on failure, std::nullopt when valid or when
// there is nothing to validate (a bare node whose bag is null/non-object).
std::optional<std::string> validate_annotations(const json& attributes) {
    if (!attributes.is_object()) return std::nullopt;
    collecting_error_handler handler;
    annotations_validator().validate(attributes, handler);
    if (handler.failed) return handler.reason;
    return std::nullopt;
}

// A short, stable label for a node in a reason string: name + id.
std::string node_label(const ir::Node& node) {
    return "step \"" + node.name + "\" (" + node.id + ")";
}

// Canonicalize a type token for comparison: trim surrounding whitespace and lower
// the case. Types are opaque strings, so "Int" and "int " match.
std::string normalize_type(std::string token) {
    const auto is_space = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    token.erase(token.begin(),
                std::find_if_not(token.begin(), token.end(), is_space));
    token.erase(std::find_if_not(token.rbegin(), token.rend(), is_space).base(),
                token.end());
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return token;
}

// Normalize an ISO-8601 UTC instant (already schema-validated against the
// date / date-time pattern) into a fixed-width "YYYY-MM-DDTHH:MM:SS" key so two
// instants order correctly under plain string comparison. A date-only value
// (length 10) is treated as midnight; fractional seconds and a trailing "Z" are
// dropped (second granularity is plenty for a point-in-time cutoff).
std::string comparable_instant(const std::string& instant) {
    const std::string date = instant.substr(0, 10);
    std::string time = "00:00:00";
    if (instant.size() >= 19 && instant[10] == 'T') {
        time = instant.substr(11, 8);
    }
    return date + "T" + time;
}

// Read an optional string field from a JSON object; std::nullopt when absent or
// not a string. (After validate_annotations the named fields are well-typed, but
// reading defensively keeps these helpers self-contained.)
std::optional<std::string> string_field(const json& object,
                                        const std::string& key) {
    if (!object.is_object()) return std::nullopt;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return std::nullopt;
    return found->get<std::string>();
}

// The union of the `schema` (source profile) field→types of every transitive
// data-flow ancestor of `start`. A field may be provided by more than one source,
// so each maps to the list of (raw) types seen, deduplicated by insertion order.
// Cycles are tolerated (acyclicity isn't enforced yet) via a visited set.
std::unordered_map<std::string, std::vector<std::string>> ancestor_schema(
    const ir::ExecutionGraph& graph, const ir::NodeId& start) {
    std::unordered_map<std::string, std::vector<std::string>> provided;
    std::unordered_set<ir::NodeId> visited;
    std::vector<ir::NodeId> frontier;

    const auto push_dataflow_predecessors = [&](const ir::NodeId& id) {
        for (const ir::Edge& edge : graph.edges_into(id)) {
            if (edge.kind == ir::EdgeKind::DataFlow) frontier.push_back(edge.from);
        }
    };

    push_dataflow_predecessors(start);
    while (!frontier.empty()) {
        const ir::NodeId id = frontier.back();
        frontier.pop_back();
        if (!visited.insert(id).second) continue;
        const std::shared_ptr<ir::Node> node = graph.find_node(id);
        if (node && node->attributes.is_object()) {
            const auto schema = node->attributes.find("schema");
            if (schema != node->attributes.end() && schema->is_object()) {
                for (const auto& [field, type] : schema->items()) {
                    if (!type.is_string()) continue;
                    std::vector<std::string>& types = provided[field];
                    const std::string value = type.get<std::string>();
                    if (std::find(types.begin(), types.end(), value) ==
                        types.end()) {
                        types.push_back(value);
                    }
                }
            }
        }
        push_dataflow_predecessors(id);
    }
    return provided;
}

// Join a list of type tokens for a reason string: ["int", "string"] -> "int, string".
std::string join_types(const std::vector<std::string>& types) {
    std::string out;
    for (const std::string& type : types) {
        if (!out.empty()) out += ", ";
        out += type;
    }
    return out;
}

bool true_field(const json& object, std::string_view name) {
    if (!object.is_object()) return false;
    const auto found = object.find(name);
    return found != object.end() && found->is_boolean() && found->get<bool>();
}

bool language_model(const ir::Node& node) {
    if (node.kind != ir::StepKind::Model || !node.attributes.is_object()) {
        return false;
    }
    const auto field = node.attributes.find("model_kind");
    if (field == node.attributes.end() || !field->is_string()) return false;
    const std::string kind = normalize_type(field->get<std::string>());
    return kind == "language" || kind == "nlp" || kind == "embedding" ||
           kind == "embeddings" || kind == "rag" ||
           kind == "fine_tuning" || kind == "retrieval";
}

bool upstream_requires_caller_read(const ir::ExecutionGraph& graph,
                                   const ir::NodeId& start) {
    std::unordered_set<ir::NodeId> visited;
    std::vector<ir::NodeId> frontier;
    frontier.push_back(start);
    while (!frontier.empty()) {
        const ir::NodeId id = frontier.back();
        frontier.pop_back();
        if (!visited.insert(id).second) continue;
        for (const ir::Edge& edge : graph.edges_into(id)) {
            if (edge.kind != ir::EdgeKind::DataFlow) continue;
            const std::shared_ptr<ir::Node> source = graph.find_node(edge.from);
            if (source && source->kind == ir::StepKind::Connect &&
                true_field(source->attributes, "caller_read_required")) {
                return true;
            }
            frontier.push_back(edge.from);
        }
    }
    return false;
}

bool speech_step(const ir::Node& node) {
    if (node.kind != ir::StepKind::Model || !node.attributes.is_object()) {
        return false;
    }
    const auto kind = node.attributes.find("model_kind");
    if (kind == node.attributes.end() || !kind->is_string()) {
        return false;
    }
    const std::string value = normalize_type(kind->get<std::string>());
    return value == "speech" || value == "audio";
}

CheckResult require_speech_flag(const ir::ExecutionGraph& graph,
                                std::string name, const char* flag,
                                std::string failure, std::string success) {
    for (const auto& node : graph.nodes()) {
        if (!node || !speech_step(*node)) {
            continue;
        }
        const auto value = node->attributes.find(flag);
        if (value == node->attributes.end() || !value->is_boolean() ||
            !value->get<bool>()) {
            return {std::move(name), CheckStatus::Fail,
                    node_label(*node) + ": " + std::move(failure)};
        }
    }
    return {std::move(name), CheckStatus::Pass, std::move(success)};
}

struct SnapshotBoundary {
    solutions::TimePoint prediction;
    std::string prediction_text;
};

std::optional<std::string> snapshot_window_error(
    const ir::Node& model, SnapshotBoundary& boundary) {
    const std::optional<std::string> prediction =
        string_field(model.attributes, "as_of");
    const std::optional<std::string> feature_end =
        string_field(model.attributes, "feature_window_end");
    const std::optional<std::string> label_start =
        string_field(model.attributes, "label_window_start");
    const auto blackout = model.attributes.find("blackout_gap_seconds");
    if (!prediction || !feature_end || !label_start ||
        blackout == model.attributes.end() || !blackout->is_number()) {
        return node_label(model) +
               ": feature lineage requires as_of, feature_window_end, "
               "label_window_start, and blackout_gap_seconds";
    }

    const std::optional<solutions::TimePoint> prediction_time =
        solutions::parse_time_point(*prediction);
    const std::optional<solutions::TimePoint> feature_end_time =
        solutions::parse_time_point(*feature_end);
    const std::optional<solutions::TimePoint> label_start_time =
        solutions::parse_time_point(*label_start);
    if (!prediction_time || !feature_end_time || !label_start_time) {
        return node_label(model) +
               ": feature and label windows must use valid ISO-8601 times";
    }
    if (feature_end_time->value > prediction_time->value) {
        return node_label(model) + ": feature_window_end " + *feature_end +
               " is after prediction as_of " + *prediction;
    }
    const double gap = blackout->get<double>();
    if (label_start_time->value < feature_end_time->value + gap) {
        return node_label(model) + ": label_window_start " + *label_start +
               " violates the configured blackout gap of " +
               std::to_string(gap) + " seconds";
    }

    boundary = SnapshotBoundary{*prediction_time, *prediction};
    return std::nullopt;
}

std::vector<std::shared_ptr<ir::Node>> terminal_data_sources(
    const ir::ExecutionGraph& graph, const ir::NodeId& start) {
    std::unordered_set<ir::NodeId> visited;
    std::vector<ir::NodeId> frontier{start};
    std::vector<std::shared_ptr<ir::Node>> terminals;
    while (!frontier.empty()) {
        const ir::NodeId current = frontier.back();
        frontier.pop_back();
        if (!visited.insert(current).second) {
            continue;
        }
        bool has_predecessor = false;
        for (const ir::Edge& edge : graph.edges_into(current)) {
            if (edge.kind != ir::EdgeKind::DataFlow) {
                continue;
            }
            has_predecessor = true;
            frontier.push_back(edge.from);
        }
        if (!has_predecessor && current != start) {
            terminals.push_back(graph.find_node(current));
        }
    }
    return terminals;
}

std::optional<std::string> snapshot_source_error(
    const ir::ExecutionGraph& graph, const ir::Node& model,
    const SnapshotBoundary& boundary) {
    const std::vector<std::shared_ptr<ir::Node>> terminals =
        terminal_data_sources(graph, model.id);
    if (terminals.empty()) {
        return node_label(model) +
               ": feature lineage has no terminal snapshot source";
    }
    for (const std::shared_ptr<ir::Node>& source : terminals) {
        if (!source || source->kind != ir::StepKind::Connect ||
            !source->attributes.is_object()) {
            return node_label(model) +
                   ": feature lineage terminates at a non-source step";
        }
        const std::optional<std::string> source_kind =
            string_field(source->attributes, "source_kind");
        const std::optional<std::string> snapshot_as_of =
            string_field(source->attributes, "snapshot_as_of");
        if (!source_kind || *source_kind != "snapshot" || !snapshot_as_of) {
            return node_label(*source) +
                   ": live-table feature lineage is refused; materialize and "
                   "reference a snapshot source";
        }
        const std::optional<solutions::TimePoint> snapshot_time =
            solutions::parse_time_point(*snapshot_as_of);
        if (!snapshot_time ||
            snapshot_time->kind != boundary.prediction.kind) {
            return node_label(*source) +
                   ": snapshot_as_of must use the model's time representation";
        }
        if (snapshot_time->value > boundary.prediction.value) {
            return node_label(*source) + ": snapshot_as_of " +
                   *snapshot_as_of + " is after prediction as_of " +
                   boundary.prediction_text;
        }
    }
    return std::nullopt;
}

const json* s1_correctness_profile(const ir::ExecutionGraph& graph,
                                   bool& applies, int& level,
                                   std::string& error) {
    applies = false;
    level = 0;
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest || !manifest->attributes.is_object() ||
        manifest->attributes.value("solution", std::string{}) != "S-1") {
        return nullptr;
    }
    if (!manifest->attributes.contains("foundation")) {
        return nullptr;
    }
    applies = true;
    const auto selected_level = manifest->attributes.find("level");
    if (selected_level == manifest->attributes.end() ||
        !selected_level->is_number_integer()) {
        error = "S-1 manifest must declare an integer level";
        return nullptr;
    }
    level = selected_level->get<int>();
    const auto profile = manifest->attributes.find("s1_correctness");
    if (profile == manifest->attributes.end() || !profile->is_object()) {
        error = "S-1 manifest is missing its correctness profile";
        return nullptr;
    }
    return &*profile;
}

const json* s2_correctness_profile(const ir::ExecutionGraph& graph,
                                   bool& applies, int& level,
                                   std::string& error) {
    applies = false;
    level = 0;
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest || !manifest->attributes.is_object() ||
        manifest->attributes.value("solution", std::string{}) != "S-2") {
        return nullptr;
    }
    if (!manifest->attributes.contains("foundation")) {
        return nullptr;
    }
    applies = true;
    const auto selected_level = manifest->attributes.find("level");
    if (selected_level == manifest->attributes.end() ||
        !selected_level->is_number_integer()) {
        error = "S-2 manifest must declare an integer level";
        return nullptr;
    }
    level = selected_level->get<int>();
    const auto profile = manifest->attributes.find("s2_correctness");
    if (profile == manifest->attributes.end() || !profile->is_object()) {
        error = "S-2 manifest is missing its correctness profile";
        return nullptr;
    }
    return &*profile;
}

const json* s5_correctness_profile(const ir::ExecutionGraph& graph,
                                   bool& applies, int& level,
                                   std::string& error) {
    applies = false;
    level = 0;
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest || !manifest->attributes.is_object() ||
        manifest->attributes.value("solution", std::string{}) != "S-5") {
        return nullptr;
    }
    if (!manifest->attributes.contains("foundation")) {
        return nullptr;
    }
    applies = true;
    const auto selected_level = manifest->attributes.find("level");
    if (selected_level == manifest->attributes.end() ||
        !selected_level->is_number_integer()) {
        error = "S-5 manifest must declare an integer level";
        return nullptr;
    }
    level = selected_level->get<int>();
    const auto profile = manifest->attributes.find("s5_correctness");
    if (profile == manifest->attributes.end() || !profile->is_object()) {
        error = "S-5 manifest is missing its correctness profile";
        return nullptr;
    }
    return &*profile;
}

CheckResult s1_not_applicable(std::string name) {
    return {std::move(name), CheckStatus::Pass,
            "no bound S-1 Pipeline to validate"};
}

CheckResult s1_failure(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Fail, std::move(reason)};
}

CheckResult s1_success(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Pass, std::move(reason)};
}

CheckResult s2_not_applicable(std::string name) {
    return {std::move(name), CheckStatus::Pass,
            "no bound S-2 Pipeline to validate"};
}

CheckResult s2_failure(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Fail, std::move(reason)};
}

CheckResult s2_success(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Pass, std::move(reason)};
}

CheckResult s5_not_applicable(std::string name) {
    return {std::move(name), CheckStatus::Pass,
            "no bound S-5 Pipeline to validate"};
}

CheckResult s5_failure(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Fail, std::move(reason)};
}

CheckResult s5_success(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Pass, std::move(reason)};
}

const json* object_field(const json& value, const char* name) {
    if (!value.is_object()) {
        return nullptr;
    }
    const auto found = value.find(name);
    return found != value.end() && found->is_object() ? &*found : nullptr;
}

bool has_methodology(const ir::ExecutionGraph& graph,
                     std::string_view methodology) {
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest || !manifest->attributes.is_object()) {
        return false;
    }
    const auto declared = manifest->attributes.find("methodologies");
    if (declared == manifest->attributes.end() || !declared->is_array()) {
        return false;
    }
    return std::ranges::any_of(*declared, [&](const json& entry) {
        return entry.is_string() &&
               entry.get_ref<const std::string&>() == methodology;
    });
}

const ir::Node* model_kind(const ir::ExecutionGraph& graph,
                           std::string_view kind) {
    for (const std::shared_ptr<ir::Node>& node :
         graph.nodes_of_kind(ir::StepKind::Model)) {
        if (node && node->attributes.is_object() &&
            node->attributes.value("model_kind", std::string{}) == kind) {
            return node.get();
        }
    }
    return nullptr;
}

const json* s4_correctness_profile(const ir::ExecutionGraph& graph,
                                   bool& applies,
                                   std::string& error) {
    applies = false;
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest || !manifest->attributes.is_object() ||
        manifest->attributes.value("solution", std::string{}) != "S-4") {
        return nullptr;
    }
    if (!manifest->attributes.contains("foundation")) {
        return nullptr;
    }
    applies = true;
    const auto level = manifest->attributes.find("level");
    if (level == manifest->attributes.end() ||
        !level->is_number_integer()) {
        error = "S-4 manifest must declare an integer level";
        return nullptr;
    }
    const auto profile = manifest->attributes.find("s4_correctness");
    if (profile == manifest->attributes.end() || !profile->is_object()) {
        error = "S-4 manifest is missing its correctness profile";
        return nullptr;
    }
    return &*profile;
}

CheckResult s4_not_applicable(std::string name) {
    return {std::move(name), CheckStatus::Pass,
            "no bound S-4 Pipeline to validate"};
}

CheckResult s4_failure(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Fail, std::move(reason)};
}

CheckResult s4_success(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Pass, std::move(reason)};
}

bool has_boolean_policy_predicate(const json& policy,
                                  const char* rule_name,
                                  std::string_view field,
                                  bool expected) {
    const auto rules = policy.find(rule_name);
    if (rules == policy.end() || !rules->is_array()) {
        return false;
    }
    return std::ranges::any_of(*rules, [&](const json& rule) {
        if (!rule.is_object()) {
            return false;
        }
        const auto named_field = rule.find("field");
        const auto operation = rule.find("operator");
        const auto value = rule.find("value");
        return named_field != rule.end() && named_field->is_string() &&
               named_field->get_ref<const std::string&>() == field &&
               operation != rule.end() && operation->is_string() &&
               operation->get_ref<const std::string&>() == "eq" &&
               value != rule.end() && value->is_boolean() &&
               value->get<bool>() == expected;
    });
}

const json* s3_correctness_profile(const ir::ExecutionGraph& graph,
                                   bool& applies, int& level,
                                   std::string& error) {
    applies = false;
    level = 0;
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest || !manifest->attributes.is_object() ||
        manifest->attributes.value("solution", std::string{}) != "S-3") {
        return nullptr;
    }
    if (!manifest->attributes.contains("foundation")) {
        return nullptr;
    }
    applies = true;
    const auto selected_level = manifest->attributes.find("level");
    if (selected_level == manifest->attributes.end() ||
        !selected_level->is_number_integer()) {
        error = "S-3 manifest must declare an integer level";
        return nullptr;
    }
    level = selected_level->get<int>();
    const auto profile = manifest->attributes.find("s3_correctness");
    if (profile == manifest->attributes.end() || !profile->is_object()) {
        error = "S-3 manifest is missing its correctness profile";
        return nullptr;
    }
    return &*profile;
}

CheckResult s3_not_applicable(std::string name) {
    return {std::move(name), CheckStatus::Pass,
            "no bound S-3 Pipeline to validate"};
}

CheckResult s3_failure(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Fail, std::move(reason)};
}

CheckResult s3_success(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Pass, std::move(reason)};
}

}  // namespace

CheckResult S1PointInTimeCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const auto prediction = profile->find("prediction_time");
    if (prediction == profile->end()) {
        return s1_failure(name(),
                          "S-1 must declare the prediction time T");
    }
    const std::optional<solutions::TimePoint> prediction_time =
        solutions::parse_time_point(*prediction);
    if (!prediction_time) {
        return s1_failure(name(), "S-1 prediction time T is invalid");
    }
    std::size_t sources = 0;
    for (const std::shared_ptr<ir::Node>& node :
         graph.nodes_of_kind(ir::StepKind::Connect)) {
        if (!node || !node->attributes.contains("solution_stage")) {
            continue;
        }
        ++sources;
        if (node->attributes.value("source_kind", std::string{}) !=
            "snapshot") {
            return s1_failure(
                name(), node_label(*node) +
                            ": live feature sources are refused; use the "
                            "point-in-time snapshot layer");
        }
        auto cutoff = node->attributes.find("solution_snapshot_as_of");
        if (cutoff == node->attributes.end()) {
            cutoff = node->attributes.find("snapshot_as_of");
        }
        if (cutoff == node->attributes.end()) {
            return s1_failure(
                name(), node_label(*node) +
                            ": snapshot source has no inclusive cutoff");
        }
        const std::optional<solutions::TimePoint> snapshot_time =
            solutions::parse_time_point(*cutoff);
        if (!snapshot_time || snapshot_time->kind != prediction_time->kind ||
            snapshot_time->value > prediction_time->value) {
            return s1_failure(
                name(), node_label(*node) +
                            ": snapshot cutoff must be at or before prediction "
                            "time T");
        }
    }
    if (sources == 0) {
        return s1_failure(name(), "S-1 has no methodology snapshot sources");
    }
    return s1_success(name(), std::to_string(sources) +
                                  " S-1 feature sources end in snapshots at "
                                  "or before T");
}

CheckResult S1LabelWindowCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const auto feature_end = profile->find("feature_window_end");
    const auto label_start = profile->find("label_window_start");
    const auto blackout = profile->find("blackout_gap_seconds");
    if (feature_end == profile->end() || label_start == profile->end() ||
        blackout == profile->end() || !blackout->is_number() ||
        blackout->get<double>() < 0.0) {
        return s1_failure(
            name(), "S-1 must declare feature and label windows plus a "
                    "non-negative blackout gap");
    }
    const auto feature_time = solutions::parse_time_point(*feature_end);
    const auto label_time = solutions::parse_time_point(*label_start);
    if (!feature_time || !label_time ||
        feature_time->kind != label_time->kind ||
        label_time->value <
            feature_time->value + blackout->get<double>()) {
        return s1_failure(
            name(), "S-1 label window (T, T+H] must start after the feature "
                    "window and its blackout gap");
    }
    return s1_success(name(),
                      "S-1 label window starts after the blackout gap");
}

CheckResult S1TemporalValidationCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const json* validation = object_field(*profile, "validation");
    if (validation == nullptr ||
        validation->value("split", std::string{}) != "out_of_time" ||
        validation->value("rolling_windows", 0) < 2 ||
        !validation->value("stable_across_windows", false)) {
        return s1_failure(
            name(), "S-1 requires an out-of-time split with stability shown "
                    "across at least two rolling windows; random splits are "
                    "refused");
    }
    return s1_success(name(),
                      "S-1 uses stable rolling out-of-time validation");
}

CheckResult S1GroupSplitCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const json* validation = object_field(*profile, "validation");
    if (validation == nullptr ||
        validation->value("group_column", std::string{}).empty() ||
        !validation->value("group_disjoint", false)) {
        return s1_failure(
            name(), "S-1 requires a named customer group and a disjoint "
                    "group split so one customer cannot straddle train and "
                    "test");
    }
    return s1_success(name(),
                      "S-1 keeps each customer wholly on one split side");
}

CheckResult S1CensoringCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const json* labels = object_field(*profile, "labels");
    if (labels == nullptr || !labels->value("fully_observed", false)) {
        return s1_failure(
            name(), "S-1 propensity labels must come only from fully "
                    "observed outcome windows");
    }
    if (level >= 2) {
        const ir::Node* survival = model_kind(graph, "survival");
        if (survival == nullptr ||
            !survival->attributes.value("censoring_handled", false) ||
            !survival->attributes.value("stop_after_event", false)) {
            return s1_failure(
                name(), "S-1 level 2 and above requires the censoring-aware "
                        "M-5 survival head");
        }
    }
    return s1_success(
        name(), level >= 2
                    ? "fully observed propensity labels and M-5 censoring are enforced"
                    : "propensity labels use fully observed outcome windows");
}

CheckResult S1CalibrationCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const json* sampling = object_field(*profile, "sampling");
    if (sampling == nullptr) {
        return s1_failure(name(), "S-1 must declare its sampling policy");
    }
    if (!sampling->value("used", false)) {
        return s1_success(name(), "S-1 uses no sampled training population");
    }
    const std::string method =
        sampling->value("calibration", std::string{});
    if ((method != "platt" && method != "isotonic") ||
        !sampling->value("calibration_disjoint", false)) {
        return s1_failure(
            name(), "sampled S-1 training requires disjoint Platt or "
                    "isotonic probability calibration");
    }
    return s1_success(name(),
                      "sampled S-1 probabilities are calibrated disjointly");
}

CheckResult S1EntityResolutionCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const json* entity = object_field(*profile, "entity_resolution");
    const ir::Node* resolver = model_kind(graph, "entity_resolution");
    if (entity == nullptr ||
        entity->value("methodology", std::string{}) != "M-2" ||
        entity->value("actionable_unit", std::string{}).empty() ||
        entity->value("duplicate_policy", std::string{}) != "refuse" ||
        resolver == nullptr) {
        return s1_failure(
            name(), "S-1 requires M-2 to resolve a named actionable unit and "
                    "must refuse duplicate entities");
    }
    return s1_success(name(),
                      "M-2 resolves one duplicate-free actionable entity");
}

CheckResult S1LeakageReviewCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    const auto blocklist = profile->find("leaky_feature_blocklist");
    const auto features = profile->find("feature_names");
    if (blocklist == profile->end() || !blocklist->is_array() ||
        blocklist->empty() || features == profile->end() ||
        !features->is_array() || features->empty()) {
        return s1_failure(
            name(), "S-1 requires a configurable, non-empty leaky-feature "
                    "blocklist and declared feature names");
    }
    std::unordered_set<std::string> blocked;
    for (const json& entry : *blocklist) {
        if (!entry.is_string() || entry.get_ref<const std::string&>().empty()) {
            return s1_failure(name(),
                              "S-1 leaky-feature blocklist entries must be names");
        }
        blocked.insert(normalize_type(entry.get_ref<const std::string&>()));
    }
    for (const json& feature : *features) {
        if (!feature.is_string()) {
            return s1_failure(name(), "S-1 feature names must be strings");
        }
        const std::string normalized =
            normalize_type(feature.get_ref<const std::string&>());
        if (blocked.contains(normalized)) {
            return s1_failure(
                name(), "S-1 feature \"" +
                            feature.get_ref<const std::string&>() +
                            "\" is on the leaky-feature blocklist");
        }
    }
    const auto threshold = profile->find("too_good_auc_threshold");
    if (threshold == profile->end() || !threshold->is_number() ||
        threshold->get<double>() <= 0.0 ||
        threshold->get<double>() > 0.95) {
        return s1_failure(
            name(), "S-1 too-good-AUC review threshold must be configured at "
                    "or below 0.95");
    }
    if (const auto reported = profile->find("reported_auc");
        reported != profile->end() &&
        (!reported->is_number() ||
         reported->get<double>() > threshold->get<double>())) {
        return s1_failure(
            name(), "S-1 AUC exceeds the too-good threshold; review target "
                    "and post-outcome leakage before deployment");
    }
    return s1_success(name(),
                      "S-1 features clear the blocklist and AUC review");
}

CheckResult S1LevelCompositionCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s1_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s1_not_applicable(name());
    }
    if (profile == nullptr) {
        return s1_failure(name(), error);
    }
    if (level >= 2 && !has_methodology(graph, "M-5")) {
        return s1_failure(name(), "S-1 level 2 requires M-5 survival timing");
    }
    if (level >= 3) {
        const ir::Node* uplift = model_kind(graph, "uplift");
        if (!has_methodology(graph, "M-6") || uplift == nullptr ||
            uplift->attributes.value("randomized_cycles", 0) < 1 ||
            uplift->attributes.value("sleeping_dogs_action", std::string{}) !=
                "no_contact" ||
            !uplift->attributes.value("positive_uplift_only", false)) {
            return s1_failure(
                name(), "S-1 level 3 requires M-6, at least one randomized "
                        "cycle, and first-class no-contact for sleeping dogs");
        }
    }
    if (level >= 4 && !has_methodology(graph, "M-8")) {
        return s1_failure(name(),
                          "S-1 level 4 requires M-8 treatment arbitration");
    }
    return s1_success(name(),
                      "S-1 selected level has every required methodology");
}

CheckResult S2PointInTimeConversionCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s2_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s2_not_applicable(name());
    }
    if (profile == nullptr) {
        return s2_failure(name(), error);
    }
    const auto prediction = profile->find("prediction_time");
    const auto feature_end = profile->find("feature_window_end");
    const auto label_start = profile->find("label_window_start");
    const auto blackout = profile->find("blackout_gap_seconds");
    const json* labels = object_field(*profile, "labels");
    if (prediction == profile->end() || feature_end == profile->end() ||
        label_start == profile->end() || blackout == profile->end() ||
        !blackout->is_number() || blackout->get<double>() < 0.0 ||
        labels == nullptr ||
        labels->value("label_column", std::string{}).empty() ||
        !labels->value("fully_observed", false)) {
        return s2_failure(
            name(), "S-2 requires a named, fully observed conversion label "
                    "after the feature cutoff and blackout gap");
    }
    const std::optional<solutions::TimePoint> prediction_time =
        solutions::parse_time_point(*prediction);
    const std::optional<solutions::TimePoint> feature_time =
        solutions::parse_time_point(*feature_end);
    const std::optional<solutions::TimePoint> label_time =
        solutions::parse_time_point(*label_start);
    if (!prediction_time || !feature_time || !label_time ||
        feature_time->kind != label_time->kind ||
        label_time->value <
            feature_time->value + blackout->get<double>()) {
        return s2_failure(
            name(), "S-2 conversion labels must begin after the point-in-time "
                    "feature window and its blackout gap");
    }
    std::size_t sources = 0;
    for (const std::shared_ptr<ir::Node>& node :
         graph.nodes_of_kind(ir::StepKind::Connect)) {
        if (!node || !node->attributes.contains("solution_stage")) {
            continue;
        }
        ++sources;
        if (node->attributes.value("source_kind", std::string{}) !=
            "snapshot") {
            return s2_failure(
                name(), node_label(*node) +
                            ": live CRM features are refused; use the "
                            "point-in-time snapshot layer");
        }
        auto cutoff = node->attributes.find("solution_snapshot_as_of");
        if (cutoff == node->attributes.end()) {
            cutoff = node->attributes.find("snapshot_as_of");
        }
        if (cutoff == node->attributes.end()) {
            return s2_failure(
                name(), node_label(*node) +
                            ": snapshot source has no inclusive cutoff");
        }
        const std::optional<solutions::TimePoint> snapshot_time =
            solutions::parse_time_point(*cutoff);
        if (!snapshot_time || snapshot_time->kind != prediction_time->kind ||
            snapshot_time->value > prediction_time->value) {
            return s2_failure(
                name(), node_label(*node) +
                            ": snapshot cutoff must be at or before the S-2 "
                            "scoring time");
        }
    }
    if (sources == 0) {
        return s2_failure(name(), "S-2 has no methodology snapshot sources");
    }
    return s2_success(
        name(), std::to_string(sources) +
                    " S-2 feature sources and conversion labels are point-in-time safe");
}

CheckResult S2PostQualificationLeakageCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s2_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s2_not_applicable(name());
    }
    if (profile == nullptr) {
        return s2_failure(name(), error);
    }
    const auto blocklist = profile->find("post_qualification_blocklist");
    const auto features = profile->find("feature_names");
    if (blocklist == profile->end() || !blocklist->is_array() ||
        blocklist->empty() || features == profile->end() ||
        !features->is_array() || features->empty()) {
        return s2_failure(
            name(), "S-2 requires declared features and a non-empty "
                    "post-qualification field blocklist");
    }
    std::unordered_set<std::string> blocked;
    for (const json& entry : *blocklist) {
        if (!entry.is_string() || entry.get_ref<const std::string&>().empty()) {
            return s2_failure(
                name(), "S-2 post-qualification blocklist entries must be names");
        }
        blocked.insert(normalize_type(entry.get_ref<const std::string&>()));
    }
    for (const json& feature : *features) {
        if (!feature.is_string()) {
            return s2_failure(name(), "S-2 feature names must be strings");
        }
        if (blocked.contains(
                normalize_type(feature.get_ref<const std::string&>()))) {
            return s2_failure(
                name(), "S-2 feature \"" +
                            feature.get_ref<const std::string&>() +
                            "\" is only available after qualification or conversion");
        }
    }
    return s2_success(
        name(), "S-2 predictors exclude post-qualification CRM fields");
}

CheckResult S2CapacityRoutingCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s2_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s2_not_applicable(name());
    }
    if (profile == nullptr) {
        return s2_failure(name(), error);
    }
    const json* routing = object_field(*profile, "routing");
    if (routing == nullptr || !routing->value("capacity_aware", false) ||
        routing->value("owner_column", std::string{}).empty() ||
        routing->value("territory_column", std::string{}).empty() ||
        routing->value("capacity_column", std::string{}).empty() ||
        routing->value("over_capacity_action", std::string{}) !=
            "no_contact") {
        return s2_failure(
            name(), "S-2 routing must name owner, territory, and current rep "
                    "capacity evidence and suppress over-capacity tasks");
    }
    if (!has_methodology(graph, "M-2") ||
        !has_methodology(graph, "M-3") ||
        (level >= 3 && !has_methodology(graph, "M-6"))) {
        return s2_failure(
            name(), "S-2 selected level is missing an entity, segment, or "
                    "incrementality stage required for routing");
    }
    return s2_success(
        name(), "S-2 routing is owner-, territory-, and capacity-aware");
}

CheckResult S3ShortTenureCensoringCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s3_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s3_not_applicable(name());
    }
    if (profile == nullptr) {
        return s3_failure(name(), error);
    }
    const json* censoring = object_field(*profile, "short_tenure");
    if (censoring == nullptr ||
        censoring->value("tenure_column", std::string{}).empty() ||
        censoring->value("minimum_observed_days", 0) <= 0 ||
        censoring->value("handling", std::string{}) != "right_censor" ||
        !censoring->value("applied_to_training_labels", false)) {
        return s3_failure(
            name(), "S-3 must right-censor short-tenure customers before "
                    "training and name the positive minimum-tenure rule");
    }
    if (level >= 2) {
        const ir::Node* survival = model_kind(graph, "survival");
        if (!has_methodology(graph, "M-5") || survival == nullptr ||
            !survival->attributes.value("censoring_handled", false) ||
            !survival->attributes.value("stop_after_event", false)) {
            return s3_failure(
                name(), "S-3 level 2 and above requires the censoring-aware "
                        "M-5 survival stage");
        }
    }
    return s3_success(
        name(), level >= 2
                    ? "short-tenure labels are censored and M-5 models tenure"
                    : "short-tenure labels are right-censored before regression");
}

CheckResult S3ValueHorizonCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s3_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s3_not_applicable(name());
    }
    if (profile == nullptr) {
        return s3_failure(name(), error);
    }
    const auto horizon = profile->find("value_horizon_days");
    const std::string value_label =
        profile->value("value_label", std::string{});
    if (horizon == profile->end() || !horizon->is_number_integer() ||
        horizon->get<int>() <= 0 || value_label.empty()) {
        return s3_failure(
            name(), "S-3 must state a positive value horizon and its net-value label");
    }
    const ir::Node* regression = model_kind(graph, "regression");
    if (regression == nullptr ||
        regression->attributes.value("objective", std::string{}) !=
            "squared_error" ||
        regression->attributes.value("target", std::string{}) != value_label ||
        regression->attributes.value("value_horizon_days", 0) !=
            horizon->get<int>()) {
        return s3_failure(
            name(), "S-3 lifetime-value regression must use the declared label and horizon");
    }
    return s3_success(name(), "S-3 predicts net value over its declared horizon");
}

CheckResult S3EntityAccountingCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s3_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s3_not_applicable(name());
    }
    if (profile == nullptr) {
        return s3_failure(name(), error);
    }
    const json* accounting = object_field(*profile, "entity_accounting");
    if (accounting == nullptr ||
        accounting->value("entity_column", std::string{}).empty() ||
        accounting->value("aggregation", std::string{}) !=
            "one_value_per_entity" ||
        accounting->value("duplicate_policy", std::string{}) != "refuse" ||
        accounting->value("methodology", std::string{}) != "M-2" ||
        model_kind(graph, "entity_resolution") == nullptr) {
        return s3_failure(
            name(), "S-3 requires M-2 and one value row per stable entity; "
                    "cross-entity or duplicate value counting is refused");
    }
    return s3_success(name(), "S-3 counts each resolved entity exactly once");
}

CheckResult S4RegulatoryContactCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    std::string error;
    const json* profile = s4_correctness_profile(graph, applies, error);
    if (!applies) {
        return s4_not_applicable(name());
    }
    if (profile == nullptr) {
        return s4_failure(name(), error);
    }
    const json* regulatory = object_field(*profile, "regulatory_contact");
    const json* policy = object_field(*profile, "policy");
    const json* frequency =
        policy == nullptr ? nullptr : object_field(*policy, "frequency_cap");
    if (regulatory == nullptr ||
        policy == nullptr ||
        regulatory->value("eligibility_field", std::string{}) !=
            "collections_eligible" ||
        regulatory->value("contact_allowed_field", std::string{}) !=
            "regulatory_contact_allowed" ||
        regulatory->value("contact_hours_field", std::string{}) !=
            "within_contact_hours" ||
        policy->value("contact_cap", 0) < 1 ||
        frequency == nullptr ||
        frequency->value("max_actions", 0) != 1 ||
        frequency->value("window_seconds", 0) < 86400 ||
        !has_boolean_policy_predicate(
            *policy, "eligibility_rules", "collections_eligible", true) ||
        !has_boolean_policy_predicate(
            *policy, "eligibility_rules", "regulatory_contact_allowed", true) ||
        !has_boolean_policy_predicate(
            *policy, "eligibility_rules", "within_contact_hours", true) ||
        !has_boolean_policy_predicate(
            *policy, "exclusion_rules", "collections_eligible", false) ||
        !has_boolean_policy_predicate(
            *policy, "exclusion_rules", "regulatory_contact_allowed", false) ||
        !has_boolean_policy_predicate(
            *policy, "exclusion_rules", "within_contact_hours", false)) {
        return s4_failure(
            name(),
            "S-4 requires policy-enforced collections eligibility, permitted "
            "contact hours, and a one-contact regulatory frequency cap");
    }
    return s4_success(
        name(),
        "S-4 regulatory eligibility and contact limits are policy enforced");
}

CheckResult S4VulnerableCustomerCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    std::string error;
    const json* profile = s4_correctness_profile(graph, applies, error);
    if (!applies) {
        return s4_not_applicable(name());
    }
    if (profile == nullptr) {
        return s4_failure(name(), error);
    }
    const json* vulnerable = object_field(*profile, "vulnerable_customer");
    const json* policy = object_field(*profile, "policy");
    if (vulnerable == nullptr ||
        policy == nullptr ||
        vulnerable->value("field", std::string{}) != "vulnerable" ||
        !vulnerable->value("excluded_when_true", false) ||
        !has_boolean_policy_predicate(
            *policy, "eligibility_rules", "vulnerable", false) ||
        !has_boolean_policy_predicate(
            *policy, "exclusion_rules", "vulnerable", true)) {
        return s4_failure(
            name(),
            "S-4 must exclude vulnerable customers and block contact when "
            "vulnerability evidence is missing");
    }
    return s4_success(
        name(),
        "S-4 vulnerable-customer exclusion blocks true or missing evidence");
}

CheckResult S4PromiseToPayPointInTimeCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    std::string error;
    const json* profile = s4_correctness_profile(graph, applies, error);
    if (!applies) {
        return s4_not_applicable(name());
    }
    if (profile == nullptr) {
        return s4_failure(name(), error);
    }
    const json* labels = object_field(*profile, "promise_to_pay_labels");
    if (labels == nullptr ||
        labels->value("semantics", std::string{}) != "promise_to_pay" ||
        labels->value("label_column", std::string{}).empty() ||
        labels->value("observation_time_column", std::string{}).empty() ||
        !labels->value("point_in_time", false) ||
        !labels->value("future_updates_excluded", false)) {
        return s4_failure(
            name(),
            "S-4 promise-to-pay labels must name their observation time and "
            "exclude status updates after the prediction cutoff");
    }
    const auto prediction = profile->find("prediction_time");
    const auto snapshot = labels->find("snapshot_as_of");
    if (prediction == profile->end() || snapshot == labels->end()) {
        return s4_failure(
            name(), "S-4 promise-to-pay labels require a prediction cutoff");
    }
    const std::optional<solutions::TimePoint> prediction_time =
        solutions::parse_time_point(*prediction);
    const std::optional<solutions::TimePoint> snapshot_time =
        solutions::parse_time_point(*snapshot);
    if (!prediction_time || !snapshot_time ||
        prediction_time->kind != snapshot_time->kind ||
        snapshot_time->value > prediction_time->value) {
        return s4_failure(
            name(),
            "S-4 promise-to-pay label snapshot must be at or before the "
            "prediction cutoff");
    }
    return s4_success(
        name(),
        "S-4 promise-to-pay labels are frozen at the prediction cutoff");
}

namespace {

const json* s12_correctness_profile(const ir::ExecutionGraph& graph,
                                    bool& applies, int& level,
                                    std::string& error) {
    applies = false;
    level = 0;
    const std::shared_ptr<ir::Node> manifest =
        graph.find_node("solution_manifest");
    if (!manifest || !manifest->attributes.is_object() ||
        manifest->attributes.value("solution", std::string{}) != "S-12") {
        return nullptr;
    }
    if (!manifest->attributes.contains("foundation")) {
        return nullptr;
    }
    applies = true;
    const auto selected_level = manifest->attributes.find("level");
    if (selected_level == manifest->attributes.end() ||
        !selected_level->is_number_integer()) {
        error = "S-12 manifest must declare an integer level";
        return nullptr;
    }
    level = selected_level->get<int>();
    const auto profile = manifest->attributes.find("s12_correctness");
    if (profile == manifest->attributes.end() || !profile->is_object()) {
        error = "S-12 manifest is missing its correctness profile";
        return nullptr;
    }
    return &*profile;
}

CheckResult s12_not_applicable(std::string name) {
    return {std::move(name), CheckStatus::Pass,
            "no bound S-12 Pipeline to validate"};
}

CheckResult s12_failure(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Fail, std::move(reason)};
}

CheckResult s12_success(std::string name, std::string reason) {
    return {std::move(name), CheckStatus::Pass, std::move(reason)};
}

bool adverse_action(std::string_view action) {
    const std::string normalized = normalize_type(std::string(action));
    return normalized.find("denial") != std::string::npos ||
           normalized.find("deny") != std::string::npos ||
           normalized.find("reject") != std::string::npos;
}

bool policy_requires(const json& policy, std::string_view field,
                     const json& expected) {
    const auto rules = policy.find("eligibility_rules");
    if (rules == policy.end() || !rules->is_array()) {
        return false;
    }
    return std::ranges::any_of(*rules, [&](const json& rule) {
        return rule.is_object() &&
               rule.value("field", std::string{}) == field &&
               rule.contains("value") && rule.at("value") == expected;
    });
}

} // namespace

CheckResult S12NoAutomatedDenialCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s12_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s12_not_applicable(name());
    }
    if (profile == nullptr) {
        return s12_failure(name(), error);
    }
    const json* policy = object_field(*profile, "policy");
    if (policy == nullptr) {
        return s12_failure(name(), "S-12 must bind its claim-path policy");
    }
    const auto tiers = policy->find("tiers");
    const auto actions = policy->find("actions");
    if (tiers == policy->end() || !tiers->is_array() || tiers->empty() ||
        actions == policy->end() || !actions->is_object()) {
        return s12_failure(name(),
                           "S-12 requires path tiers and action autonomy");
    }
    for (const json& tier : *tiers) {
        if (!tier.is_object() || !tier.contains("action") ||
            !tier.at("action").is_string()) {
            return s12_failure(name(), "S-12 path tiers must name actions");
        }
        const std::string action = tier.at("action");
        if (adverse_action(action)) {
            return s12_failure(
                name(), "S-12 may not select claim denial as a model-driven path");
        }
    }
    bool has_manual_denial = false;
    for (const auto& [action, settings] : actions->items()) {
        if (!adverse_action(action)) {
            continue;
        }
        if (!settings.is_object() ||
            settings.value("autonomy", std::string{}) != "A0") {
            return s12_failure(
                name(), "every adverse claim outcome must remain A0 human-only");
        }
        has_manual_denial = true;
    }
    if (!has_manual_denial) {
        return s12_failure(
            name(), "S-12 must represent denial only as an A0 recommendation");
    }
    return s12_success(
        name(), "claim paths exclude automated denial and retain A0 review");
}

CheckResult S12FairnessReviewCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s12_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s12_not_applicable(name());
    }
    if (profile == nullptr) {
        return s12_failure(name(), error);
    }
    const json* fairness = object_field(*profile, "fairness_review");
    if (fairness == nullptr ||
        !fairness->value("cohort_review_required", false) ||
        !fairness->value("referral_rate_review_required", false) ||
        fairness->value("owner", std::string{}).empty()) {
        return s12_failure(
            name(), "S-12 requires an owned cohort and referral-rate fairness review");
    }
    const auto blocked = fairness->find("blocked_feature_names");
    const auto features = fairness->find("feature_names");
    if (blocked == fairness->end() || !blocked->is_array() ||
        blocked->empty() || features == fairness->end() ||
        !features->is_array()) {
        return s12_failure(
            name(), "S-12 fairness review needs protected-field exclusions and feature names");
    }
    std::unordered_set<std::string> protected_fields;
    for (const json& field : *blocked) {
        if (!field.is_string() || field.get_ref<const std::string&>().empty()) {
            return s12_failure(
                name(), "S-12 protected-field exclusions must be names");
        }
        protected_fields.insert(
            normalize_type(field.get_ref<const std::string&>()));
    }
    for (const json& feature : *features) {
        if (!feature.is_string()) {
            return s12_failure(name(), "S-12 feature names must be strings");
        }
        if (protected_fields.contains(
                normalize_type(feature.get_ref<const std::string&>()))) {
            return s12_failure(
                name(), "S-12 protected attributes are evaluation-only, not model features");
        }
    }
    return s12_success(
        name(), "claim routing has an owned fairness review and protected-field exclusions");
}

CheckResult S12DocumentConfidenceCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s12_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s12_not_applicable(name());
    }
    if (profile == nullptr) {
        return s12_failure(name(), error);
    }
    const json* policy = object_field(*profile, "policy");
    const json* confidence = object_field(*profile, "document_confidence");
    if (policy == nullptr || confidence == nullptr ||
        !confidence->value("low_confidence_routes_to_review", false) ||
        !confidence->value("fast_track_requires_clear", false) ||
        !policy_requires(*policy, "document_confidence_clear", true)) {
        return s12_failure(
            name(), "S-12 must block path delivery until document confidence clears review");
    }
    const double minimum = confidence->value("minimum_review_threshold", 1.1);
    const ir::Node* extraction = model_kind(graph, "document_extraction");
    if (!has_methodology(graph, "M-11") || extraction == nullptr) {
        return s12_failure(name(), "S-12 requires the M-11 document stage");
    }
    const auto threshold = extraction->attributes.find("review_threshold");
    if (threshold == extraction->attributes.end() ||
        !threshold->is_number() ||
        threshold->get<double>() < minimum ||
        threshold->get<double>() > 1.0) {
        return s12_failure(
            name(), "S-12 document review threshold is below its governed minimum");
    }
    return s12_success(
        name(), "M-11 confidence and review routing guard every claim path");
}

CheckResult S12LevelCompositionCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s12_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s12_not_applicable(name());
    }
    if (profile == nullptr) {
        return s12_failure(name(), error);
    }
    for (const std::string_view required : {"M-2", "M-11", "M-1"}) {
        if (!has_methodology(graph, required)) {
            return s12_failure(
                name(), "S-12 levels require M-2, M-11, and M-1");
        }
    }
    if (level >= 3 && !has_methodology(graph, "M-3")) {
        return s12_failure(
            name(), "S-12 level 3 and above requires M-3 claim segmentation");
    }
    bool has_claim_scores = false;
    for (const std::shared_ptr<ir::Node>& node :
         graph.nodes_of_kind(ir::StepKind::Act)) {
        if (!node || !node->attributes.is_object()) {
            continue;
        }
        if (node->attributes.value("entity_output_column", std::string{}) ==
                "claim_id" &&
            node->attributes.value("score_output_column", std::string{}) ==
                "referral_score") {
            has_claim_scores = true;
            break;
        }
    }
    if (!has_claim_scores) {
        return s12_failure(
            name(), "S-12 M-1 must publish claim_id and referral_score outputs");
    }
    return s12_success(
        name(), "S-12 selected level has claim-specific runnable stages");
}

CheckResult S5RollingOriginCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s5_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s5_not_applicable(name());
    }
    if (profile == nullptr) {
        return s5_failure(name(), error);
    }
    const json* validation = object_field(*profile, "validation");
    const ir::Node* forecast = model_kind(graph, "forecast");
    if (validation == nullptr ||
        validation->value("split", std::string{}) != "rolling_origin" ||
        validation->value("backtest_folds", 0) < 2 ||
        forecast == nullptr ||
        forecast->attributes.value("split", std::string{}) !=
            "rolling_origin" ||
        !forecast->attributes.value("temporal_validation", false)) {
        return s5_failure(
            name(), "S-5 requires at least two rolling-origin backtest folds; "
                    "random or row holdouts are refused");
    }
    return s5_success(name(),
                      "S-5 uses rolling-origin backtests only");
}

CheckResult S5FutureCovariateCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s5_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s5_not_applicable(name());
    }
    if (profile == nullptr) {
        return s5_failure(name(), error);
    }
    const auto covariates = profile->find("future_covariates");
    if (covariates == profile->end() || !covariates->is_array() ||
        !covariates->empty()) {
        return s5_failure(
            name(), "S-5 refuses future covariates; forecast features must be "
                    "strictly past target lags and rolling summaries");
    }
    bool found_past_lags = false;
    for (const std::shared_ptr<ir::Node>& node :
         graph.nodes_of_kind(ir::StepKind::Clean)) {
        if (!node ||
            node->attributes.value("transform", std::string{}) !=
                "past_lags") {
            continue;
        }
        found_past_lags = true;
        if (!node->attributes.value("strictly_past", false)) {
            return s5_failure(
                name(), "S-5 forecast features must end strictly before each "
                        "forecast origin");
        }
    }
    if (!found_past_lags) {
        return s5_failure(
            name(), "S-5 forecast features must end strictly before each "
                    "forecast origin");
    }
    return s5_success(name(),
                      "S-5 uses only strictly past target history");
}

CheckResult S5HierarchyConsistencyCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s5_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s5_not_applicable(name());
    }
    if (profile == nullptr) {
        return s5_failure(name(), error);
    }
    const std::string series_column =
        profile->value("series_column", std::string{});
    const auto hierarchy = profile->find("hierarchy_columns");
    if (series_column.empty() || hierarchy == profile->end() ||
        !hierarchy->is_array() || hierarchy->empty() ||
        !hierarchy->back().is_string() ||
        hierarchy->back().get_ref<const std::string&>() != series_column ||
        profile->value("hierarchy_mode", std::string{}) !=
            "bottom_level_only" ||
        !profile->value("hierarchy_consistent", false)) {
        return s5_failure(
            name(), "S-5 requires a named leaf series as the last hierarchy "
                    "column and bottom-level-only forecasts so parent totals "
                    "are derived by summing leaves");
    }
    return s5_success(
        name(), "S-5 forecasts hierarchy leaves and derives parent totals");
}

CheckResult S5LevelCompositionCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    bool applies = false;
    int level = 0;
    std::string error;
    const json* profile =
        s5_correctness_profile(graph, applies, level, error);
    if (!applies) {
        return s5_not_applicable(name());
    }
    if (profile == nullptr) {
        return s5_failure(name(), error);
    }
    for (const std::string_view methodology : {"M-2", "M-4", "M-3"}) {
        if (!has_methodology(graph, methodology)) {
            return s5_failure(
                name(), "S-5 requires M-2, M-4, and M-3 at every level");
        }
    }
    if (level >= 3 && !has_methodology(graph, "M-5")) {
        return s5_failure(
            name(), "S-5 level 3 and above requires M-5 event timing");
    }
    return s5_success(name(),
                      "S-5 selected level has every required methodology");
}

bool TemporalValidationCheck::is_time_ordered(std::string_view model_kind) {
    return model_kind == "forecast" || model_kind == "survival" || model_kind == "streaming";
}

CheckResult TemporalValidationCheck::evaluate(const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model || !node->attributes.is_object()) continue;
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string()) continue;
        const std::string named = kind->get<std::string>();
        if (!is_time_ordered(named)) continue;

        // Calibration on a time-ordered model is the churn recipe misapplied — see the header.
        for (const auto& other : graph.nodes()) {
            if (!other->attributes.is_object()) continue;
            const auto calibrated = other->attributes.find("calibrated");
            if (calibrated != other->attributes.end() && calibrated->is_boolean() &&
                calibrated->get<bool>()) {
                return CheckResult{
                    name(), CheckStatus::Fail,
                    node_label(*node) + ": '" + named +
                        "' is time-ordered, so calibration does not apply — there is no "
                        "probability to calibrate, and fitting a calibrator on rows held out by "
                        "content re-introduces exactly the leak temporal validation prevents. "
                        "Drop `calibrated` and backtest over rolling origins instead."};
            }
        }

        // Look across the whole graph, exactly as the calibration half above does. Looking only
        // at the model node was an asymmetry bug: `build_solution_graph` puts non-step facts on
        // the solution node, so a graph that legitimately declared temporal validation was
        // refused for not declaring it.
        bool has_temporal_validation = false;
        for (const auto& other : graph.nodes()) {
            if (!other->attributes.is_object()) continue;
            const auto validated = other->attributes.find("temporal_validation");
            if (validated != other->attributes.end() && validated->is_boolean() &&
                validated->get<bool>()) {
                has_temporal_validation = true;
                break;
            }
        }
        if (!has_temporal_validation) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) + ": '" + named +
                    "' is time-ordered and must be validated over time. Add "
                    "`temporal_validation` to this step — a figure measured on rows held out by "
                    "content is not comparable to anything this model will see in production."};
        }
    }
    return CheckResult{name(), CheckStatus::Pass,
                       "every time-ordered model is validated over time and uncalibrated"};
}

CheckResult LabelEfficiencyGoldIsolationCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string()) {
            continue;
        }
        const std::string model_kind = kind->get<std::string>();
        if (model_kind != "label_efficiency" &&
            model_kind != "active_learning" &&
            model_kind != "weak_supervision") {
            continue;
        }
        const auto weak_as_gold =
            node->attributes.find("weak_labels_are_gold");
        if (weak_as_gold == node->attributes.end() ||
            !weak_as_gold->is_boolean() || weak_as_gold->get<bool>()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": weak labels must never be treated as gold; set "
                    "`weak_labels_are_gold` to false"};
        }
        const auto split = node->attributes.find("gold_split");
        if (split == node->attributes.end() || !split->is_string() ||
            split->get<std::string>() != "disjoint") {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": the gold evaluation set must be disjoint; set "
                    "`gold_split` to `disjoint`"};
        }
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        "label-efficiency gold rows are declared disjoint from weak labels"};
}

const std::vector<std::string>& MethodologyCheck::implemented() {
    static const std::vector<std::string> kImplemented = [] {
        std::vector<std::string> result;
        for (const zetta::ml::Methodology& methodology :
             zetta::ml::methodologies()) {
            if (!methodology.runnable)
                continue;
            result.insert(result.end(), methodology.model_kinds.begin(),
                          methodology.model_kinds.end());
        }
        return result;
    }();
    return kImplemented;
}

CheckResult MethodologyCheck::evaluate(const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model) continue;
        if (!node->attributes.is_object()) continue;
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end()) continue;  // opt-in, like every check here
        if (!kind->is_string()) {
            return CheckResult{name(), CheckStatus::Fail,
                               node_label(*node) + ": model_kind must be a string"};
        }
        const std::string named = kind->get<std::string>();
        const std::vector<std::string>& ok = implemented();
        if (std::find(ok.begin(), ok.end(), named) == ok.end()) {
            if (const zetta::ml::Methodology* methodology =
                    zetta::ml::find_methodology(named)) {
                return CheckResult{
                    name(), CheckStatus::Fail,
                    node_label(*node) + ": " +
                        zetta::ml::unsupported_methodology_message(
                            *methodology)};
            }
            std::string supported;
            for (const std::string& entry : ok) {
                if (!supported.empty()) supported += ", ";
                supported += entry;
            }
            // Say what to do, not just what is wrong (FEAT-3).
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) + ": this node names the methodology '" + named +
                    "', which this engine cannot execute. Implemented today: " + supported +
                    ". Either build the pipeline with an implemented methodology, or track the "
                    "missing one on the methodology portfolio issue (#984)."};
        }
    }
    return CheckResult{name(), CheckStatus::Pass,
                       "every model step names a methodology the engine can execute"};
}

CheckResult StreamingCorrectnessCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    const auto fail = [this](const ir::Node& node, std::string reason) {
        return CheckResult{name(), CheckStatus::Fail,
                           node_label(node) + ": " + std::move(reason)};
    };
    for (const auto& node : graph.nodes()) {
        if (!node || node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        if (auto error = validate_annotations(node->attributes)) {
            return fail(*node,
                        "malformed correctness annotations — " + *error);
        }
        const std::string kind =
            node->attributes.value("model_kind", std::string{});
        if (kind != "streaming" && kind != "online_learning") {
            continue;
        }
        if (!node->attributes.value("event_time_ordered", false)) {
            return fail(*node,
                        "streaming decisions must set event_time_ordered=true");
        }
        if (!node->attributes.contains("allowed_lateness")) {
            return fail(*node,
                        "streaming decisions must declare allowed_lateness");
        }
        if (node->attributes.value("future_event_policy", std::string{}) !=
            "buffer_until_watermark") {
            return fail(*node,
                        "future_event_policy must be buffer_until_watermark");
        }
        if (node->attributes.value("label_time_policy", std::string{}) !=
            "not_before_event_time") {
            return fail(*node,
                        "label_time_policy must be not_before_event_time");
        }
        if (!node->attributes.value("prequential", false)) {
            return fail(*node,
                        "streaming evaluation must set prequential=true");
        }
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        "streaming models order event time, delay labels, and evaluate test-then-train"};
}

CheckResult PrivacyBudgetCheck::evaluate(const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const ml::Methodology* methodology = ml::find_methodology(
            node->attributes.value("model_kind", std::string{}));
        if (methodology == nullptr || methodology->id != "M-20") {
            continue;
        }
        const auto fail = [&](std::string reason) {
            return CheckResult{name(), CheckStatus::Fail,
                               node_label(*node) + ": " + std::move(reason)};
        };
        const auto number = [&](const char* key) -> std::optional<double> {
            const auto value = node->attributes.find(key);
            if (value == node->attributes.end() || !value->is_number()) {
                return std::nullopt;
            }
            return value->get<double>();
        };
        const std::optional<double> epsilon = number("epsilon");
        const std::optional<double> delta = number("delta");
        const std::optional<double> maximum_epsilon = number("maximum_epsilon");
        const std::optional<double> maximum_delta = number("maximum_delta");
        const std::optional<double> clip_min = number("clip_min");
        const std::optional<double> clip_max = number("clip_max");
        if (!epsilon || !delta || !maximum_epsilon || !maximum_delta) {
            return fail("epsilon, delta, maximum_epsilon, and maximum_delta must all be stated");
        }
        if (*epsilon <= 0.0 || *delta < 0.0 || *maximum_epsilon <= 0.0 ||
            *maximum_delta < 0.0 || *delta >= 1.0 || *maximum_delta >= 1.0) {
            return fail("privacy-budget values are outside their valid ranges");
        }
        if (*epsilon > *maximum_epsilon || *delta > *maximum_delta) {
            return fail("requested epsilon/delta exceeds the enforced privacy budget");
        }
        if (!clip_min || !clip_max || *clip_min >= *clip_max) {
            return fail("public clip_min and clip_max bounds are required for DP sensitivity");
        }
        const auto noise = node->attributes.find("noise");
        if (noise == node->attributes.end() || !noise->is_string() ||
            (noise->get<std::string>() != "gaussian" &&
             noise->get<std::string>() != "laplace")) {
            return fail("noise must state the gaussian or laplace mechanism");
        }
        if (noise->get<std::string>() == "gaussian" && *delta <= 0.0) {
            return fail("the gaussian mechanism requires delta greater than zero");
        }
        const auto evaluation = node->attributes.find("privacy_evaluation");
        if (evaluation == node->attributes.end() || !evaluation->is_boolean() ||
            !evaluation->get<bool>()) {
            return fail("privacy_evaluation must be true before synthetic output publication");
        }
    }
    return CheckResult{name(), CheckStatus::Pass,
                       "every synthetic_private step states and enforces its privacy budget"};
}

CheckResult EnsembleOutOfFoldCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const std::optional<std::string> kind =
            string_field(node->attributes, "model_kind");
        if (!kind || (*kind != "ensemble" && *kind != "ensembling" &&
                      *kind != "stacking")) {
            continue;
        }
        const std::optional<std::string> predictions =
            string_field(node->attributes, "stacking_predictions");
        if (!predictions || *predictions != "out_of_fold") {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": ensemble stacking_predictions must be `out_of_fold`; "
                    "the meta-model may never fit in-sample base scores"};
        }
        const auto folds = node->attributes.find("folds");
        if (folds == node->attributes.end() ||
            !folds->is_number_integer() || folds->get<long long>() < 2) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": ensemble folds must be an integer of at least 2"};
        }
        const std::optional<std::string> refit =
            string_field(node->attributes, "base_refit");
        if (!refit || *refit != "all_training") {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": ensemble base_refit must be `all_training` after the "
                    "stacker is fit"};
        }
        const std::optional<std::string> time =
            string_field(node->attributes, "time_column");
        if (time && !time->empty()) {
            const std::optional<std::string> strategy =
                string_field(node->attributes, "fold_strategy");
            if (!strategy || *strategy != "time_ordered") {
                return CheckResult{
                    name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": an ensemble with a time column must use "
                        "`time_ordered` folds so validation never precedes training"};
            }
        }
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        "every ensemble fits only out-of-fold scores and respects time order"};
}


CheckResult UncertaintyValidationCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto model_kind = node->attributes.find("model_kind");
        if (model_kind == node->attributes.end() ||
            !model_kind->is_string()) {
            continue;
        }
        const std::string normalized_model_kind =
            normalize_type(model_kind->get<std::string>());
        if (normalized_model_kind != "bayesian" &&
            normalized_model_kind != "probabilistic" &&
            normalized_model_kind != "uncertainty") {
            continue;
        }
        const auto coverage = node->attributes.find("coverage_checked");
        if (coverage == node->attributes.end() || !coverage->is_boolean() ||
            !coverage->get<bool>()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": Bayesian uncertainty must set coverage_checked=true; "
                    "nominal coverage is not evidence of observed coverage"};
        }
        const auto assumption =
            node->attributes.find("exchangeability_assumption");
        if (assumption == node->attributes.end() ||
            !assumption->is_string() || assumption->get<std::string>().empty()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": Bayesian uncertainty must state its "
                    "exchangeability_assumption"};
        }
        const auto nominal = node->attributes.find("nominal_coverage");
        if (nominal == node->attributes.end() || !nominal->is_number() ||
            nominal->get<double>() <= 0.0 || nominal->get<double>() >= 1.0) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": nominal_coverage must be a number between 0 and 1"};
        }
        const auto uncertainty_kind =
            node->attributes.find("uncertainty_kind");
        if (uncertainty_kind == node->attributes.end() ||
            !uncertainty_kind->is_string()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": uncertainty_kind must name hierarchical or split_conformal"};
        }
        const std::string normalized_uncertainty_kind =
            normalize_type(uncertainty_kind->get<std::string>());
        const bool is_conformal =
            normalized_uncertainty_kind == "split_conformal_regression" ||
            normalized_uncertainty_kind == "split_conformal_classification";
        if (normalized_uncertainty_kind != "hierarchical" && !is_conformal) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": uncertainty_kind must name hierarchical, "
                    "split_conformal_regression, or "
                    "split_conformal_classification"};
        }
        if (is_conformal) {
            const auto disjoint =
                node->attributes.find("calibration_disjoint");
            if (disjoint == node->attributes.end() ||
                !disjoint->is_boolean() || !disjoint->get<bool>()) {
                return CheckResult{
                    name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": split conformal calibration rows must be declared "
                        "disjoint from fitting rows"};
            }
        }
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        "Bayesian model steps measure coverage and state exchangeability"};
}


CheckResult EntityResolutionGraphCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string()) {
            continue;
        }
        const std::string model_kind = kind->get<std::string>();
        if (model_kind != "entity_resolution" &&
            model_kind != "entity-resolution") {
            continue;
        }

        const auto blocking = node->attributes.find("blocking_keys");
        if (blocking == node->attributes.end() || !blocking->is_array() ||
            blocking->empty()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": entity resolution must declare non-empty blocking_keys"};
        }
        const auto match = node->attributes.find("match_threshold");
        const auto review = node->attributes.find("review_threshold");
        if (match == node->attributes.end() || !match->is_number() ||
            review == node->attributes.end() || !review->is_number()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": entity resolution must state numeric match_threshold and "
                    "review_threshold"};
        }
        const double match_value = match->get<double>();
        const double review_value = review->get<double>();
        if (review_value < 0.0 || match_value > 1.0 ||
            review_value >= match_value) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": thresholds must satisfy 0 <= review < match <= 1"};
        }
        const auto policy = node->attributes.find("conflict_policy");
        if (policy == node->attributes.end() || !policy->is_string() ||
            policy->get<std::string>() !=
                "reject_verified_identifier_conflicts") {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": entity resolution must reject verified identifier "
                    "conflicts"};
        }
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        "every entity-resolution model states safe blocking, thresholds, and "
        "conflict handling"};
}

CheckResult SurvivalCensoringCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string()) {
            continue;
        }
        const std::string model_kind = kind->get<std::string>();
        if (model_kind != "survival" && model_kind != "time_to_event") {
            continue;
        }
        if (const auto error = validate_annotations(node->attributes)) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": malformed correctness annotations — " + *error};
        }
        const auto requires_true = [&](const char* field) {
            const auto found = node->attributes.find(field);
            return found != node->attributes.end() && found->is_boolean() &&
                   found->get<bool>();
        };
        const auto requires_string = [&](const char* field) {
            const auto found = node->attributes.find(field);
            return found != node->attributes.end() && found->is_string() &&
                   !found->get_ref<const std::string&>().empty();
        };
        if (!requires_string("duration_column") ||
            !requires_string("event_indicator") ||
            !requires_true("censoring_handled") ||
            !requires_true("stop_after_event") ||
            !requires_true("point_in_time_features")) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": survival requires duration_column, event_indicator, "
                        "censoring_handled=true, stop_after_event=true, and "
                        "point_in_time_features=true"};
        }
    }
    return {name(), CheckStatus::Pass,
            "survival steps preserve censoring and point-in-time boundaries"};
}

CheckResult OptimizationContractCheck::evaluate(const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model || !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string()) {
            continue;
        }
        const std::string model_kind = kind->get<std::string>();
        if (model_kind != "optimization" && model_kind != "operations_research") {
            continue;
        }
        const auto requires_true = [&](std::string_view attribute,
                                       std::string reason) {
            const auto found = node->attributes.find(attribute);
            if (found == node->attributes.end() || !found->is_boolean() ||
                !found->get<bool>()) {
                return CheckResult{name(), CheckStatus::Fail,
                                   node_label(*node) + ": " + reason};
            }
            return CheckResult{name(), CheckStatus::Pass, {}};
        };
        CheckResult result = requires_true(
            "scenario_input",
            "optimization requires scenario or quantile inputs; a point "
            "forecast cannot represent plan risk");
        if (result.status == CheckStatus::Fail) {
            return result;
        }
        result = requires_true(
            "verify_constraints",
            "optimization must independently verify every bound and "
            "constraint before returning a plan");
        if (result.status == CheckStatus::Fail) {
            return result;
        }
        result = requires_true(
            "report_solver_status",
            "optimization must report solver status and the MIP optimality gap");
        if (result.status == CheckStatus::Fail) {
            return result;
        }
    }
    return CheckResult{name(), CheckStatus::Pass,
                       "optimization plans require scenarios, independent "
                       "verification, and status"};
}

CheckResult SchemaTypeCheck::evaluate(const ir::ExecutionGraph& graph) const {
    const auto fail = [this](std::string reason) {
        return CheckResult{name(), CheckStatus::Fail, std::move(reason)};
    };
    const auto pass = [this](std::string reason) {
        return CheckResult{name(), CheckStatus::Pass, std::move(reason)};
    };

    // Boundary: reject malformed annotations before reading any of them.
    for (const auto& node : graph.nodes()) {
        if (auto error = validate_annotations(node->attributes)) {
            return fail(node_label(*node) +
                        ": malformed correctness annotations — " + *error);
        }
    }

    std::size_t checked = 0;
    for (const auto& node : graph.nodes()) {
        if (!node->attributes.is_object()) continue;
        const auto requires_field = node->attributes.find("requires");
        if (requires_field == node->attributes.end() ||
            !requires_field->is_object() || requires_field->empty()) {
            continue;
        }
        ++checked;

        const auto provided = ancestor_schema(graph, node->id);
        for (const auto& [field, wanted_type] : requires_field->items()) {
            const std::string wanted = wanted_type.get<std::string>();
            const auto source = provided.find(field);
            if (source == provided.end()) {
                return fail(node_label(*node) + ": requires field \"" + field +
                            "\" (" + wanted +
                            ") but no upstream data source provides it");
            }
            const std::string wanted_norm = normalize_type(wanted);
            const bool matches = std::any_of(
                source->second.begin(), source->second.end(),
                [&](const std::string& have) {
                    return normalize_type(have) == wanted_norm;
                });
            if (!matches) {
                return fail(node_label(*node) + ": requires field \"" + field +
                            "\" as " + wanted +
                            " but the upstream source provides it as " +
                            join_types(source->second) + " (type mismatch)");
            }
        }
    }

    if (checked == 0) {
        return pass("no step declares schema requirements");
    }
    return pass(std::to_string(checked) +
                " step(s) type-check against their source profiles");
}

CheckResult DataLeakageCheck::evaluate(const ir::ExecutionGraph& graph) const {
    const auto fail = [this](std::string reason) {
        return CheckResult{name(), CheckStatus::Fail, std::move(reason)};
    };
    const auto pass = [this](std::string reason) {
        return CheckResult{name(), CheckStatus::Pass, std::move(reason)};
    };

    // Boundary: reject malformed annotations before reading any of them.
    for (const auto& node : graph.nodes()) {
        if (auto error = validate_annotations(node->attributes)) {
            return fail(node_label(*node) +
                        ": malformed correctness annotations — " + *error);
        }
    }

    std::size_t checked = 0;
    for (const auto& node : graph.nodes()) {
        if (!node->attributes.is_object()) continue;
        const json& attributes = node->attributes;
        const auto features = attributes.find("features");
        const bool has_features = features != attributes.end() &&
                                  features->is_array() && !features->empty();
        if (!has_features) continue;  // nothing feature-level to validate

        const std::optional<std::string> as_of = string_field(attributes, "as_of");
        if (!as_of) {
            return fail(node_label(*node) +
                        ": declares features but no \"as_of\" cutoff to validate "
                        "point-in-time correctness against");
        }
        ++checked;

        const std::optional<std::string> label = string_field(attributes, "label");
        const std::string cutoff = comparable_instant(*as_of);
        for (const json& feature : *features) {
            const std::string feature_name = feature.value("name", std::string{});

            // Target leakage: the feature is, or is derived from, the label.
            const std::optional<std::string> derived_from =
                string_field(feature, "derived_from");
            const bool flagged = feature.value("leaks_target", false);
            const bool is_label = label && feature_name == *label;
            const bool from_label =
                derived_from && (*derived_from == "label" ||
                                 (label && *derived_from == *label));
            if (flagged || is_label || from_label) {
                const std::string target = label ? ("\"" + *label + "\"")
                                                 : std::string("the target");
                return fail(node_label(*node) + ": feature \"" + feature_name +
                            "\" is derived from the label (" + target +
                            ") — target leakage");
            }

            // Point-in-time: the feature draws on data dated after the cutoff.
            const std::optional<std::string> data_as_of =
                string_field(feature, "data_as_of");
            if (data_as_of && comparable_instant(*data_as_of) > cutoff) {
                return fail(node_label(*node) + ": feature \"" + feature_name +
                            "\" uses data as of " + *data_as_of +
                            ", after the as_of cutoff " + *as_of +
                            " — point-in-time violation (data leakage)");
            }
        }
    }

    if (checked == 0) {
        return pass("no ML step declares features to check for leakage");
    }
    return pass(std::to_string(checked) +
                " ML step(s) are point-in-time correct and target-leakage free");
}

CheckResult SnapshotLineageCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    const auto fail = [this](std::string reason) {
        return CheckResult{name(), CheckStatus::Fail, std::move(reason)};
    };
    const auto pass = [this](std::string reason) {
        return CheckResult{name(), CheckStatus::Pass, std::move(reason)};
    };

    for (const auto& node : graph.nodes()) {
        if (auto error = validate_annotations(node->attributes)) {
            return fail(node_label(*node) +
                        ": malformed correctness annotations — " + *error);
        }
    }

    std::size_t checked = 0;
    for (const std::shared_ptr<ir::Node>& model : graph.nodes()) {
        if (!model || model->kind != ir::StepKind::Model ||
            !model->attributes.is_object()) {
            continue;
        }
        const auto features = model->attributes.find("features");
        if (features == model->attributes.end() || !features->is_array() ||
            features->empty()) {
            continue;
        }
        ++checked;
        SnapshotBoundary boundary;
        if (const auto error = snapshot_window_error(*model, boundary)) {
            return fail(*error);
        }
        if (const auto error = snapshot_source_error(graph, *model, boundary)) {
            return fail(*error);
        }
    }

    if (checked == 0) {
        return pass("no model step declares features that require snapshot lineage");
    }
    return pass(std::to_string(checked) +
                " feature-bearing model step(s) terminate in valid snapshots");
}

CheckResult CausalUpliftCheck::evaluate(const ir::ExecutionGraph& graph) const {
    const auto fail = [this](std::string reason) {
        return CheckResult{name(), CheckStatus::Fail, std::move(reason)};
    };
    std::size_t checked = 0;
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model || !node->attributes.is_object()) {
            continue;
        }
        const json& attributes = node->attributes;
        const std::string kind = attributes.value("model_kind", std::string{});
        if (kind != "uplift" && kind != "causal") {
            continue;
        }
        ++checked;
        if (const auto error = validate_annotations(attributes)) {
            return fail(node_label(*node) +
                        ": malformed correctness annotations — " + *error);
        }
        const std::optional<std::string> treatment =
            string_field(attributes, "treatment");
        const std::optional<std::string> outcome =
            string_field(attributes, "outcome");
        if (!treatment || !outcome) {
            return fail(node_label(*node) +
                        ": uplift requires explicit treatment and outcome columns");
        }
        if (*treatment == *outcome) {
            return fail(node_label(*node) +
                        ": treatment and outcome must be different columns");
        }
        const double lower = attributes.value("overlap_lower", 0.05);
        const double upper = attributes.value("overlap_upper", 0.95);
        if (lower < 0.05 || upper > 0.95 || lower >= upper) {
            return fail(node_label(*node) +
                        ": overlap bounds must stay within [0.05,0.95] and be ordered");
        }

        const std::optional<std::string> treatment_time =
            string_field(attributes, "treatment_time");
        const auto features = attributes.find("features");
        if (features == attributes.end() || !features->is_array()) {
            continue;
        }
        for (const json& feature : *features) {
            const std::string feature_name = feature.value("name", std::string{});
            const std::string normalized_feature = normalize_type(feature_name);
            if (feature.value("post_treatment", false)) {
                return fail(node_label(*node) + ": feature \"" + feature_name +
                            "\" is observed after treatment");
            }
            const std::string normalized_outcome = normalize_type(*outcome);
            const bool outcome_like =
                normalized_feature == normalized_outcome ||
                normalized_feature.starts_with(normalized_outcome + "_") ||
                normalized_feature.ends_with("_" + normalized_outcome);
            if (outcome_like) {
                return fail(node_label(*node) + ": feature \"" + feature_name +
                            "\" is named like outcome \"" + *outcome + "\"");
            }
            if (feature_name == *treatment) {
                return fail(node_label(*node) + ": treatment column \"" +
                            *treatment + "\" cannot be a feature");
            }
            const std::optional<std::string> data_as_of =
                string_field(feature, "data_as_of");
            if (treatment_time && data_as_of &&
                comparable_instant(*data_as_of) >
                    comparable_instant(*treatment_time)) {
                return fail(node_label(*node) + ": feature \"" + feature_name +
                            "\" is timestamped after treatment");
            }
        }
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        checked == 0
            ? "no causal uplift step to check"
            : "causal uplift roles, overlap, and feature timing are valid"};
}

CheckResult RankCorrectnessCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    std::size_t checked = 0;
    for (const auto& node : graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!node->attributes.is_object()) continue;
        const std::optional<std::string> kind =
            string_field(node->attributes, "model_kind");
        if (!kind || (*kind != "rank" && *kind != "ranking" &&
                      *kind != "learning_to_rank")) {
            continue;
        }
        ++checked;
        const std::optional<std::string> group =
            string_field(node->attributes, "group_column");
        if (!group || group->empty()) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": learning-to-rank requires a non-empty group_column"};
        }
        const std::optional<std::string> split =
            string_field(node->attributes, "split_strategy");
        if (!split || *split != "group") {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": split_strategy must be \"group\" so one group never "
                        "appears in both training and validation"};
        }
        const std::optional<std::string> availability =
            string_field(node->attributes, "feature_availability");
        if (!availability || *availability != "ranking_time") {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": feature_availability must be \"ranking_time\"; later "
                        "information would leak into the queue"};
        }
    }
    return {name(), CheckStatus::Pass,
            checked == 0
                ? "no learning-to-rank model to validate"
                : std::to_string(checked) +
                      " rank model(s) require groups, whole-group splits, and "
                      "ranking-time features"};
}

CheckResult LanguageGroundingCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (!language_model(*node)) continue;
        if (auto error = validate_annotations(node->attributes)) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": malformed correctness annotations — " + *error};
        }
        if (!true_field(node->attributes, "citations_required")) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": M-10 requires citations on every answer sentence"};
        }
        if (!true_field(node->attributes, "refuse_without_retrieval")) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": M-10 must refuse when retrieval is empty or irrelevant"};
        }
    }
    return {name(), CheckStatus::Pass,
            "every language model requires grounded citations and refusal"};
}

CheckResult LanguageTrainingAccessCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (!language_model(*node)) continue;
        if (auto error = validate_annotations(node->attributes)) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": malformed correctness annotations — " + *error};
        }
        if (!true_field(node->attributes, "governance_check")) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": M-10 must authorize every training column before reading text"};
        }
        if (!upstream_requires_caller_read(graph, node->id)) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": M-10 requires an upstream source with caller-read enforcement"};
        }
    }
    return {name(), CheckStatus::Pass,
            "every language model requires caller-authorized training text"};
}

CheckResult DocumentReviewCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string() ||
            kind->get<std::string>() != "document_extraction") {
            continue;
        }
        const auto confidence =
            node->attributes.find("confidence_required");
        if (confidence == node->attributes.end() ||
            !confidence->is_boolean() || !confidence->get<bool>()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": document extraction must assign a confidence to every field"};
        }
        const auto evidence = node->attributes.find("retain_evidence");
        if (evidence == node->attributes.end() || !evidence->is_boolean() ||
            !evidence->get<bool>()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": document extraction must retain each field's source page and box"};
        }
        const auto routing =
            node->attributes.find("route_low_confidence_to_review");
        if (routing == node->attributes.end() || !routing->is_boolean() ||
            !routing->get<bool>()) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": fields below the review threshold must be routed to review"};
        }
        const auto threshold = node->attributes.find("review_threshold");
        if (threshold == node->attributes.end() || !threshold->is_number() ||
            !std::isfinite(threshold->get<double>()) ||
            threshold->get<double>() < 0.0 ||
            threshold->get<double>() > 1.0) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": review_threshold must be a number between 0 and 1"};
        }
        bool has_review_step = false;
        for (const auto& candidate : graph.successors(node->id)) {
            if (candidate->kind != ir::StepKind::Act ||
                !candidate->attributes.is_object()) {
                continue;
            }
            const auto route = candidate->attributes.find("route");
            if (route != candidate->attributes.end() && route->is_string() &&
                route->get<std::string>() == "review_queue") {
                has_review_step = true;
                break;
            }
        }
        if (!has_review_step) {
            return CheckResult{
                name(), CheckStatus::Fail,
                node_label(*node) +
                    ": document extraction needs an Act step that writes the review_queue"};
        }
    }
    return CheckResult{
        name(), CheckStatus::Pass,
        "document fields keep confidence and evidence, with uncertain values routed to review"};
}

CheckResult GraphEdgeTimeCheck::evaluate(const ir::ExecutionGraph& graph) const {
    for (const auto& model : graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!model || !model->attributes.is_object()) continue;
        const std::string kind =
            model->attributes.value("model_kind", std::string{});
        if (kind != "graph" && kind != "graph_ml") continue;

        const auto time = model->attributes.find("time_column");
        const auto cutoff = model->attributes.find("prediction_time");
        if (time == model->attributes.end() || !time->is_string() ||
            time->get<std::string>().empty()) {
            return {name(), CheckStatus::Fail,
                    "graph model must name its edge time_column"};
        }
        if (cutoff == model->attributes.end() || !cutoff->is_number() ||
            !std::isfinite(cutoff->get<double>())) {
            return {name(), CheckStatus::Fail,
                    "graph model must name a finite prediction_time cutoff"};
        }

        for (const auto& clean : graph.nodes_of_kind(ir::StepKind::Clean)) {
            if (!clean || !clean->attributes.is_object()) continue;
            const std::vector<ir::Edge> outgoing = graph.edges_from(clean->id);
            const bool feeds_model =
                std::any_of(outgoing.begin(), outgoing.end(),
                            [&](const ir::Edge& edge) {
                                return edge.to == model->id &&
                                       edge.kind == ir::EdgeKind::DataFlow;
                            });
            if (!feeds_model) continue;
            const auto filter_time =
                clean->attributes.find("filter_time_column");
            const auto filter_cutoff = clean->attributes.find("prediction_time");
            if (filter_time == clean->attributes.end() ||
                !filter_time->is_string() ||
                filter_time->get<std::string>() != time->get<std::string>()) {
                continue;
            }
            if (filter_cutoff == clean->attributes.end() ||
                !filter_cutoff->is_number() ||
                filter_cutoff->get<double>() != cutoff->get<double>()) {
                continue;
            }
            if (!clean->attributes.value("exclude_after_prediction_time", false)) {
                return {name(), CheckStatus::Fail,
                        "graph edge-time filter is disabled"};
            }
            return {name(), CheckStatus::Pass,
                    "graph edges are filtered at the prediction cutoff"};
        }
        return {name(), CheckStatus::Fail,
                "graph model requires an edge-time filter with the same "
                "column and prediction cutoff"};
    }
    return {name(), CheckStatus::Pass, "no graph model step to check"};
}

CheckResult VisionSourceSplitCheck::evaluate(
    const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (auto error = validate_annotations(node->attributes)) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": malformed correctness annotations — " + *error};
        }
        const auto model_kind = node->attributes.find("model_kind");
        if (model_kind == node->attributes.end() ||
            !model_kind->is_string()) {
            continue;
        }
        std::string kind = model_kind->get<std::string>();
        std::transform(kind.begin(), kind.end(), kind.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (kind != "vision" && kind != "computer_vision") {
            continue;
        }
        const auto disjoint = node->attributes.find("source_disjoint");
        if (disjoint == node->attributes.end() || !disjoint->is_boolean() ||
            !disjoint->get<bool>()) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": vision evaluation must set source_disjoint=true; "
                        "splitting frames can put one source in both train and "
                        "test"};
        }
        const auto source = node->attributes.find("source_column");
        if (source == node->attributes.end() || !source->is_string() ||
            source->get<std::string>().empty()) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": source_column must name the camera, site, document, "
                        "or other group held out as a unit"};
        }
    }
    return {name(), CheckStatus::Pass,
            "every vision model holds out complete source groups"};
}

CheckResult SpeechConsentCheck::evaluate(const ir::ExecutionGraph& graph) const {
    for (const auto& node : graph.nodes()) {
        if (!node || !speech_step(*node)) {
            continue;
        }
        const auto required = node->attributes.find("consent_required");
        const auto column = node->attributes.find("consent_column");
        if (required == node->attributes.end() || !required->is_boolean() ||
            !required->get<bool>() || column == node->attributes.end() ||
            !column->is_string() || column->empty()) {
            return {name(), CheckStatus::Fail,
                    node_label(*node) +
                        ": speech requires explicit consent and a consent_column"};
        }
    }
    return {name(), CheckStatus::Pass,
            "every speech model requires an explicit consent field"};
}

CheckResult SpeechPiiRedactionCheck::evaluate(const ir::ExecutionGraph& graph) const {
    return require_speech_flag(
        graph, name(), "redact_pii",
        "speech transcript PII redaction must be enabled",
        "every speech model redacts transcript PII before storage");
}

CheckResult SpeechLocalAudioCheck::evaluate(const ir::ExecutionGraph& graph) const {
    return require_speech_flag(
        graph, name(), "audio_local_only",
        "speech audio must remain on its owner node",
        "every speech model keeps audio on its owner node");
}

CheckResult BanditPolicyCheck::evaluate(const ir::ExecutionGraph& graph) const {
    const auto fail = [this](const ir::Node& node, std::string reason) {
        return CheckResult{name(), CheckStatus::Fail,
                           node_label(node) + ": " + std::move(reason)};
    };
    std::size_t checked = 0;
    for (const auto& node : graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!node || !node->attributes.is_object()) {
            continue;
        }
        const json& attributes = node->attributes;
        const std::string kind = attributes.value("model_kind", "");
        if (kind != "bandit" && kind != "bandits" &&
            kind != "sequential_decisioning") {
            continue;
        }
        ++checked;
        const auto floor_value = attributes.find("exploration_floor");
        if (floor_value == attributes.end() || !floor_value->is_number()) {
            return fail(*node, "exploration_floor must be declared");
        }
        const double floor = floor_value->get<double>();
        if (!std::isfinite(floor) || !(floor > 0.0) || floor > 1.0) {
            return fail(*node,
                        "exploration_floor must be finite and in (0, 1]");
        }
        const auto default_cap_value = attributes.find("default_arm_cap");
        if (default_cap_value == attributes.end() ||
            !default_cap_value->is_number()) {
            return fail(*node, "default_arm_cap must be declared");
        }
        const double default_cap = default_cap_value->get<double>();
        if (!std::isfinite(default_cap) || default_cap < floor ||
            default_cap > 1.0) {
            return fail(*node,
                        "default_arm_cap must be finite and no lower than "
                        "exploration_floor");
        }

        const json caps = attributes.value("arm_caps", json::object());
        if (!caps.is_object()) {
            return fail(*node, "arm_caps must be an object");
        }
        for (const auto& [arm, value] : caps.items()) {
            if (!value.is_number()) {
                return fail(*node, "cap for arm '" + arm +
                                       "' must be numeric");
            }
            const double cap = value.get<double>();
            if (!std::isfinite(cap) || cap < floor || cap > 1.0) {
                return fail(*node, "cap for arm '" + arm +
                                       "' must be in [exploration_floor, 1]");
            }
        }

        const auto arms_value = attributes.find("arms");
        if (arms_value == attributes.end()) {
            continue;
        }
        if (!arms_value->is_array() || arms_value->size() < 2) {
            return fail(*node, "arms must declare at least two values");
        }
        if (floor * static_cast<double>(arms_value->size()) > 1.0) {
            return fail(*node,
                        "exploration floors exceed the available probability");
        }
        double total_cap = 0.0;
        for (const json& arm_value : *arms_value) {
            if (!arm_value.is_string()) {
                return fail(*node, "every declared arm must be a string");
            }
            const std::string arm = arm_value.get<std::string>();
            const auto cap = caps.find(arm);
            total_cap += cap == caps.end() ? default_cap : cap->get<double>();
        }
        if (total_cap < 1.0) {
            return fail(*node,
                        "arm caps cannot carry total probability 1");
        }
    }
    if (checked == 0) {
        return CheckResult{name(), CheckStatus::Pass,
                           "no bandit policy requires allocation checks"};
    }
    return CheckResult{name(), CheckStatus::Pass,
                       "every bandit policy has feasible floors and caps"};
}

} // namespace zetta::pipeline
