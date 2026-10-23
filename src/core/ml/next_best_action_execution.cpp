#include "zetta/ml/next_best_action_methodology.hpp"
#include "zetta/ml/run_features.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/api/background.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/runtime.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

using Json = nlohmann::json;

class LaunchSchemaError final : public nlohmann::json_schema::error_handler {
public:
  std::string reason;

  void error(const Json::json_pointer &pointer, const Json &,
             const std::string &message) override {
    if (reason.empty()) {
      reason = pointer.to_string() + ": " + message;
    }
  }
};

std::vector<std::string>
selected_columns(const NextBestActionPipeline &pipeline) {
  std::vector<std::string> result = {pipeline.contract.entity_column,
                                     pipeline.params.action_column,
                                     pipeline.contract.label_column,
                                     pipeline.params.value_column,
                                     pipeline.params.cost_column,
                                     pipeline.params.eligibility_column,
                                     pipeline.params.contacts_column,
                                     pipeline.params.contact_cap_column,
                                     pipeline.params.interaction_count_column,
                                     pipeline.params.interaction_time_column};
  if (!pipeline.params.logging_propensity_column.empty()) {
    result.push_back(pipeline.params.logging_propensity_column);
  }
  result.insert(result.end(), pipeline.contract.feature_columns.begin(),
                pipeline.contract.feature_columns.end());
  return result;
}

data::ResolvedDataset resolve_input(
    Node &node, NextBestActionPipeline &pipeline,
    const data::DatasetSampler &sampler = {},
    std::shared_ptr<const data::EntityKeyMapping> entity_keys = {}) {
  data::ResolvedDataset resolved;
  resolved.database = pipeline.database;
  resolved.table = pipeline.table;
  resolved.contract = pipeline.contract;
  for (const data::DatasetSummary &dataset : node.datasets()) {
    if (dataset.database != pipeline.database ||
        dataset.name != pipeline.table) {
      continue;
    }
    if (dataset.degraded || dataset.partitions.empty() || dataset.rows == 0) {
      resolved.error =
          "next-best-action requires a complete, nonempty partitioned table";
      if (!dataset.degraded_reason.empty()) {
        resolved.error += ": " + dataset.degraded_reason;
      }
      return resolved;
    }
    const std::vector<std::string> selected = selected_columns(pipeline);
    for (const std::string &column : selected) {
      if (std::ranges::find(dataset.columns, column) == dataset.columns.end()) {
        resolved.error = "next-best-action input column '" + column +
                         "' is absent from " + pipeline.database + "." +
                         pipeline.table;
        return resolved;
      }
    }
    Json excluded = Json::array();
    for (const std::string &column : dataset.columns) {
      if (std::ranges::find(selected, column) == selected.end()) {
        excluded.push_back(
            {{"column", column},
             {"reason", "not selected as an entity, action, response, policy, "
                        "interaction or feature column"}});
      }
    }
    pipeline.params.dataset_facts = {
        {"source", pipeline.database + "." + pipeline.table},
        {"columns_read", selected},
        {"not_used", std::move(excluded)},
        {"rows_excluded", 0},
        {"catalog_rows", dataset.rows}};
    resolved.table_id = dataset.table_id;
    resolved.rows = dataset.rows;
    if (entity_keys) {
      resolved.contract.entity_keys = std::move(entity_keys);
      return resolved;
    }
    if (sampler) {
      std::string sample_error;
      const std::vector<Json> rows = sampler(
          resolved.database, resolved.table, 0, sample_error);
      if (!sample_error.empty()) {
        resolved.error =
            "next-best-action could not map entity column: " + sample_error;
        return resolved;
      }
      std::vector<Json> values;
      values.reserve(rows.size());
      bool text_entities = false;
      for (const Json &row : rows) {
        const auto entity = row.find(resolved.contract.entity_column);
        if (entity == row.end()) {
          resolved.error =
              "next-best-action row is missing entity column '" +
              resolved.contract.entity_column + "'";
          return resolved;
        }
        text_entities = text_entities || entity->is_string();
        values.push_back(*entity);
      }
      if (text_entities) {
        data::EntityKeyMappingResult mapped =
            data::derive_entity_key_mapping(
                resolved.contract.entity_column, values);
        if (!mapped.ok()) {
          resolved.error = std::move(mapped.error);
          return resolved;
        }
        resolved.contract.entity_keys = std::move(mapped.mapping);
        resolved.notes.push_back(
            "entity column '" + resolved.contract.entity_column +
            "' (text; keyed by a derived id, outputs carry the original value)");
      }
    }
    return resolved;
  }
  resolved.error = "next-best-action table not found: " + pipeline.database +
                   "." + pipeline.table;
  return resolved;
}

