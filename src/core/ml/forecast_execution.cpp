#include "zetta/ml/forecast_methodology.hpp"
#include "zetta/ml/run_features.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/api/background.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/cpu_device.hpp"
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

data::ResolvedDataset resolve_history(Node &node, ForecastPipeline &pipeline) {
  data::ResolvedDataset resolved;
  resolved.database = pipeline.database;
  resolved.table = pipeline.table;
  auto &params = pipeline.params;
  resolved.contract.time_column = params.time_column;
  if (params.as_of) {
    resolved.contract.as_of = *params.as_of;
  }
  resolved.contract.label_column = params.target_column;
  resolved.contract.entity_column = params.series_column;
  for (const data::DatasetSummary &dataset : node.datasets()) {
    if (dataset.database != pipeline.database ||
        dataset.name != pipeline.table) {
      continue;
    }
    if (dataset.degraded || dataset.partitions.empty() || dataset.rows == 0) {
      resolved.error =
          "forecast requires a complete, nonempty partitioned table: " +
          dataset.degraded_reason;
      return resolved;
    }
    std::vector<std::string> selected = {params.time_column,
                                         params.target_column};
    if (!params.series_column.empty()) {
      selected.push_back(params.series_column);
    }
    for (const std::string &column : selected) {
      if (std::ranges::find(dataset.columns, column) == dataset.columns.end()) {
        resolved.error = "forecast input column '" + column +
                         "' is absent from " + pipeline.database + "." +
                         pipeline.table;
        return resolved;
      }
    }
    Json excluded = Json::array();
    for (const std::string &column : dataset.columns) {
      if (std::ranges::find(selected, column) == selected.end()) {
        excluded.push_back({{"column", column},
                            {"reason", "not a time, target or series role; "
                                       "external features are not used"}});
      }
    }
    params.dataset_facts = {
        {"source", pipeline.database + "." + pipeline.table},
        {"columns_read", selected},
        {"not_used", excluded},
        {"rows_excluded", 0},
        {"catalog_rows", dataset.rows}};
    resolved.table_id = dataset.table_id;
    resolved.rows = dataset.rows;
    return resolved;
  }
  resolved.error =
      "forecast table not found: " + pipeline.database + "." + pipeline.table;
  return resolved;
}

struct ForecastWork {
  Node *node = nullptr;
  data::ResolvedDataset dataset;
  ForecastRunParams params;
  std::shared_ptr<pipeline::Run> run;
  observability::TraceContext trace;
  std::atomic<bool> done = false;
};

runtime::task<> finish_before_release(std::shared_ptr<ForecastWork> work) {
  while (!work->done.load()) {
    work->node->drain_owner_work();
    co_await runtime::delay(std::chrono::milliseconds(1));
  }
}

// BackgroundActions releases this on the owner thread. The coroutine holds
// only Work, so destroying the action can still drain pending owner writes.
struct ForecastAction {
  std::shared_ptr<ForecastWork> work;

  ~ForecastAction() {
    runtime::block_on(finish_before_release(work));
  }
};

runtime::task<> execute_forecast(std::shared_ptr<ForecastWork> work) {
  observability::ScopedSpan span(&work->node->span_recorder(), work->trace,
                                 "methodology:M-4:run");
  span.set_attribute("dataset", work->params.dataset_facts);
  compute::CpuDevice device;
  const ForecastRunResult result =
      run_forecast_table(*work->node, device, work->dataset, work->params);
  work->node->run_on_owner([&] {
    if (result.ok()) {
      auto outputs = result.outputs;
      outputs.push_back({"report", "forecast report", work->run->id()});
      work->run->set_outputs(std::move(outputs));
      work->run->set_lineage(result.lineage);
      work->run->add_log(
          {util::now_iso8601(), "info", result.result.metrics.comparison});
      work->run->add_log({util::now_iso8601(), "info",
                          "forecast read " + std::to_string(result.rows_read) +
                              " rows; excluded 0 rows"});
      work->run->set_status(pipeline::RunStatus::Succeeded);
    } else {
      work->run->add_log({util::now_iso8601(), "error", result.error});
      work->run->set_status(pipeline::RunStatus::Failed);
    }
  });
  span.set_attribute("rows_read", result.rows_read);
  span.set_attribute("model_id", result.model_id);
  span.finish(result.ok(), result.error);
  co_return;
}

class LaunchError final : public nlohmann::json_schema::error_handler {
public:
  std::string reason;

  void error(const Json::json_pointer &pointer, const Json &,
             const std::string &message) override {
    if (reason.empty()) {
      reason = pointer.to_string() + ": " + message;
    }
  }
};

compute::CoordinatedLaunchListener::LaunchOutcome
run_peer(Node &node, const compute::CoordinatedLaunchRequest &request) {
  LaunchError validation;
  nlohmann::json_schema::json_validator validator(forecast_launch_schema());
  validator.validate(request.payload, validation);
  if (!validation.reason.empty()) {
    return {false, "forecast launch: " + validation.reason};
  }
  if (!node.job_coordinator() || !node.coordinated_launch() ||
      !compute::participant_rank(request,
                                 node.coordinated_launch()->node_id()) ||
      request.participants !=
          node.job_coordinator()->collectives().participants()) {
    return {false, "forecast launch: collective membership changed"};
  }
  std::string error;
  const auto graph = ir::from_json(request.payload["graph"], &error);
  if (!graph) {
    return {false, error};
  }
  auto pipeline = parse_forecast_pipeline(*graph, error);
  if (!pipeline) {
    return {false, error};
  }
  pipeline->params.project_id =
      request.payload["project_id"].get<std::string>();
  pipeline->params.run_id = request.id;
  pipeline->params.output_table = "forecast_" + request.id;
  pipeline->params.model_name =
      request.payload["model_name"].get<std::string>();
  const data::ResolvedDataset dataset = resolve_history(node, *pipeline);
  if (!dataset.ok()) {
    return {false, dataset.error};
  }
  compute::CpuDevice device;
  const ForecastRunResult result =
      run_forecast_table(node, device, dataset, pipeline->params);
  return {result.ok(),
          result.ok() ? result.result.metrics.comparison : result.error};
}

} // namespace

