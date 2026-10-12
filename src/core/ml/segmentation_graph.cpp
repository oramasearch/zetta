#include "zetta/ml/segmentation_methodology.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json-schema.hpp>

#include "zetta/data/dataset_path.hpp"
#include "zetta/pipeline/graph_checks.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {
class ValidationErrors : public nlohmann::json_schema::error_handler {
  public:
    std::string reason;

    void error(const nlohmann::json::json_pointer &path, const nlohmann::json &,
               const std::string &message) override {
        if (reason.empty()) {
            reason = path.to_string() + ": " + message;
        }
    }
};
} // namespace

const nlohmann::json &segmentation_config_schema() {
    static const auto schema = nlohmann::json::parse(R"JSON({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.SegmentationConfig",
      "type": "object",
      "additionalProperties": false,
      "required": ["scaling", "encoding", "selection"],
      "properties": {
        "scaling": {"const": "global_auto"},
        "encoding": {"const": "bounded_one_hot"},
        "selection": {"const": "silhouette_then_davies_bouldin"},
        "entity_column": {"type": "string"},
        "label_column": {"type": "string"},
        "time_column": {"type": "string", "minLength": 1},
        "as_of": {
          "description": "inclusive cutoff as epoch seconds or an ISO-8601 UTC timestamp",
          "oneOf": [
            {"type": "number"},
            {"type": "string", "pattern": "^\\d{4}-\\d{2}-\\d{2}(T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?Z?)?$"}
          ]
        },
        "feature_columns": {"type": "array", "uniqueItems": true,
          "items": {"type": "string", "minLength": 1}},
        "categorical_columns": {"type": "array", "uniqueItems": true,
          "items": {"type": "string", "minLength": 1}},
        "excluded_columns": {"type": "array", "uniqueItems": true,
          "items": {"type": "string", "minLength": 1}},
        "minimum_k": {"type": "integer", "minimum": 2, "maximum": 64},
        "maximum_k": {"type": "integer", "minimum": 2, "maximum": 64},
        "restarts": {"type": "integer", "minimum": 2, "maximum": 32},
        "max_iterations": {"type": "integer", "minimum": 1, "maximum": 10000},
        "center_shift_tolerance": {"type": "number", "exclusiveMinimum": 0},
        "minimum_segment_share": {"type": "number", "minimum": 0.02, "maximum": 0.5},
        "silhouette_sample_size": {"type": "integer", "minimum": 3, "maximum": 8192},
        "seed": {"type": "integer", "minimum": 0, "maximum": 4294967295},
        "maximum_category_levels": {"type": "integer", "minimum": 2, "maximum": 256},
        "rare_category_share": {"type": "number", "minimum": 0, "exclusiveMaximum": 1}
      },
      "dependencies": {"time_column": ["as_of"], "as_of": ["time_column"]}
    })JSON");
    return schema;
}

SegmentationParams segmentation_params(const nlohmann::json &config) {
    SegmentationParams params;
    params.entity_column = config.value("entity_column", params.entity_column);
    params.label_column = config.value("label_column", params.label_column);
    params.feature_columns =
        config.value("feature_columns", params.feature_columns);
    params.categorical_columns =
        config.value("categorical_columns", params.categorical_columns);
    params.excluded_columns =
        config.value("excluded_columns", params.excluded_columns);
    params.minimum_k = config.value("minimum_k", params.minimum_k);
    params.maximum_k = config.value("maximum_k", params.maximum_k);
    params.restarts = config.value("restarts", params.restarts);
    params.max_iterations =
        config.value("max_iterations", params.max_iterations);
    params.center_shift_tolerance =
        config.value("center_shift_tolerance", params.center_shift_tolerance);
    params.minimum_segment_share =
        config.value("minimum_segment_share", params.minimum_segment_share);
    params.silhouette_sample_size =
        config.value("silhouette_sample_size", params.silhouette_sample_size);
    params.seed = config.value("seed", params.seed);
    params.maximum_category_levels =
        config.value("maximum_category_levels", params.maximum_category_levels);
    params.rare_category_share =
        config.value("rare_category_share", params.rare_category_share);
    return params;
}

bool validate_segmentation_config(const nlohmann::json &config,
                                  std::string &error) {
    static const nlohmann::json_schema::json_validator validator(
        segmentation_config_schema());
    ValidationErrors errors;
    validator.validate(config, errors);
    error = errors.reason;
    if (!error.empty()) {
        return false;
    }
    if (const auto cutoff = config.find("as_of");
        cutoff != config.end() && !zetta::util::parse_time_seconds(*cutoff)) {
        error = "as_of must be a finite epoch number or ISO-8601 timestamp";
        return false;
    }
    auto params = segmentation_params(config);
    const auto same_column = [](std::string left, std::string right) {
        for (auto *value : {&left, &right}) {
            std::transform(value->begin(), value->end(), value->begin(),
                           [](unsigned char character) {
                               return static_cast<char>(
                                   std::tolower(character));
                           });
        }
        return !left.empty() && left == right;
    };
    const bool outcome_key = std::any_of(
        data::label_column_candidates().begin(),
        data::label_column_candidates().end(), [&](const auto &column) {
            return same_column(params.entity_column, column);
        });
    if (outcome_key || same_column(params.entity_column, params.label_column) ||
        same_column(params.entity_column, config.value("time_column", ""))) {
        error = "entity key must not also be a label/outcome or time role";
        return false;
    }
    if (params.maximum_k < params.minimum_k) {
        error = "maximum_k must be at least minimum_k";
        return false;
    }
    params.excluded_columns.push_back(config.value("time_column", ""));
    for (const auto &column : params.feature_columns) {
        if (segmentation_excludes_column(column, params)) {
            error = "feature '" + column +
                    "' is an entity key, label/outcome, or excluded role";
            return false;
        }
    }
    for (const auto &column : params.categorical_columns) {
        if (segmentation_excludes_column(column, params)) {
            error = "categorical feature '" + column +
                    "' is an entity key, label/outcome, or excluded role";
            return false;
        }
    }
    return true;
}

