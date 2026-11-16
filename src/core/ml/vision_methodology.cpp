// SPDX-License-Identifier: MIT

#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "zetta/api/registry.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/vision.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::ml {
namespace {

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

std::string hint_string(const nlohmann::json &hints, const char *name,
                        std::string fallback) {
  if (!hints.is_object()) {
    return fallback;
  }
  const auto found = hints.find(name);
  if (found == hints.end() || !found->is_string() || found->empty()) {
    return fallback;
  }
  return found->get<std::string>();
}

std::uint64_t hint_unsigned(const nlohmann::json &hints, const char *name,
                            std::uint64_t fallback) {
  if (!hints.is_object()) {
    return fallback;
  }
  const auto found = hints.find(name);
  if (found == hints.end() || !found->is_number_integer()) {
    return fallback;
  }
  const std::int64_t value = found->get<std::int64_t>();
  return value > 0 ? static_cast<std::uint64_t>(value) : fallback;
}

double hint_number(const nlohmann::json &hints, const char *name,
                   double fallback) {
  if (!hints.is_object()) {
    return fallback;
  }
  const auto found = hints.find(name);
  return found != hints.end() && found->is_number() ? found->get<double>()
                                                    : fallback;
}

ir::ExecutionGraph build_m13_shape(const ShapeRequest &request) {
  const std::string database =
      hint_string(request.hints, "database", "datasets");
  const std::string table = hint_string(request.hints, "table", "images");
  const std::string image = hint_string(request.hints, "image_column", "image");
  const std::string label = hint_string(request.hints, "label_column", "label");
  const std::string source =
      hint_string(request.hints, "source_column", "source");

  ir::ExecutionGraph graph;
  graph.add_node(
      ir::StepKind::Connect, "Load governed images",
      {{"source", database},
       {"table", table},
       {"schema", {{image, "string"}, {label, "integer"}, {source, "string"}}}},
      "load_images");
  graph.add_node(
      ir::StepKind::Clean, "Decode and normalize images",
      {{"transform", "decode_resize_normalize"},
       {"schema", {{image, "string"}, {label, "integer"}, {source, "string"}}}},
      "prepare_images");
  graph.add_node(
      ir::StepKind::Model, "Train source-disjoint vision classifier",
      {{"model_kind", "vision"},
       {"architecture", "vit_linear_transfer"},
       {"backbone_model", hint_string(request.hints, "backbone_model", "")},
       {"image_column", image},
       {"label_column", label},
       {"source_column", source},
       {"source_disjoint", true},
       {"holdout_fraction",
        hint_number(request.hints, "holdout_fraction", 0.25)},
       {"epochs", hint_unsigned(request.hints, "epochs", 80)},
       {"learning_rate", hint_number(request.hints, "learning_rate", 0.15)},
       {"seed", hint_unsigned(request.hints, "seed", 1)},
       {"requires",
        {{image, "string"}, {label, "integer"}, {source, "string"}}}},
      "train_vision");
  graph.add_node(ir::StepKind::Act, "Write predictions and evaluation report",
                 {{"output", "prediction_table"},
                  {"columns", {"image", "label", "predicted_label", "score"}}},
                 "write_vision_outputs");
  graph.add_edge("load_images", "prepare_images");
  graph.add_edge("prepare_images", "train_vision");
  graph.add_edge("train_vision", "write_vision_outputs");
  return graph;
}

bool m13_owns(const ir::ExecutionGraph &graph) {
  for (const std::shared_ptr<ir::Node> &node : graph.nodes()) {
    if (!node || node->kind != ir::StepKind::Model ||
        !node->attributes.is_object()) {
      continue;
    }
    const auto kind = node->attributes.find("model_kind");
    if (kind == node->attributes.end() || !kind->is_string()) {
      continue;
    }
    const std::string value = lower(kind->get<std::string>());
    if (value == "vision" || value == "computer_vision") {
      return true;
    }
  }
  return false;
}

std::optional<data::ResolvedDataset>
resolve_graph_dataset(Node &node, const ir::ExecutionGraph &graph,
                      std::string &error) {
  for (const std::shared_ptr<ir::Node> &step : graph.nodes()) {
    if (!step || step->kind != ir::StepKind::Connect ||
        !step->attributes.is_object()) {
      continue;
    }
    const auto source = step->attributes.find("source");
    const auto table = step->attributes.find("table");
    if (source == step->attributes.end() || !source->is_string() ||
        table == step->attributes.end() || !table->is_string()) {
      continue;
    }
    const std::string database_name = source->get<std::string>();
    const std::string table_name = table->get<std::string>();
    for (const data::DatasetSummary &dataset : node.datasets()) {
      if (dataset.database != database_name || dataset.name != table_name) {
        continue;
      }
      if (dataset.degraded) {
        error = "vision dataset '" + database_name + "." + table_name +
                "' is degraded: " + dataset.degraded_reason;
        return std::nullopt;
      }
      data::ResolvedDataset resolved;
      resolved.table_id = dataset.table_id;
      resolved.database = database_name;
      resolved.table = table_name;
      resolved.rows = dataset.rows;
      return resolved;
    }
    error = "vision dataset '" + database_name + "." + table_name +
            "' was not found";
    return std::nullopt;
  }
  error = "vision graph has no connect step naming source and table";
  return std::nullopt;
}

const nlohmann::json *vision_model_attributes(const ir::ExecutionGraph &graph) {
  for (const std::shared_ptr<ir::Node> &step : graph.nodes()) {
    if (!step || step->kind != ir::StepKind::Model ||
        !step->attributes.is_object()) {
      continue;
    }
    const auto kind = step->attributes.find("model_kind");
    if (kind != step->attributes.end() && kind->is_string()) {
      const std::string value = lower(kind->get<std::string>());
      if (value == "vision" || value == "computer_vision") {
        return &step->attributes;
      }
    }
  }
  return nullptr;
}

api::ApiResult run_m13(RunRequest &request) {
  if (request.context == nullptr || request.project == nullptr ||
      request.graph == nullptr) {
    return api::ApiResult::failure(
        "engine_error", "M-13 run request is missing its engine context");
  }
  api::ApiContext &context = *request.context;
  const nlohmann::json *attributes =
      vision_model_attributes(request.graph->graph());
  if (attributes == nullptr) {
    return api::ApiResult::failure("engine_error",
                                   "M-13 graph has no owned vision model step");
  }
  std::string dataset_error;
  std::optional<data::ResolvedDataset> dataset = resolve_graph_dataset(
      context.node, request.graph->graph(), dataset_error);
  if (!dataset) {
    return api::ApiResult::failure("engine_error", dataset_error);
  }

  VisionRunParams params;
  params.project = request.project->id();
  params.graph = request.graph->id();
  params.image_column = attributes->value("image_column", "image");
  params.label_column = attributes->value("label_column", "label");
  params.source_column = attributes->value("source_column", "source");
  params.backbone_model = attributes->value("backbone_model", "");
  params.model_name = request.input.value("model_name", params.model_name);
  params.holdout_fraction = attributes->value("holdout_fraction", 0.25);
  params.epochs = attributes->value("epochs", std::size_t{80});
  params.learning_rate = attributes->value("learning_rate", 0.15);
  params.seed = attributes->value("seed", std::uint64_t{1});
  if (params.backbone_model.empty()) {
    return api::ApiResult::failure(
        "invalid_input",
        "M-13 requires hints.backbone_model when the intent is compiled");
  }

  const VisionReadAuthorizer authorizer = [&](std::string_view image_ref,
                                              std::string &reason) {
    if (context.node.authorize_blob(context.acting_key, context.acting_is_user,
                                    std::string(image_ref))) {
      return true;
    }
    reason = "the acting identity lacks blob:read permission";
    return false;
  };
  data::FeaturePlanOptions feature_options;
  feature_options.label_column = params.label_column;
  feature_options.native_entries.push_back({{params.image_column}, {"image_embedding"},
      "native PNG/JPEG decoding, normalization and frozen ViT embedding",
      "the classifier learns from image content with a source-disjoint holdout"});
  const auto feature_plan = plan_run_features(request, "M-13", *dataset, feature_options);
  if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
    return api::ApiResult::failure("invalid_input", error);
  }
  VisionRunResult result =
      run_vision_on_table(context.node, *dataset, params, authorizer);
  if (!result.ok()) {
    return api::ApiResult::failure("engine_error", result.error);
  }
  record_run_features(context.node, result.run_id, feature_plan);
  return api::ApiResult::success(
      {{"run_id", result.run_id},
       {"engine", "vision"},
       {"status", "succeeded"},
       {"dataset",
        {{"database", dataset->database},
         {"table", dataset->table},
         {"rows", dataset->rows}}},
       {"dataset_notes",
        {"split by source column '" + params.source_column + "'"}}});
}

} // namespace

Methodology make_m13_methodology() {
  Methodology methodology;
  methodology.id = "M-13";
  methodology.name = "Computer vision";
  methodology.family = "Language, documents & perception";
  methodology.model_kinds = {"computer_vision", "vision"};
  methodology.solutions = {};
  methodology.keywords = {"image", "vision", "classify image", "detect defect",
                          "visual inspection"};
  methodology.inputs = {
      {"image", true, "large-object reference to a PNG or JPEG"},
      {"label", true, "non-negative integer class"},
      {"group", true, "camera, site, document, or other source"}};
  methodology.outputs = {"vision model", "prediction table",
                         "confusion matrix"};
  methodology.metrics = {"accuracy", "macro-F1"};
  methodology.runnable = true;
  methodology.build_shape = build_m13_shape;
  methodology.owns = m13_owns;
  methodology.run = run_m13;
  return methodology;
}

} // namespace zetta::ml