struct RecommendationWork {
  Node *node = nullptr;
  data::ResolvedDataset dataset;
  NextBestActionTableParams params;
  std::shared_ptr<pipeline::Run> run;
  observability::TraceContext trace;
  std::atomic<bool> done = false;
};

runtime::task<>
finish_before_release(std::shared_ptr<RecommendationWork> work) {
  while (!work->done.load()) {
    work->node->drain_owner_work();
    (void)(co_await runtime::delay(std::chrono::milliseconds(1)));
  }
}

struct RecommendationAction {
  std::shared_ptr<RecommendationWork> work;

  ~RecommendationAction() { runtime::block_on(finish_before_release(work)); }
};

runtime::task<>
execute_recommendation(std::shared_ptr<RecommendationWork> work) {
  observability::ScopedSpan span(&work->node->span_recorder(), work->trace,
                                 "methodology:M-8:run");
  span.set_attribute("dataset", work->params.dataset_facts);
  const NextBestActionTableResult result = run_next_best_action_on_table(
      *work->node, work->dataset, work->params, *work->run);
  work->node->run_on_owner([&] {
    if (result.ok()) {
      work->run->add_log({util::now_iso8601(), "info",
                          "next-best-action read " +
                              std::to_string(result.rows) +
                              " candidate rows; wrote " +
                              std::to_string(result.recommendations.size()) +
                              " ranked entity-action rows"});
      if (!result.offline.refusal.empty()) {
        work->run->add_log(
            {util::now_iso8601(), "warn", result.offline.refusal});
      }
      work->run->set_status(pipeline::RunStatus::Succeeded);
    } else {
      work->run->add_log({util::now_iso8601(), "error", result.error});
      work->run->set_status(pipeline::RunStatus::Failed);
    }
  });
  span.set_attribute("rows_read", result.rows);
  span.set_attribute("model_id", result.model_id);
  span.finish(result.ok(), result.error);
  co_return;
}

bool valid_launch_payload(const Json &payload, std::string &error) {
  nlohmann::json_schema::json_validator validator(
      next_best_action_launch_schema());
  LaunchSchemaError validation;
  validator.validate(payload, validation);
  error = validation.reason;
  return error.empty();
}

compute::CoordinatedLaunchListener::LaunchOutcome
run_peer(Node &node, const compute::CoordinatedLaunchRequest &request) {
  std::string error;
  if (!valid_launch_payload(request.payload, error)) {
    return {false, error};
  }
  if (!node.job_coordinator() || !node.coordinated_launch() ||
      !compute::participant_rank(request,
                                 node.coordinated_launch()->node_id()) ||
      request.participants !=
          node.job_coordinator()->collectives().participants()) {
    return {false, "next-best-action launch: collective membership changed"};
  }
  const auto graph = ir::from_json(request.payload["graph"], &error);
  if (!graph) {
    return {false, error};
  }
  auto pipeline = parse_next_best_action_pipeline(*graph, error);
  if (!pipeline) {
    return {false, error};
  }
  pipeline->params.model_name =
      request.payload["model_name"].get<std::string>();
  pipeline->params.round_tag = "ml.next_best_action." + request.id;
  std::shared_ptr<const data::EntityKeyMapping> entity_keys;
  if (!request.payload["entity_key_mapping"].is_null()) {
    entity_keys = data::entity_key_mapping_from_json(
        request.payload["entity_key_mapping"], &error);
    if (!entity_keys) {
      return {false, "next-best-action launch: " + error};
    }
  }
  const data::ResolvedDataset dataset =
      resolve_input(node, *pipeline, {}, std::move(entity_keys));
  if (!dataset.ok()) {
    return {false, dataset.error};
  }
  const NextBestActionTableResult result =
      run_next_best_action_table_participant(
          node, dataset, pipeline->params,
          request.payload["project_id"].get<std::string>(),
          request.payload["graph_id"].get<std::string>(), request.id);
  return {result.ok(), result.ok() ? "next-best-action participant completed"
                                   : result.error};
}

} // namespace

const nlohmann::json &next_best_action_launch_schema() {
  static const Json schema = {
      {"$schema", "http://json-schema.org/draft-07/schema#"},
      {"$id", "https://oramasearch.github.io/zetta/schemas/"
              "next_best_action_launch.schema.json"},
      {"title", "zetta.NextBestActionLaunch"},
      {"type", "object"},
      {"additionalProperties", false},
      {"required",
       {"graph", "project_id", "graph_id", "model_name",
        "entity_key_mapping"}},
      {"properties",
       {{"graph", {{"type", "object"}}},
        {"project_id", {{"type", "string"}, {"minLength", 1}}},
        {"graph_id", {{"type", "string"}, {"minLength", 1}}},
        {"model_name", {{"type", "string"}, {"minLength", 1}}},
        {"entity_key_mapping", {{"type", {"object", "null"}}}}}}};
  return schema;
}

