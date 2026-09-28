#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/uplift_runner.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace zetta::ml {
namespace {

std::string hint_string(const nlohmann::json &hints, const char *key,
                        std::string fallback = {}) {
  const auto found = hints.find(key);
  return found != hints.end() && found->is_string() ? found->get<std::string>()
                                                    : std::move(fallback);
}

nlohmann::json feature_annotations(const nlohmann::json &hints) {
  nlohmann::json features = nlohmann::json::array();
  const auto found = hints.find("features");
  if (found == hints.end() || !found->is_array()) {
    return features;
  }
  for (const nlohmann::json &feature : *found) {
    if (feature.is_string()) {
      features.push_back({{"name", feature}});
    } else if (feature.is_object()) {
      features.push_back(feature);
    }
  }
  return features;
}

ir::ExecutionGraph build_m6_shape(const ShapeRequest &request) {
  const std::string source = hint_string(request.hints, "source", "datasets");
  const std::string table =
      hint_string(request.hints, "table", "uplift_population");
  const std::string treatment =
      hint_string(request.hints, "treatment", "treatment");
  const std::string outcome = hint_string(request.hints, "outcome", "outcome");
  const std::string entity = hint_string(request.hints, "entity", "entity");

  nlohmann::json model = {{"model_kind", "uplift"},
                          {"treatment", treatment},
                          {"outcome", outcome},
                          {"label", outcome},
                          {"entity", entity}};
  const nlohmann::json features = feature_annotations(request.hints);
  if (!features.empty()) {
    model["features"] = features;
    model["as_of"] =
        hint_string(request.hints, "treatment_time", "1970-01-01T00:00:00Z");
  }
  if (const auto time = request.hints.find("treatment_time");
      time != request.hints.end() && time->is_string()) {
    model["treatment_time"] = *time;
  }
  model["overlap_lower"] = 0.05;
  model["overlap_upper"] = 0.95;

  ir::ExecutionGraph graph;
  graph.add_node(ir::StepKind::Connect, "Load treatment population",
                 {{"source", source}, {"table", table}}, "load_population");
  graph.add_node(
      ir::StepKind::Clean, "Select pre-treatment features",
      {{"note", "exclude outcome-derived and post-treatment columns"}},
      "prepare_causal_features");
  graph.add_node(ir::StepKind::Model, "Estimate treatment uplift",
                 std::move(model), "train_uplift");
  graph.add_node(ir::StepKind::Act, "Publish targeting policy",
                 {{"targeting_fraction", 0.1},
                  {"note", "recommend only profiles inside overlap"}},
                 "publish_targeting_policy");
  graph.add_edge("load_population", "prepare_causal_features");
  graph.add_edge("prepare_causal_features", "train_uplift");
  graph.add_edge("train_uplift", "publish_targeting_policy");
  return graph;
}

bool m6_owns(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes_of_kind(ir::StepKind::Model)) {
    if (!node->attributes.is_object()) {
      continue;
    }
    const std::string kind =
        node->attributes.value("model_kind", std::string{});
    if (kind == "uplift" || kind == "causal") {
      return true;
    }
  }
  return false;
}

const ir::Node *uplift_model(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes_of_kind(ir::StepKind::Model)) {
    if (node && node->attributes.is_object()) {
      const std::string kind =
          node->attributes.value("model_kind", std::string{});
      if (kind == "uplift" || kind == "causal") {
        return node.get();
      }
    }
  }
  return nullptr;
}

std::string graph_dataset_path(const ir::ExecutionGraph &graph) {
  const std::vector<std::shared_ptr<ir::Node>> sources =
      graph.nodes_of_kind(ir::StepKind::Connect);
  if (sources.empty() || !sources.front()->attributes.is_object()) {
    return {};
  }
  const nlohmann::json &attributes = sources.front()->attributes;
  const std::string source =
      attributes.value("source", std::string("datasets"));
  const std::string table = attributes.value("table", std::string{});
  return table.empty() ? std::string{} : source + "." + table;
}

