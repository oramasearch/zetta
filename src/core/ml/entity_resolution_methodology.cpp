// SPDX-License-Identifier: MIT

#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/data/dataset.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/entity_resolution.hpp"
#include "zetta/ml/entity_resolution_job.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr const char *kEntityResolutionLaunchKind = "entity_resolution";

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
  const std::string entity_column =
      hint_string(request, "entity_column", "");
  const auto sources = request.hints.find("sources");
  const bool multiple_sources =
      sources != request.hints.end() && sources->is_array() &&
      sources->size() > 1;
  const bool keyed = !request.solution.empty() &&
                     !entity_column.empty() && !multiple_sources;
  graph.add_node(
      ir::StepKind::Connect, "Read records from every source",
      nlohmann::json{{"source", source},
                     {"table", table},
                     {"required_columns",
                      keyed ? nlohmann::json::array({entity_column})
                            : nlohmann::json::array(
                                  {"source", "record_id"})},
                     {"resolution_mode", keyed ? "keyed" : "fuzzy"}},
      "read_records");
  graph.add_node(
      ir::StepKind::Clean,
      keyed ? "Use declared entity key" : "Normalize and block candidate pairs",
      keyed
          ? nlohmann::json{{"resolution_mode", "keyed"},
                           {"entity_column", entity_column},
                           {"duplicate_rows", "collapse_observations"}}
          : nlohmann::json{{"resolution_mode", "fuzzy"},
                           {"normalization", "email_phone_name_address"},
                           {"blocking_routes",
                            {"exact_email", "exact_phone", "sorted_name",
                             "minhash_lsh"}}},
      "block_pairs");
  graph.add_node(
      ir::StepKind::Model, "Score entity matches",
      keyed
          ? nlohmann::json{{"model_kind", "entity_resolution"},
                           {"resolution_mode", "keyed"},
                           {"entity_column", entity_column},
                           {"scorer", "declared_entity_key"}}
          : nlohmann::json{
                {"model_kind", "entity_resolution"},
                {"resolution_mode", "fuzzy"},
                {"scorer", "fellegi_sunter_em"},
                {"label_column", "truth_entity"},
                {"blocking_keys", {"email", "phone", "name", "address"}},
                {"match_threshold", 0.82},
                {"review_threshold", 0.55},
                {"conflict_policy", "reject_verified_identifier_conflicts"}},
      "score_pairs");
  graph.add_node(
      ir::StepKind::Clean,
      keyed ? "Keep one resolved entity per declared key"
            : "Cluster conflict-safe entities",
      keyed
          ? nlohmann::json{{"resolution_mode", "keyed"},
                           {"entity_column", entity_column}}
          : nlohmann::json{
                {"resolution_mode", "fuzzy"},
                {"hard_keys", {"verified_email", "verified_phone"}},
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
  for (const auto &node : graph.nodes_of_kind(ir::StepKind::Connect)) {
    if (!node || !node->attributes.is_object()) {
      continue;
    }
    const std::string source =
        node->attributes.value("source", std::string{});
    const std::string table =
        node->attributes.value("table", std::string{});
    if (!source.empty() && !table.empty()) {
      params.keyed_source = source + "." + table;
      break;
    }
  }
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
    if (node->attributes.value("resolution_mode", std::string{}) == "keyed") {
      params.mode = EntityResolutionMode::Keyed;
      params.keyed_entity_column =
          node->attributes.value("entity_column", std::string{});
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

nlohmann::json params_json(const EntityResolutionParams &params) {
  return {{"seed", params.seed},
          {"sorted_neighbourhood_window",
           params.sorted_neighbourhood_window},
          {"match_threshold", params.match_threshold},
          {"review_threshold", params.review_threshold},
          {"one_record_per_source", params.one_record_per_source},
          {"mode", to_string(params.mode)},
          {"keyed_entity_column", params.keyed_entity_column},
          {"keyed_source", params.keyed_source},
          {"source_column", params.source_column},
          {"record_id_column", params.record_id_column},
          {"name_column", params.name_column},
          {"address_column", params.address_column},
          {"email_column", params.email_column},
          {"phone_column", params.phone_column},
          {"date_column", params.date_column},
          {"email_verified_column", params.email_verified_column},
          {"phone_verified_column", params.phone_verified_column},
          {"truth_entity_column", params.truth_entity_column}};
}

EntityResolutionParams params_from_json(const nlohmann::json &value) {
  EntityResolutionParams params;
  params.seed = value.at("seed");
  params.sorted_neighbourhood_window =
      value.at("sorted_neighbourhood_window");
  params.match_threshold = value.at("match_threshold");
  params.review_threshold = value.at("review_threshold");
  params.one_record_per_source = value.at("one_record_per_source");
  params.mode = value.at("mode") == "keyed" ? EntityResolutionMode::Keyed
                                             : EntityResolutionMode::Fuzzy;
  params.keyed_entity_column = value.at("keyed_entity_column");
  params.keyed_source = value.at("keyed_source");
  params.source_column = value.at("source_column");
  params.record_id_column = value.at("record_id_column");
  params.name_column = value.at("name_column");
  params.address_column = value.at("address_column");
  params.email_column = value.at("email_column");
  params.phone_column = value.at("phone_column");
  params.date_column = value.at("date_column");
  params.email_verified_column = value.at("email_verified_column");
  params.phone_verified_column = value.at("phone_verified_column");
  params.truth_entity_column = value.at("truth_entity_column");
  return params;
}

nlohmann::json launch_payload(const data::ResolvedDataset &dataset,
                              const EntityResolutionParams &params,
                              const std::string &suffix,
                              const std::string &output_node) {
  return {{"database", dataset.database},
          {"table", dataset.table},
          {"table_id", dataset.table_id},
          {"rows", dataset.rows},
          {"notes", dataset.notes},
          {"params", params_json(params)},
          {"output_suffix", suffix},
          {"output_node", output_node}};
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
  data::ResolvedDataset dataset = data::resolve_dataset(
      context.node, path, contract,
      data::default_dataset_sampler(context.node, context.cluster));
  if (!dataset.ok()) {
    return run_dataset_failure(request, "M-2", dataset);
  }

  EntityResolutionParams params = graph_params(request.graph->graph());
  data::FeaturePlanOptions feature_options;
  feature_options.entity_column =
      params.mode == EntityResolutionMode::Keyed
          ? params.keyed_entity_column
          : params.record_id_column;
  for (const auto& summary : context.node.datasets()) {
    if (summary.table_id != dataset.table_id) {
      continue;
    }
    const auto add = [&](const std::string& column, std::vector<std::string> names,
                         std::string transform) {
      if (std::find(summary.columns.begin(), summary.columns.end(), column) != summary.columns.end()) {
        feature_options.native_entries.push_back({{column}, std::move(names), std::move(transform),
            "pairwise identity evidence for candidate matching"});
      }
    };
    if (params.mode == EntityResolutionMode::Fuzzy) {
      add(params.name_column, {"name_jaro_winkler", "name_token_jaccard"},
          "normalized Jaro-Winkler and token Jaccard pair similarities");
      add(params.address_column, {"address_token_jaccard"},
          "address token Jaccard similarity");
      add(params.email_column, {"email_agreement", "email_missing"},
          "normalized exact email agreement and pair missing flag");
      add(params.phone_column, {"phone_agreement", "phone_missing"},
          "normalized exact phone agreement and pair missing flag");
      add(params.date_column, {"date_similarity", "date_missing"},
          "exponential date-distance similarity and pair missing flag");
      add(params.email_verified_column, {"verified_email_constraint"},
          "reject conflicting verified emails during clustering");
      add(params.phone_verified_column, {"verified_phone_constraint"},
          "reject conflicting verified phone numbers during clustering");
      if (std::find(summary.columns.begin(), summary.columns.end(),
                    params.truth_entity_column) != summary.columns.end()) {
        feature_options.label_column = params.truth_entity_column;
      }
    }
  }
  const auto feature_plan = plan_run_features(request, "M-2", dataset, feature_options);
  if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
    return api::ApiResult::failure("invalid_input", error);
  }

  const std::shared_ptr<pipeline::Run> run =
      context.node.create_run(request.project->id(), request.graph->id());
  if (!run) {
    return api::ApiResult::failure("engine_error",
                                   "M-2 could not create its Run record");
  }
  // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
  run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
  run->set_status(pipeline::RunStatus::Running);
  record_run_features(context.node, run->id(), feature_plan);
  run->add_log({util::now_iso8601(), "info",
                "entity resolution read dataset: " + dataset_name});

  std::vector<std::string> roster;
  std::string output_node;
  if (context.node.job_coordinator() != nullptr) {
    roster = context.node.job_coordinator()->collectives().participants();
  }
  if (roster.size() > 1) {
    if (!request.input.value("coordinate", true)) {
      run->set_status(pipeline::RunStatus::Failed);
      return api::ApiResult::failure(
          "invalid_input",
          "entity resolution requires coordinate=true when the cluster has "
          "peers");
    }
    if (context.node.coordinated_launch() == nullptr ||
        context.cluster == nullptr) {
      run->set_status(pipeline::RunStatus::Failed);
      return api::ApiResult::failure(
          "engine_error",
          "entity resolution requires an attached coordinated compute cluster");
    }
    output_node = context.node.coordinated_launch()->node_id();
    compute::CoordinatedLaunchRequest launch;
    launch.id = run->id();
    launch.kind = kEntityResolutionLaunchKind;
    launch.initiator = output_node;
    launch.participants = roster;
    launch.payload = launch_payload(dataset, params, run->id().substr(0, 8),
                                    output_node);
    context.node.coordinated_launch()->fan_out(launch);
  }
  const EntityResolutionTableResult table_result = run_entity_resolution(
      context.node, dataset, params, run->id().substr(0, 8), context.cluster,
      output_node);
  if (!table_result.ok()) {
    run->add_log({util::now_iso8601(), "error", table_result.resolution.error});
    run->set_status(pipeline::RunStatus::Failed);
    return api::ApiResult::failure("engine_error",
                                   table_result.resolution.error);
  }

  const std::uint64_t excluded =
      params.mode == EntityResolutionMode::Keyed
          ? 0
          : dataset.rows >= table_result.resolution.entities.size()
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
                     {"resolution_mode", to_string(params.mode)},
                     {"match_threshold", params.match_threshold},
                     {"review_threshold", params.review_threshold}};
  if (dataset.contract.entity_keys) {
    model_value.feature_schema["entity_key_mapping"] =
        dataset.contract.entity_keys->to_json();
    run->set_entity_key_mapping(dataset.contract.entity_keys->to_json());
  }
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

class EntityResolutionLaunchErrors
    : public nlohmann::json_schema::error_handler {
public:
  std::string reason;

  void error(const nlohmann::json::json_pointer &path,
             const nlohmann::json &, const std::string &message) override {
    if (reason.empty()) {
      reason = path.to_string() + ": " + message;
    }
  }
};

compute::CoordinatedLaunchListener::LaunchOutcome run_peer(
    Node &node, cluster::Cluster &cluster,
    compute::CoordinatedLaunchListener &listener,
    const compute::CoordinatedLaunchRequest &launch) {
  static const nlohmann::json_schema::json_validator validator(
      entity_resolution_launch_schema());
  EntityResolutionLaunchErrors errors;
  validator.validate(launch.payload, errors);
  if (!errors.reason.empty()) {
    return {false, "entity resolution launch: " + errors.reason};
  }
  if (!compute::participant_rank(launch, listener.node_id()) ||
      node.job_coordinator() == nullptr ||
      launch.participants !=
          node.job_coordinator()->collectives().participants() ||
      launch.payload.at("output_node") != launch.initiator) {
    return {false,
            "entity resolution launch roster or writer no longer matches "
            "the cluster"};
  }
  data::ResolvedDataset dataset;
  dataset.database = launch.payload.at("database");
  dataset.table = launch.payload.at("table");
  dataset.table_id = launch.payload.at("table_id");
  dataset.rows = launch.payload.at("rows");
  dataset.notes =
      launch.payload.at("notes").get<std::vector<std::string>>();
  const EntityResolutionTableResult result = run_entity_resolution(
      node, dataset, params_from_json(launch.payload.at("params")),
      launch.payload.at("output_suffix").get<std::string>(), &cluster,
      launch.payload.at("output_node").get<std::string>());
  return {result.ok(),
          result.ok() ? "entity resolution peer completed"
                      : result.resolution.error};
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

const nlohmann::json &entity_resolution_launch_schema() {
  static const nlohmann::json schema = [] {
    nlohmann::json result = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "zetta.EntityResolutionLaunch"},
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"database", "table", "table_id", "rows", "notes", "params",
          "output_suffix", "output_node"}}};
    for (const auto *key : {"database", "table", "table_id",
                            "output_suffix", "output_node"}) {
      result["properties"][key] = {{"type", "string"}, {"minLength", 1}};
    }
    result["properties"]["rows"] = {{"type", "integer"}, {"minimum", 0}};
    result["properties"]["notes"] = {
        {"type", "array"}, {"items", {{"type", "string"}}}};
    nlohmann::json params = {
        {"type", "object"},
        {"additionalProperties", false},
        {"required",
         {"seed", "sorted_neighbourhood_window", "match_threshold",
          "review_threshold", "one_record_per_source", "mode",
          "keyed_entity_column", "keyed_source", "source_column",
          "record_id_column", "name_column", "address_column",
          "email_column", "phone_column", "date_column",
          "email_verified_column", "phone_verified_column",
          "truth_entity_column"}}};
    params["properties"]["seed"] = {{"type", "integer"}, {"minimum", 0}};
    params["properties"]["sorted_neighbourhood_window"] = {
        {"type", "integer"}, {"minimum", 2}};
    params["properties"]["match_threshold"] = {
        {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}};
    params["properties"]["review_threshold"] = {
        {"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}};
    params["properties"]["one_record_per_source"] = {{"type", "boolean"}};
    params["properties"]["mode"] = {
        {"type", "string"}, {"enum", {"fuzzy", "keyed"}}};
    params["properties"]["keyed_entity_column"] = {{"type", "string"}};
    params["properties"]["keyed_source"] = {{"type", "string"}};
    for (const auto *key :
         {"source_column", "record_id_column", "name_column",
          "address_column", "email_column", "phone_column", "date_column",
          "email_verified_column", "phone_verified_column",
          "truth_entity_column"}) {
      params["properties"][key] = {{"type", "string"}};
    }
    result["properties"]["params"] = std::move(params);
    return result;
  }();
  return schema;
}

void install_entity_resolution_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node,
    cluster::Cluster &cluster) {
  listener.set_runner(
      kEntityResolutionLaunchKind,
      [&node, &cluster, &listener](const auto &launch) {
        return run_peer(node, cluster, listener, launch);
      });
}

} // namespace zetta::ml
