#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/entity_resolution.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

bool m2_owns(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes()) {
    if (node->kind != ir::StepKind::Model || !node->attributes.is_object()) {
      continue;
    }
    const auto kind = node->attributes.find("model_kind");
    if (kind != node->attributes.end() && kind->is_string()) {
      const std::string value = kind->get<std::string>();
      return value == "entity_resolution" || value == "entity-resolution";
    }
  }
  return false;
}

std::string hint_string(const ShapeRequest &request, const char *name,
                        std::string fallback) {
  const auto value = request.hints.find(name);
  return value != request.hints.end() && value->is_string()
             ? value->get<std::string>()
             : std::move(fallback);
}

ir::ExecutionGraph build_m2_shape(const ShapeRequest &request) {
  ir::ExecutionGraph graph;
  const std::string source = hint_string(request, "source", "datasets");
  const std::string table = hint_string(request, "table", "records");
  graph.add_node(
      ir::StepKind::Connect, "Read records from every source",
      nlohmann::json{{"source", source},
                     {"table", table},
                     {"required_columns", {"source", "record_id", "name"}}},
      "read_records");
  graph.add_node(ir::StepKind::Clean, "Normalize and block candidate pairs",
                 nlohmann::json{{"normalization", "email_phone_name_address"},
                                {"blocking_routes",
                                 {"exact_email", "exact_phone", "sorted_name",
                                  "minhash_lsh"}}},
                 "block_pairs");
  graph.add_node(
      ir::StepKind::Model, "Score entity matches",
      nlohmann::json{
          {"model_kind", "entity_resolution"},
          {"scorer", "fellegi_sunter_em"},
          {"label_column", "truth_entity"},
          {"blocking_keys", {"email", "phone", "name", "address"}},
          {"match_threshold", 0.82},
          {"review_threshold", 0.55},
          {"conflict_policy", "reject_verified_identifier_conflicts"}},
      "score_pairs");
  graph.add_node(
      ir::StepKind::Clean, "Cluster conflict-safe entities",
      nlohmann::json{{"hard_keys", {"verified_email", "verified_phone"}},
                     {"transitive_conflicts", "reject"}},
      "cluster_entities");
  graph.add_node(
      ir::StepKind::Serve, "Write resolution tables",
      nlohmann::json{{"outputs", {"resolved_entities", "match_pairs"}}},
      "write_resolution_tables");
  graph.add_edge("read_records", "block_pairs");
  graph.add_edge("block_pairs", "score_pairs");
  graph.add_edge("score_pairs", "cluster_entities");
  graph.add_edge("cluster_entities", "write_resolution_tables");
  return graph;
}

std::optional<std::string> graph_dataset_path(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes()) {
    if (node->kind != ir::StepKind::Connect || !node->attributes.is_object()) {
      continue;
    }
    const auto source = node->attributes.find("source");
    const auto table = node->attributes.find("table");
    if (source != node->attributes.end() && source->is_string() &&
        table != node->attributes.end() && table->is_string()) {
      return source->get<std::string>() + "." + table->get<std::string>();
    }
  }
  return std::nullopt;
}

EntityResolutionParams graph_params(const ir::ExecutionGraph &graph) {
  EntityResolutionParams params;
  for (const auto &node : graph.nodes()) {
    if (node->kind != ir::StepKind::Model || !node->attributes.is_object()) {
      continue;
    }
    if (const auto value = node->attributes.find("match_threshold");
        value != node->attributes.end() && value->is_number()) {
      params.match_threshold = value->get<double>();
    }
    if (const auto value = node->attributes.find("review_threshold");
        value != node->attributes.end() && value->is_number()) {
      params.review_threshold = value->get<double>();
    }
    if (const auto value = node->attributes.find("one_record_per_source");
        value != node->attributes.end() && value->is_boolean()) {
      params.one_record_per_source = value->get<bool>();
    }
  }
  return params;
}

nlohmann::json metrics_json(const EntityResolutionResult &result,
                            const EntityResolutionParams &params,
                            std::uint64_t input_rows,
                            std::uint64_t excluded_rows) {
  nlohmann::json metrics = {
      {"merge_rate", result.metrics.merge_rate},
      {"review_band_size",
       static_cast<double>(result.metrics.review_band_size)},
      {"candidate_pairs", static_cast<double>(result.metrics.candidate_pairs)},
      {"labels_present", result.metrics.labels_present ? 1.0 : 0.0},
      {"match_threshold", params.match_threshold},
      {"review_threshold", params.review_threshold},
      {"input_rows", static_cast<double>(input_rows)},
      {"excluded_rows", static_cast<double>(excluded_rows)}};
  if (result.metrics.labels_present) {
    metrics["blocking_recall"] = result.metrics.blocking_recall;
    metrics["pairwise_precision"] = result.metrics.pairwise_precision;
    metrics["pairwise_recall"] = result.metrics.pairwise_recall;
    metrics["pairwise_f1"] = result.metrics.pairwise_f1;
    metrics["bcubed_precision"] = result.metrics.bcubed_precision;
    metrics["bcubed_recall"] = result.metrics.bcubed_recall;
  }
  return metrics;
}