api::ApiResult run_m6(RunRequest &request) {
  if (request.context == nullptr || request.project == nullptr ||
      request.graph == nullptr) {
    return api::ApiResult::failure(
        "engine_error", "M-6 run request is missing its engine context");
  }
  const ir::Node *model = uplift_model(request.graph->graph());
  if (model == nullptr) {
    return api::ApiResult::failure("invalid_input",
                                   "M-6 graph has no uplift model step");
  }
  const std::string dataset_name = request.input.value(
      "dataset_path", graph_dataset_path(request.graph->graph()));
  if (dataset_name.empty()) {
    return api::ApiResult::failure("invalid_input",
                                   "M-6 needs a table on its connect step");
  }
  const data::DatasetPath path = data::parse_dataset_path(dataset_name);
  if (path.kind != data::DatasetPathKind::Table) {
    return api::ApiResult::failure(
        "invalid_input", "M-6 runs on a resolved database.table dataset");
  }

  const std::string treatment = request.input.value(
      "treatment_column", model->attributes.value("treatment", std::string{}));
  const std::string outcome = request.input.value(
      "outcome_column", model->attributes.value("outcome", std::string{}));
  if (treatment.empty() || outcome.empty()) {
    return api::ApiResult::failure(
        "invalid_input", "M-6 requires treatment and outcome columns");
  }

  data::DatasetContract overrides;
  if (const auto given = request.input.find("dataset");
      given != request.input.end()) {
    std::string reason;
    const std::optional<data::DatasetContract> parsed =
        data::dataset_contract_from_json(*given, &reason);
    if (!parsed) {
      return api::ApiResult::failure("invalid_input", reason);
    }
    overrides = *parsed;
  }
  overrides.label_column = outcome;
  if (overrides.entity_column.empty()) {
    overrides.entity_column = model->attributes.value("entity", std::string{});
  }
  data::ResolvedDataset resolved = data::resolve_dataset(
      request.context->node, path, overrides,
      data::default_dataset_sampler(request.context->node,
                                    request.context->cluster));
  if (!resolved.ok()) {
    return api::ApiResult::failure("engine_error", resolved.error);
  }
  resolved.contract.label_column = outcome;
  std::erase(resolved.contract.feature_columns, treatment);
  std::erase(resolved.contract.feature_columns, outcome);
  std::erase(resolved.contract.feature_columns,
             resolved.contract.entity_column);
  if (resolved.contract.feature_columns.empty()) {
    return api::ApiResult::failure("invalid_input",
                                   "M-6 has no pre-treatment feature columns");
  }

  UpliftRunOptions options;
  options.treatment_column = treatment;
  options.outcome_column = outcome;
  options.model_name = request.input.value("model_name", options.model_name);
  options.project = request.project->id();
  options.graph = request.graph->id();
  options.coordinate = request.input.value("coordinate", true);
  options.params.holdout_fraction =
      request.input.value("holdout_fraction", options.params.holdout_fraction);
  options.params.targeting_fraction = request.input.value(
      "targeting_fraction", options.params.targeting_fraction);
  options.params.bootstrap_samples = request.input.value(
      "bootstrap_samples", options.params.bootstrap_samples);
  options.params.seed = request.input.value("seed", options.params.seed);

  UpliftRunResult result =
      run_uplift_on_table(request.context->node, resolved, options);
  if (!result.run) {
    return api::ApiResult::failure("engine_error", result.error);
  }
  nlohmann::json output{
      {"run_id", result.run->id()},
      {"engine", "uplift"},
      {"status", std::string(to_string(result.run->status()))},
      {"dataset", data::to_json(resolved.contract)},
      {"dataset_notes", resolved.notes}};
  if (!result.ok()) {
    output["error"] = result.error;
  }
  return api::ApiResult::success(std::move(output));
}

} // namespace

Methodology make_m6_methodology() {
  Methodology methodology;
  methodology.id = "M-6";
  methodology.name = "Causal inference & uplift";
  methodology.family = "Action & decision";
  methodology.model_kinds = {"causal", "uplift"};
  methodology.solutions = {"S-11"};
  methodology.keywords = {"uplift", "treatment", "who to target",
                          "incremental effect"};
  methodology.inputs = {
      {"entity", true, "unit receiving treatment"},
      {"treatment", true, "binary treatment assignment"},
      {"label", true, "binary observed outcome"},
      {"time", false, "treatment timestamp for leakage checks"},
      {"feature", true, "pre-treatment attributes"}};
  methodology.outputs = {"uplift model", "treatment effects",
                         "targeting policy", "Qini curve", "run report"};
  methodology.metrics = {"held-out AUUC", "Qini coefficient",
                         "uplift at targeting fraction"};
  methodology.issue = "https://github.com/oramasearch/zetta/issues/1045";
  methodology.runnable = true;
  methodology.build_shape = build_m6_shape;
  methodology.owns = m6_owns;
  methodology.run = run_m6;
  return methodology;
}

} // namespace zetta::ml
