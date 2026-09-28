#include "zetta/ml/anomaly_methodology.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <set>
#include <thread>

#include "zetta/api/background.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/runtime.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kLaunchKind = "anomaly_table";

ir::ExecutionGraph build_anomaly_shape(const ShapeRequest &request) {
  const auto &hints = request.hints;
  const auto hint = [&](const char *key, nlohmann::json fallback) {
    const auto found = hints.find(key);
    return found == hints.end() ? fallback : *found;
  };
  nlohmann::json model = {
      {"model_kind", "anomaly"},
      {"mode", hint("mode", "tabular")},
      {"feature_columns", hint("feature_columns", nlohmann::json::array())},
      {"entity_column", hint("entity_column", "")},
      {"label_column", hint("label_column", "")},
      {"time_column", hint("time_column", "")},
      {"group_column", hint("group_column", "")},
      {"label_use", hint("label_use", "evaluation_only")},
      {"scaling", hint("scaling", "median_mad")},
      {"causal", hint("causal", true)},
      {"seed", hint("seed", 1046)},
      {"tree_count", hint("tree_count", 100)},
      {"subsample_size", hint("subsample_size", 256)},
      {"seasonal_period", hint("seasonal_period", 7)},
      {"trend_window", hint("trend_window", 28)},
      {"residual_window", hint("residual_window", 56)},
      {"seasonal_cycles", hint("seasonal_cycles", 8)}};
  if (hints.contains("alert_budget")) {
    model["alert_budget"] = hints.at("alert_budget");
  }
  if (hints.contains("contamination") || !hints.contains("alert_budget")) {
    model["contamination"] = hint("contamination", 0.01);
  }
  const bool kpi = model.at("mode") == "kpi_time_series";
  ir::ExecutionGraph graph;
  graph.add_node(ir::StepKind::Connect, "Read observations",
                 {{"source", hint("source", hint("database", "datasets"))},
                  {"table", hint("table", "observations")}},
                 "anomaly_source");
  graph.add_node(
      ir::StepKind::Clean,
      kpi ? "Remove causal trend and seasonality"
          : "Measure robust feature deviations",
      {{"operation", kpi ? "causal_kpi_residuals" : "robust_features"}},
      "anomaly_features");
  graph.add_node(ir::StepKind::Model, "Score anomalies", std::move(model),
                 "anomaly_model");
  graph.add_node(ir::StepKind::Act, "Write anomaly and budget tables",
                 {{"operation", "anomaly_tables"}}, "anomaly_outputs");
  graph.add_edge("anomaly_source", "anomaly_features");
  graph.add_edge("anomaly_features", "anomaly_model");
  graph.add_edge("anomaly_model", "anomaly_outputs");
  return graph;
}

bool owns_anomaly(const ir::ExecutionGraph &graph) {
  const auto models = graph.nodes_of_kind(ir::StepKind::Model);
  if (models.size() != 1 || !models[0]->attributes.is_object()) {
    return false;
  }
  const auto kind = models[0]->attributes.find("model_kind");
  return kind != models[0]->attributes.end() && kind->is_string() &&
         is_anomaly_kind(kind->get<std::string>());
}