api::ApiResult run_m2(RunRequest &request) {
  if (request.context == nullptr || request.project == nullptr ||
      request.graph == nullptr) {
    return api::ApiResult::failure(
        "engine_error", "M-2 run request is missing its engine context");
  }
  api::ApiContext &context = *request.context;
  std::string dataset_name = request.input.value("dataset_path", "");
  if (dataset_name.empty()) {
    const std::optional<std::string> from_graph =
        graph_dataset_path(request.graph->graph());
    if (!from_graph) {
      return api::ApiResult::failure(
          "invalid_input",
          "M-2 needs dataset_path or a connect step with source and table");
    }
    dataset_name = *from_graph;
  }
  const data::DatasetPath path = data::parse_dataset_path(dataset_name);
  if (path.kind != data::DatasetPathKind::Table) {
    return api::ApiResult::failure(
        "invalid_input", "M-2 dataset_path must name a database.table");
  }
  data::DatasetContract contract;
  if (const auto given = request.input.find("dataset");
      given != request.input.end()) {
    std::string reason;
    const std::optional<data::DatasetContract> parsed =
        data::dataset_contract_from_json(*given, &reason);
    if (!parsed) {
      return api::ApiResult::failure("invalid_input", reason);
    }
    contract = *parsed;
  }
  const data::ResolvedDataset dataset = data::resolve_dataset(
      context.node, path, contract,
      data::default_dataset_sampler(context.node, context.cluster));
  if (!dataset.ok()) {
    return api::ApiResult::failure("engine_error", dataset.error);
  }

  const std::shared_ptr<pipeline::Run> run =
      context.node.create_run(request.project->id(), request.graph->id());
  if (!run) {
    return api::ApiResult::failure("engine_error",
                                   "M-2 could not create its Run record");
  }
  run->set_status(pipeline::RunStatus::Running);
  run->add_log({util::now_iso8601(), "info",
                "entity resolution read dataset: " + dataset_name});

  EntityResolutionParams params = graph_params(request.graph->graph());
  const EntityResolutionTableResult table_result = run_entity_resolution(
      context.node, dataset, params, run->id().substr(0, 8), context.cluster);
  if (!table_result.ok()) {
    run->add_log({util::now_iso8601(), "error", table_result.resolution.error});
    run->set_status(pipeline::RunStatus::Failed);
    return api::ApiResult::failure("engine_error",
                                   table_result.resolution.error);
  }

  const std::uint64_t excluded =
      dataset.rows >= table_result.resolution.entities.size()
          ? dataset.rows - table_result.resolution.entities.size()
          : 0;
  ModelValue model_value;
  model_value.name =
      request.input.value("model_name", "entity_resolution_model");
  model_value.kind = "entity_resolution";
  model_value.project = request.project->id();
  model_value.run = run->id();
  model_value.feature_schema =
      nlohmann::json{{"scoring_method", table_result.resolution.scoring_method},
                     {"match_threshold", params.match_threshold},
                     {"review_threshold", params.review_threshold}};
  model_value.metrics =
      metrics_json(table_result.resolution, params, dataset.rows, excluded);
  const std::shared_ptr<Model> model =
      context.node.create_model(std::move(model_value));
  if (!model || !model->set_weights(nlohmann::json{
                    {"kind", "entity_resolution"},
                    {"scoring_method", table_result.resolution.scoring_method},
                    {"seed",
                     params.seed}}.dump())) {
    run->add_log({util::now_iso8601(), "error",
                  "entity resolution could not register its model"});
    run->set_status(pipeline::RunStatus::Failed);
    return api::ApiResult::failure("engine_error",
                                   "M-2 could not register its model");
  }

  run->set_outputs({{"dataset", "resolved entities", table_result.entity_table},
                    {"dataset", "match pairs", table_result.pair_table},
                    {"model", model->name(), model->id()},
                    {"report", "run report", run->id()}});
  pipeline::RunLineage lineage;
  lineage.sources = {"dataset:" + dataset_name};
  for (const auto &node : request.graph->graph().nodes()) {
    lineage.steps.push_back(node->id);
  }
  lineage.freshness = util::now_iso8601();
  lineage.confidence =
      1.0 - std::min(1.0, table_result.resolution.metrics.merge_rate);
  run->set_lineage(std::move(lineage));
  for (const std::string &note : dataset.notes) {
    run->add_log({util::now_iso8601(), "info", "dataset: " + note});
  }
  for (const std::string &note : table_result.notes) {
    run->add_log({util::now_iso8601(), "warn", "dataset: not used: " + note});
  }
  run->add_log(
      {util::now_iso8601(), "info",
       "thresholds: match >= " + std::to_string(params.match_threshold) +
           ", review >= " + std::to_string(params.review_threshold) +
           ", otherwise non-match"});
  run->set_status(pipeline::RunStatus::Succeeded);

  return api::ApiResult::success(
      {{"run_id", run->id()},
       {"engine", "entity_resolution"},
       {"status", "succeeded"},
       {"poll", "get_run and get_run_report for outputs and metrics"},
       {"dataset", data::to_json(dataset.contract)},
       {"dataset_notes", dataset.notes}});
}

} // namespace

Methodology make_m2_methodology() {
  Methodology methodology;
  methodology.id = "M-2";
  methodology.name = "Entity resolution";
  methodology.family = "Identity & structure";
  methodology.model_kinds = {"entity_resolution", "entity-resolution"};
  methodology.solutions = {};
  methodology.keywords = {"resolve identity", "resolve customer records",
                          "deduplicate", "match records"};
  methodology.inputs = {
      {"entity", true, "source record identifier"},
      {"group", true, "source system"},
      {"label", false, "truth entity for pair and cluster metrics"},
      {"time", false, "date-distance pair feature"},
  };
  methodology.outputs = {"resolved entities", "match pairs", "review band"};
  methodology.metrics = {
      "pair precision",  "pair recall", "B-cubed precision", "B-cubed recall",
      "blocking recall", "merge rate",  "review-band size"};
  methodology.runnable = true;
  methodology.build_shape = build_m2_shape;
  methodology.owns = m2_owns;
  methodology.run = run_m2;
  return methodology;
}

} // namespace zetta::ml