const nlohmann::json &forecast_launch_schema() {
  static const Json schema = {
      {"$schema", "http://json-schema.org/draft-07/schema#"},
      {"type", "object"},
      {"additionalProperties", false},
      {"required", {"graph", "project_id", "model_name"}},
      {"properties",
       {{"graph", {{"type", "object"}}},
        {"project_id", {{"type", "string"}, {"minLength", 1}}},
        {"model_name", {{"type", "string"}, {"minLength", 1}}}}}};
  return schema;
}

void install_forecast_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node) {
  listener.set_runner(
      "forecast", [&node](const compute::CoordinatedLaunchRequest &request) {
        return run_peer(node, request);
      });
}

api::ApiResult run_forecast_methodology(RunRequest &request) {
  if (!request.context || !request.project || !request.graph ||
      !request.context->background) {
    return api::ApiResult::failure(
        "engine_error",
        "M-4 requires an engine context and background action owner");
  }
  Node &node = request.context->node;
  if (!node.job_coordinator() || !node.coordinated_launch()) {
    return api::ApiResult::failure("engine_error",
                                   "M-4 requires the native compute plane");
  }
  std::string error;
  auto pipeline = parse_forecast_pipeline(request.graph->graph(), error);
  if (!pipeline) {
    return api::ApiResult::failure("gate_blocked", error);
  }
  // M-4's roles and source belong to the stored pipeline. Do not accept the
  // classifier's row-holdout or dataset contract as an alternate training path.
  if (request.input.contains("dataset_path") ||
      request.input.contains("dataset") ||
      request.input.value("holdout_fraction", 0.0) != 0.0 ||
      !request.input.value("coordinate", true)) {
    return api::ApiResult::failure(
        "invalid_input",
        "M-4 reads its pipeline source/table and roles; use compile hints, "
        "temporal folds and coordinated execution");
  }
  auto dataset = resolve_history(node, *pipeline);
  if (!dataset.ok()) {
    return api::ApiResult::failure("invalid_input", dataset.error);
  }
  data::FeaturePlanOptions feature_options;
  feature_options.label_column = pipeline->params.target_column;
  feature_options.time_column = pipeline->params.time_column;
  feature_options.entity_column = pipeline->params.series_column;
  for (const auto lag : pipeline->params.forecast.lags) {
    feature_options.native_entries.push_back({
        {pipeline->params.target_column, pipeline->params.time_column},
        {"lag_" + std::to_string(lag)}, "strictly past lag " + std::to_string(lag),
        "forecast history is ordered within series and evaluated with rolling origins"});
  }
  if (pipeline->params.forecast.rolling_window > 0) {
    const auto window = std::to_string(pipeline->params.forecast.rolling_window);
    feature_options.native_entries.push_back({
        {pipeline->params.target_column, pipeline->params.time_column},
        {"rolling_mean_" + window}, "mean of the prior " + window + " observations",
        "the current and future target are excluded from each rolling window"});
  }
  const auto feature_plan = plan_run_features(request, "M-4", dataset, feature_options);
  if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
    return api::ApiResult::failure("invalid_input", reason);
  }
  auto run = node.create_run(request.project->id(), request.graph->id());
  if (!run) {
    return api::ApiResult::failure("engine_error",
                                   "could not create forecast Run");
  }
  auto work = std::make_shared<ForecastWork>();
  record_run_features(node, run->id(), feature_plan);
  work->node = &node;
  work->dataset = std::move(dataset);
  work->params = pipeline->params;
  work->params.project_id = request.project->id();
  work->params.run_id = run->id();
  work->params.output_table = "forecast_" + run->id();
  work->params.model_name =
      request.input.value("model_name", std::string("forecast"));
  work->run = run;
  work->trace = request.context->trace;
  run->set_trace_id(work->trace.trace_id);
  run->set_status(pipeline::RunStatus::Running);
  run->add_log(
      {util::now_iso8601(), "info",
       "forecast dataset: " + pipeline->database + "." + pipeline->table});
  compute::CoordinatedLaunchRequest launch;
  launch.id = run->id();
  launch.kind = "forecast";
  launch.initiator = node.coordinated_launch()->node_id();
  launch.participants = node.job_coordinator()->collectives().participants();
  launch.payload = {{"graph", ir::to_json(request.graph->graph())},
                    {"project_id", work->params.project_id},
                    {"model_name", work->params.model_name}};
  node.coordinated_launch()->fan_out(launch);
  auto action = std::make_shared<ForecastAction>();
  action->work = work;
  request.context->background->adopt(action, [work] {
    return work->done.load();
  });
  runtime::spawn(execute_forecast(work), [work] {
    work->done.store(true);
  });
  return api::ApiResult::success({{"run_id", run->id()},
                                  {"engine", "forecast"},
                                  {"methodology", "M-4"},
                                  {"status", "running"},
                                  {"dataset", work->params.dataset_facts}});
}

} // namespace zetta::ml