data::ResolvedDataset
resolve_anomaly_dataset(Node &node, const AnomalyPlan &plan,
                        const data::DatasetSampler &sampler = {}) {
  data::ResolvedDataset result;
  result.database = plan.database;
  result.table = plan.table;
  result.contract = plan.contract;
  const auto summaries = node.datasets();
  const auto found = std::find_if(summaries.begin(), summaries.end(),
                                  [&](const data::DatasetSummary &summary) {
                                    return summary.database == plan.database &&
                                           summary.name == plan.table;
                                  });
  if (found == summaries.end()) {
    result.error =
        "anomaly: no table '" + plan.database + "." + plan.table + "'";
    return result;
  }
  if (found->degraded || found->rows == 0) {
    result.error =
        "anomaly: dataset is empty or degraded: " + found->degraded_reason;
    return result;
  }
  result.table_id = found->table_id;
  result.rows = found->rows;
  const auto present = [&](const std::string &column) {
    return column.empty() ||
           std::ranges::find(found->columns, column) != found->columns.end();
  };
  for (const auto &role :
       {plan.contract.entity_column, plan.contract.label_column,
        plan.contract.time_column}) {
    if (!present(role)) {
      result.error =
          "anomaly: no column '" + role + "' in the pipeline's table";
      return result;
    }
  }
  std::set<std::string> excluded = {"id", plan.contract.entity_column,
                                    plan.contract.label_column,
                                    plan.contract.time_column};
  if (result.contract.feature_columns.empty()) {
    if (!sampler) {
      result.error = "anomaly: coordinated plan must resolve feature_columns "
                     "before launch";
      return result;
    }
    std::string error;
    const auto sample =
        sampler(plan.database, plan.table, data::kDefaultSampleRows, error);
    if (!error.empty() || sample.empty()) {
      result.error = "anomaly: cannot inspect input columns: " + error;
      return result;
    }
    for (const std::string &column : found->columns) {
      if (excluded.contains(column)) {
        continue;
      }
      const bool numeric =
          std::ranges::all_of(sample, [&](const nlohmann::json &row) {
            const auto value = row.find(column);
            return value != row.end() &&
                   (value->is_boolean() ||
                    (value->is_number() &&
                     std::isfinite(value->get<double>())));
          });
      if (numeric) {
        result.contract.feature_columns.push_back(column);
      }
    }
  }
  if (result.contract.feature_columns.empty()) {
    result.error =
        "anomaly: no numeric feature columns remain after role exclusions";
    return result;
  }
  for (const std::string &feature : result.contract.feature_columns) {
    if (!present(feature)) {
      result.error = "anomaly: no feature column '" + feature + "'";
      return result;
    }
  }
  for (const std::string &column : found->columns) {
    if (std::ranges::find(result.contract.feature_columns, column) ==
        result.contract.feature_columns.end()) {
      const std::string reason =
          column == plan.contract.label_column  ? "evaluation only"
          : column == plan.contract.time_column ? "time ordering only"
          : column == plan.contract.entity_column
              ? "row identity only"
              : "not selected as a numeric feature";
      result.notes.push_back(column + ": " + reason);
    }
  }
  return result;
}

struct AnomalyWork {
  explicit AnomalyWork(Node &owner) : node(owner) {
  }
  Node &node;
  std::atomic<bool> done{false};
};

// The background-action owner drains pending owner writes during shutdown,
// just as AsyncChurnRun does. Work itself runs on the shared coroutine pool.
struct AnomalyAction {
  explicit AnomalyAction(std::shared_ptr<AnomalyWork> value)
      : work(std::move(value)) {
  }
  ~AnomalyAction() {
    while (!work->done.load(std::memory_order_acquire)) {
      work->node.drain_owner_work();
      std::this_thread::yield();
    }
  }
  std::shared_ptr<AnomalyWork> work;
};

runtime::task<> execute_anomaly(std::shared_ptr<AnomalyWork> work,
                                std::shared_ptr<pipeline::Run> run,
                                data::ResolvedDataset dataset,
                                AnomalyParams params,
                                compute::CoordinatedLaunchRequest launch,
                                std::string model_name,
                                observability::TraceContext trace) {
  observability::ScopedSpan span(&work->node.span_recorder(), trace,
                                 "anomaly:run");
  work->node.run_on_owner([&] {
    run->set_status(pipeline::RunStatus::Running);
    run->add_log({util::now_iso8601(), "info",
                  "dataset: " + dataset.database + "." + dataset.table});
    for (const std::string &note : dataset.notes) {
      run->add_log({util::now_iso8601(), "info", "dataset: not used: " + note});
    }
    if (launch.participants.size() > 1) {
      work->node.coordinated_launch()->fan_out(launch);
    }
  });
  AnomalyTableOptions options;
  options.round_tag = "ml.anomaly:" + launch.id;
  options.model_name = std::move(model_name);
  options.materialize_on_caller = true;
  const AnomalyTableResult result = run_anomaly_on_table(
      work->node, dataset, params, run.get(), kDefaultAnomalyTimeout, options);
  work->node.run_on_owner([&] {
    if (result.ok()) {
      auto outputs = run->outputs();
      outputs.push_back({"report", "Anomaly findings", run->id()});
      run->set_outputs(std::move(outputs));
      run->add_log(
          {util::now_iso8601(), "info",
           "anomaly: scored " + std::to_string(result.report.rows_scored) +
               " rows; flagged " + std::to_string(result.report.flagged)});
      run->set_status(pipeline::RunStatus::Succeeded);
    } else {
      run->add_log({util::now_iso8601(), "error", result.error});
      run->set_status(pipeline::RunStatus::Failed);
    }
  });
  span.finish(result.ok(), result.error);
  co_return;
}

