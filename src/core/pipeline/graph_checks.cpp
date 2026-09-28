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
  "description": "The correctness annotations read by the gate's concrete checks: schema profiles, point-in-time metadata, and M-10 grounding and read-authorization requirements. Only the named fields are constrained; a node may carry any other attributes.",
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

}  // namespace

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