void install_next_best_action_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node) {
  listener.set_runner(
      "next_best_action",
      [&node](const compute::CoordinatedLaunchRequest &request) {
        return run_peer(node, request);
      });
}

api::ApiResult run_next_best_action_methodology(RunRequest &request) {
  if (!request.context || !request.project || !request.graph ||
      !request.context->background) {
    return api::ApiResult::failure(
        "engine_error",
        "M-8 requires an engine context and background action owner");
  }
  Node &node = request.context->node;
  if (!node.job_coordinator() || !node.coordinated_launch()) {
    return api::ApiResult::failure("engine_error",
                                   "M-8 requires the native compute plane");
  }
  std::string error;
  auto pipeline =
      parse_next_best_action_pipeline(request.graph->graph(), error);
  if (!pipeline) {
    return api::ApiResult::failure("gate_blocked", error);
  }
  if (request.input.contains("dataset_path") ||
      request.input.contains("dataset") ||
      !request.input.value("coordinate", true)) {
    return api::ApiResult::failure(
        "invalid_input",
        "M-8 reads the source, roles and policy in its stored pipeline; use "
        "compile hints and coordinated execution");
  }
  data::ResolvedDataset dataset = resolve_input(
      node, *pipeline,
      data::default_dataset_sampler(node, request.context->cluster));
  if (!dataset.ok()) {
    return api::ApiResult::failure("invalid_input", dataset.error);
  }
  data::FeaturePlanOptions feature_options;
  for (const auto& column : dataset.contract.feature_columns) {
    feature_options.native_entries.push_back({{column}, {column},
        "native numeric input to the response-propensity GBDT",
        "the pipeline selects this pre-decision predictor"});
  }
  feature_options.native_entries.push_back({
      {dataset.contract.entity_column, pipeline->params.action_column,
       pipeline->params.interaction_count_column, pipeline->params.interaction_time_column},
      {"als_affinity"}, "implicit-feedback ALS on training interactions before the temporal holdout",
      "historical entity-action affinity supplements response propensity"});
  feature_options.native_entries.push_back({
      {pipeline->params.value_column, pipeline->params.cost_column,
       pipeline->params.eligibility_column, pipeline->params.contacts_column,
       pipeline->params.contact_cap_column},
      {"expected_value", "eligible_action"},
      "response probability times value minus cost; eligibility and contact-cap filters",
      "the native policy ranks only allowed actions by expected net value"});
  const auto feature_plan = plan_run_features(request, "M-8", dataset, feature_options);
  if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
    return api::ApiResult::failure("invalid_input", reason);
  }
  std::shared_ptr<pipeline::Run> run =
      node.create_run(request.project->id(), request.graph->id());
  if (!run) {
    return api::ApiResult::failure("engine_error",
                                   "could not create next-best-action Run");
  }
  // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
  run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
  auto work = std::make_shared<RecommendationWork>();
  record_run_features(node, run->id(), feature_plan);
  work->node = &node;
  work->dataset = std::move(dataset);
  work->params = pipeline->params;
  work->params.model_name =
      request.input.value("model_name", std::string("next_best_action"));
  work->params.round_tag = "ml.next_best_action." + run->id();
  work->run = run;
  work->trace = request.context->trace;
  run->set_trace_id(work->trace.trace_id);
  run->set_status(pipeline::RunStatus::Running);
  run->add_log({util::now_iso8601(), "info",
                "next-best-action dataset: " + pipeline->database + "." +
                    pipeline->table});

  compute::CoordinatedLaunchRequest launch;
  launch.id = run->id();
  launch.kind = "next_best_action";
  launch.initiator = node.coordinated_launch()->node_id();
  launch.participants = node.job_coordinator()->collectives().participants();
  launch.payload = {{"graph", ir::to_json(request.graph->graph())},
                    {"project_id", request.project->id()},
                    {"graph_id", request.graph->id()},
                    {"model_name", work->params.model_name},
                    {"entity_key_mapping",
                     work->dataset.contract.entity_keys
                         ? work->dataset.contract.entity_keys->to_json()
                         : Json(nullptr)}};
  node.coordinated_launch()->fan_out(launch);

  auto action = std::make_shared<RecommendationAction>();
  action->work = work;
  request.context->background->adopt(action,
                                     [work] { return work->done.load(); });
  runtime::spawn(execute_recommendation(work),
                 [work] { work->done.store(true); });
  return api::ApiResult::success({{"run_id", run->id()},
                                  {"engine", "next_best_action"},
                                  {"methodology", "M-8"},
                                  {"status", "running"},
                                  {"dataset", work->params.dataset_facts}});
}

} // namespace zetta::ml