api::ApiResult run_anomaly(RunRequest &request) {
  if (!request.context || !request.project || !request.graph ||
      !request.context->background) {
    return api::ApiResult::failure(
        "engine_error", "anomaly: missing run context or background owner");
  }
  auto &context = *request.context;
  if (!context.node.job_coordinator() || !context.node.coordinated_launch()) {
    return api::ApiResult::failure(
        "engine_error", "anomaly: attach the compute plane before running");
  }
  // Copy through the validated IR boundary: ExecutionGraph copies share nodes.
  std::string error;
  auto graph = ir::from_json(ir::to_json(request.graph->graph()), &error);
  if (!graph || !parse_anomaly_plan(*graph, error)) {
    return api::ApiResult::failure("invalid_input", error);
  }
  if (request.input.contains("dataset_path")) {
    const auto path = data::parse_dataset_path(
        request.input.at("dataset_path").get<std::string>());
    if (path.kind != data::DatasetPathKind::Table) {
      return api::ApiResult::failure(
          "invalid_input", "anomaly: dataset_path must name a landed table");
    }
    auto source = graph->nodes_of_kind(ir::StepKind::Connect).front();
    source->attributes["source"] = path.database;
    source->attributes["table"] = path.table;
  }
  auto model = graph->nodes_of_kind(ir::StepKind::Model).front();
  if (request.input.contains("dataset")) {
    const auto &given = request.input.at("dataset");
    if (given.contains("as_of")) {
      return api::ApiResult::failure("invalid_input",
                                     "anomaly: prefilter a cutoff in the input "
                                     "table; as_of is not supported");
    }
    for (const char *key :
         {"entity_column", "label_column", "time_column", "feature_columns"}) {
      if (given.contains(key)) {
        model->attributes[key] = given.at(key);
      }
    }
  }
  const auto plan = parse_anomaly_plan(*graph, error);
  if (!plan) {
    return api::ApiResult::failure("invalid_input", error);
  }
  const auto roster =
      context.node.job_coordinator()->collectives().participants();
  if (roster.size() > 1 && (plan->params.mode == AnomalyMode::KpiTimeSeries ||
                            !request.input.value("coordinate", true))) {
    return api::ApiResult::failure(
        "not_supported", "anomaly: multi-member runs require coordinated "
                         "tabular mode; KPI prefix state is not implemented");
  }
  const auto dataset = resolve_anomaly_dataset(
      context.node, *plan,
      data::default_dataset_sampler(context.node, context.cluster));
  if (!dataset.ok()) {
    return api::ApiResult::failure("invalid_input", dataset.error);
  }
  model->attributes["feature_columns"] = dataset.contract.feature_columns;
  auto run =
      context.node.create_run(request.project->id(), request.graph->id());
  if (!run) {
    return api::ApiResult::failure("engine_error",
                                   "anomaly: could not create Run");
  }
  run->set_trace_id(context.trace.trace_id);
  compute::CoordinatedLaunchRequest launch;
  launch.id = run->id();
  launch.kind = std::string(kLaunchKind);
  launch.initiator = context.node.coordinated_launch()->node_id();
  launch.participants = roster;
  // Reuse the existing validated ExecutionGraph wire format. No raw rows cross.
  launch.payload = ir::to_json(*graph);
  auto work = std::make_shared<AnomalyWork>(context.node);
  auto action = std::make_shared<AnomalyAction>(work);
  context.background->adopt(
      action, [work] { return work->done.load(std::memory_order_acquire); });
  runtime::spawn(
      execute_anomaly(work, run, dataset, plan->params, std::move(launch),
                      request.input.value("model_name", std::string{}),
                      context.trace),
      [work] { work->done.store(true, std::memory_order_release); });
  return api::ApiResult::success({{"run_id", run->id()},
                                  {"engine", "anomaly"},
                                  {"status", "running"},
                                  {"poll", "get_run / get_run_report"},
                                  {"dataset", data::to_json(dataset.contract)},
                                  {"dataset_notes", dataset.notes}});
}

} // namespace

