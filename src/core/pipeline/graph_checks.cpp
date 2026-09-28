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
  "description": "The correctness annotations the gate's concrete checks (#309) read from an ExecutionGraph node's free-form `attributes` bag: source/required schema profiles (schema_type check) and point-in-time / target-leakage metadata (data_leakage check). Only the named fields are constrained; a node may carry any other attributes. Mirrored verbatim by the inline schema in src/core/graph_checks.cpp (the runtime source of truth) — keep the two in sync.",
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

} // namespace zetta::pipeline