bool is_segmentation_kind(const std::string &kind) {
    std::string normalized = kind;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return normalized == "segmentation" || normalized == "clustering" ||
           normalized == "cluster";
}
} // namespace zetta::ml

namespace zetta::pipeline {
namespace {
std::string
check_topology(const ir::ExecutionGraph &graph,
               const std::shared_ptr<ir::Node> &model,
               const std::shared_ptr<ir::Node> &prepare,
               const std::shared_ptr<ir::Node> &source,
               const std::vector<std::shared_ptr<ir::Node>> &outputs,
               const nlohmann::json &config) {
    const auto reject = [](const std::string &reason) {
        return reason;
    };
    if (!source || !prepare || outputs.size() != 2 || graph.node_count() != 5) {
        return reject("expected connect -> feature preparation -> model -> "
                      "assignment/profile outputs");
    }
    for (const auto *field : {"source", "table"}) {
        const auto value = source->attributes.find(field);
        if (value == source->attributes.end() || !value->is_string() ||
            value->get_ref<const std::string &>().empty()) {
            return reject(std::string("connect requires a nonempty ") + field);
        }
    }
    if (!prepare->attributes.is_object() ||
        prepare->attributes.value("segmentation", nlohmann::json{}) != config ||
        prepare->attributes.value("entity_key", nlohmann::json{}) !=
            "kept_aside") {
        return reject(
            "feature preparation must use the model's global "
            "scaling/encoding contract and keep the entity key aside");
    }
    std::set<std::string> output_kinds;
    std::set<std::pair<std::string, std::string>> expected_edges = {
        {source->id, prepare->id}, {prepare->id, model->id}};
    for (const auto &output : outputs) {
        const auto kind = output->attributes.find("output");
        if (kind == output->attributes.end() || !kind->is_string()) {
            return reject("outputs must name assignments and profiles");
        }
        output_kinds.insert(kind->get<std::string>());
        expected_edges.emplace(model->id, output->id);
    }
    if (output_kinds != std::set<std::string>{"assignments", "profiles"}) {
        return reject("outputs must name assignments and profiles");
    }
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto &edge : graph.edges()) {
        if (edge.kind != ir::EdgeKind::DataFlow) {
            return "segmentation connections must carry data";
        }
        edges.emplace(edge.from, edge.to);
    }
    if (edges != expected_edges) {
        return reject(
            "feature preparation and both outputs must be connected to "
            "the model");
    }

    return {};
}
} // namespace
std::string SegmentationCheck::name() const {
    return "segmentation";
}

CheckResult SegmentationCheck::evaluate(const ir::ExecutionGraph &graph) const {
    std::shared_ptr<ir::Node> model;
    std::shared_ptr<ir::Node> prepare;
    std::shared_ptr<ir::Node> source;
    std::vector<std::shared_ptr<ir::Node>> outputs;
    for (const auto &step : graph.nodes()) {
        const auto kind = step->attributes.find("model_kind");
        if (step->kind == ir::StepKind::Model &&
            kind != step->attributes.end() && kind->is_string() &&
            ml::is_segmentation_kind(kind->get<std::string>())) {
            model = step;
        }
    }
    if (!model) {
        return {name(), CheckStatus::NotApplicable, "no segmentation model"};
    }
    const auto fail = [&](const std::string &reason) {
        return CheckResult{name(), CheckStatus::Fail,
                           "segmentation: " + reason};
    };
    if (!model->attributes.contains("segmentation")) {
        return fail(
            "model requires its segmentation feature and training contract");
    }
    const auto &config = model->attributes.at("segmentation");
    std::string error;
    if (!ml::validate_segmentation_config(config, error)) {
        return fail(error);
    }
    auto params = ml::segmentation_params(config);
    for (const auto &step : graph.nodes()) {
        if (step->kind == ir::StepKind::Model && step != model) {
            return fail("exactly one model is supported");
        }
        if (step->kind == ir::StepKind::Connect) {
            if (source) {
                return fail("exactly one entity table is required");
            }
            source = step;
        }
        if (step->kind == ir::StepKind::Clean) {
            if (prepare) {
                return fail("exactly one feature preparation step is required");
            }
            prepare = step;
        }
        if (step->kind == ir::StepKind::Act) {
            outputs.push_back(step);
        }
        for (const auto *role :
             {"label", "target", "outcome", "entity_column"}) {
            const auto named = step->attributes.find(role);
            if (named != step->attributes.end() && named->is_string()) {
                params.excluded_columns.push_back(named->get<std::string>());
            }
        }
    }
    for (const auto &column : params.feature_columns) {
        if (ml::segmentation_excludes_column(column, params)) {
            return fail("feature '" + column +
                        "' is named as an entity key or label/outcome");
        }
    }
    for (const auto &column : params.categorical_columns) {
        if (ml::segmentation_excludes_column(column, params)) {
            return fail("categorical feature '" + column +
                        "' is named as an entity key or label/outcome");
        }
    }
    const auto topology =
        check_topology(graph, model, prepare, source, outputs, config);
    if (!topology.empty()) {
        return fail(topology);
    }
    return {
        name(), CheckStatus::Pass,
        "global preprocessing, excluded roles, k selection and output shape "
        "checked"};
}
} // namespace zetta::pipeline