Methodology make_anomaly_methodology() {
  Methodology result;
  result.id = "M-7";
  result.name = "Anomaly detection";
  result.family = "Time & vigilance";
  result.model_kinds = {"anomaly", "anomaly_detection", "outlier"};
  result.solutions = {"S-7"};
  result.keywords = {"anomaly", "outlier", "unusual", "spike"};
  result.inputs = {
      {"feature", true, "numeric features or metrics"},
      {"entity", false, "integer row identity; otherwise row ordinal"},
      {"label", false, "binary label used only for precision@k"},
      {"time", false, "required numeric observation time in KPI mode"}};
  result.outputs = {"anomaly table", "budget report", "model", "lineage"};
  result.metrics = {"threshold", "contamination", "alert_budget", "alert_rate",
                    "precision_at_k"};
  result.issue = "https://github.com/oramasearch/zetta/issues/1046";
  result.runnable = true;
  result.build_shape = build_anomaly_shape;
  result.owns = owns_anomaly;
  result.run = run_anomaly;
  return result;
}

void install_anomaly_launch_runner(compute::CoordinatedLaunchListener &listener,
                                   Node &node) {
  listener.set_runner(
      std::string(kLaunchKind),
      [&node, &listener](const compute::CoordinatedLaunchRequest &request) {
        using Outcome = compute::CoordinatedLaunchListener::LaunchOutcome;
        std::string error;
        const auto graph = ir::from_json(request.payload, &error);
        if (!graph || !compute::participant_rank(request, listener.node_id())) {
          return Outcome{
              false, "anomaly launch: invalid graph or participant: " + error};
        }
        const auto plan = parse_anomaly_plan(*graph, error);
        if (!plan || plan->params.mode != AnomalyMode::Tabular) {
          return Outcome{
              false, "anomaly launch: requires a valid tabular plan: " + error};
        }
        const auto dataset = resolve_anomaly_dataset(node, *plan);
        if (!dataset.ok()) {
          return Outcome{false, dataset.error};
        }
        AnomalyTableOptions options;
        options.round_tag = "ml.anomaly:" + request.id;
        options.write_outputs = false;
        const auto result =
            run_anomaly_on_table(node, dataset, plan->params, nullptr,
                                 kDefaultAnomalyTimeout, options);
        return Outcome{result.ok(), result.ok()
                                        ? "anomaly partition co-run completed"
                                        : result.error};
      });
}

nlohmann::json anomaly_run_report(Node &node, const pipeline::Run &run,
                                  const Model &model, std::size_t limit) {
  nlohmann::json result = {
      {"run",
       {{"id", run.id()},
        {"graph", run.graph()},
        {"project", run.project()},
        {"status", pipeline::to_string(run.status())},
        {"finished", run.updated()}}},
      {"methodology", "M-7"},
      {"model",
       {{"id", model.id()},
        {"name", model.name()},
        {"kind", model.kind()},
        {"metrics", model.metrics()}}},
      {"dataset",
       model.feature_schema().value("dataset", nlohmann::json::object())},
      {"output_tables", nlohmann::json::array()},
      {"anomalies", nlohmann::json::array()},
      {"missing", nlohmann::json::array()}};
  for (const auto &output : run.outputs()) {
    if (output.kind != "dataset") {
      continue;
    }
    result["output_tables"].push_back(
        {{"name", output.name}, {"table", output.ref}});
    if (output.name != "anomaly table") {
      continue;
    }
    const auto dot = output.ref.find('.');
    const auto database = node.find_database(output.ref.substr(0, dot));
    const auto table =
        database ? database->find_table(output.ref.substr(dot + 1)) : nullptr;
    if (!table) {
      result["missing"].push_back(
          {{"field", "anomalies"}, {"why", "output table is unavailable"}});
      continue;
    }
    std::vector<nlohmann::json> rows;
    for (std::size_t index = 0; index < table->store().size(); ++index) {
      if (auto row = table->store().at(index)) {
        rows.push_back(*row);
      }
    }
    const auto rank = [](const nlohmann::json &row) {
      const auto value = row.find("rank");
      return value != row.end() && value->is_number() ? value->get<double>()
                                                      : 0.0;
    };
    std::ranges::sort(rows, [&](const auto &left, const auto &right) {
      return rank(left) < rank(right);
    });
    rows.resize(std::min(rows.size(), limit));
    result["anomalies"] = std::move(rows);
  }
  if (!model.metrics().contains("precision_at_k")) {
    result["missing"].push_back(
        {{"field", "precision_at_k"},
         {"why", "no optional evaluation label was supplied"}});
  }
  return result;
}

} // namespace zetta::ml
