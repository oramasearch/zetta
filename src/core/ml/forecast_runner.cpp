#include "zetta/ml/forecast_runner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "zetta/cluster/collectives.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr std::int64_t kMaximumExactDoubleInteger = 9'007'199'254'740'992LL;

zetta::cluster::CollectiveOp collective_operation(const GbdtReduce operation) {
  switch (operation) {
  case GbdtReduce::Sum:
    return zetta::cluster::CollectiveOp::Sum;
  case GbdtReduce::Min:
    return zetta::cluster::CollectiveOp::Minimum;
  case GbdtReduce::Max:
    return zetta::cluster::CollectiveOp::Maximum;
  }
  return zetta::cluster::CollectiveOp::Sum;
}

std::vector<double> flatten_forecast(const std::vector<ForecastPoint> &rows) {
  std::vector<double> flat;
  flat.reserve(rows.size() * 5);
  for (const ForecastPoint &row : rows) {
    flat.push_back(static_cast<double>(row.series));
    flat.push_back(row.timestamp);
    flat.push_back(row.point);
    flat.push_back(row.lower);
    flat.push_back(row.upper);
  }
  return flat;
}

std::vector<ForecastPoint>
expand_forecast(const std::vector<std::vector<double>> &gathered) {
  std::vector<ForecastPoint> rows;
  for (const std::vector<double> &member : gathered) {
    if (member.size() % 5 != 0)
      return {};
    for (std::size_t index = 0; index < member.size(); index += 5) {
      rows.push_back(ForecastPoint{
          .series = static_cast<std::int64_t>(member[index]),
          .timestamp = member[index + 1],
          .point = member[index + 2],
          .lower = member[index + 3],
          .upper = member[index + 4],
      });
    }
  }
  std::ranges::sort(rows,
                    [](const ForecastPoint &left, const ForecastPoint &right) {
                      if (left.series != right.series)
                        return left.series < right.series;
                      return left.timestamp < right.timestamp;
                    });
  return rows;
}

bool series_ownership_is_disjoint(
    compute::JobRound &round, const std::vector<ForecastSeries> &local_series,
    const std::string &tag, const std::chrono::milliseconds timeout) {
  std::vector<double> keys;
  keys.reserve(local_series.size());
  double local_valid = 1.0;
  for (const ForecastSeries &series : local_series) {
    if (series.key < -kMaximumExactDoubleInteger ||
        series.key > kMaximumExactDoubleInteger) {
      local_valid = 0.0;
      continue;
    }
    keys.push_back(static_cast<double>(series.key));
  }
  const auto keys_valid =
      round.allreduce(tag + ":series-keys-valid", {local_valid},
                      zetta::cluster::CollectiveOp::Minimum, timeout);
  if (!keys_valid || keys_valid->size() != 1 || keys_valid->front() != 1.0)
    return false;
  const auto gathered = round.gather(tag + ":series-owners", keys, timeout);
  if (!gathered)
    return false;
  double valid = 1.0;
  if (round.is_root()) {
    std::set<std::int64_t> owners;
    for (const std::vector<double> &member : *gathered) {
      for (const double key : member) {
        if (!owners.insert(static_cast<std::int64_t>(key)).second)
          valid = 0.0;
      }
    }
  }
  const auto agreed =
      round.broadcast(tag + ":series-owners:valid", {valid}, timeout);
  return agreed && agreed->size() == 1 && agreed->front() == 1.0;
}

nlohmann::json model_metrics(const ForecastMetrics &metrics) {
  nlohmann::json result = {
      {"backtest_mae", metrics.mean.mae},
      {"backtest_mase", metrics.mean.mase},
      {"backtest_smape", metrics.mean.smape},
      {"seasonal_naive_mae", metrics.mean.naive_mae},
      {"forecast_value_added", metrics.mean.forecast_value_added},
      {"interval_coverage", metrics.mean.interval_coverage},
      {"interval_level", metrics.interval_level},
      {"interval_half_width", metrics.interval_half_width},
      {"backtest_folds", metrics.folds.size()},
      {"beats_seasonal_naive", metrics.beats_seasonal_naive ? 1.0 : 0.0},
  };
  for (const ForecastFoldMetrics &fold : metrics.folds) {
    const std::string prefix = "fold_" + std::to_string(fold.fold) + "_";
    result[prefix + "mae"] = fold.mae;
    result[prefix + "mase"] = fold.mase;
    result[prefix + "smape"] = fold.smape;
    result[prefix + "seasonal_naive_mae"] = fold.naive_mae;
    result[prefix + "forecast_value_added"] = fold.forecast_value_added;
    result[prefix + "interval_coverage"] = fold.interval_coverage;
    result[prefix + "rows"] = fold.rows;
  }
  return result;
}

bool write_root_outputs(Node &node, const ForecastRunParams &params,
                        const std::string &source_name,
                        ForecastRunResult &output) {
  std::shared_ptr<data::Database> database =
      node.find_database(params.output_database);
  if (!database)
    database = node.create_database(params.output_database);
  if (!database) {
    output.error = "forecast runner: could not create output database '" +
                   params.output_database + "'";
    return false;
  }
  if (database->find_table(params.output_table)) {
    output.error = "forecast runner: output table '" + params.output_database +
                   "." + params.output_table + "' already exists";
    return false;
  }
  const std::shared_ptr<data::Table> table =
      database->create_table(params.output_table);
  if (!table) {
    output.error = "forecast runner: could not create output table";
    return false;
  }
  for (const ForecastPoint &row : output.result.forecast) {
    const double interval_half_width = (row.upper - row.lower) / 2.0;
    const double exception_score =
        interval_half_width / std::max(std::abs(row.point), 1.0);
    table->add_row(nlohmann::json{{"series", row.series},
                                  {"timestamp", row.timestamp},
                                  {"point_forecast", row.point},
                                  {"lower", row.lower},
                                  {"upper", row.upper},
                                  {"exception_score", exception_score}});
  }
  output.forecast_table = params.output_database + "." + params.output_table;

  ModelValue model_value;
  model_value.name = params.model_name;
  model_value.kind = "forecast";
  model_value.project = params.project_id;
  model_value.run = params.run_id;
  model_value.feature_schema = {
      {"model_kind", "forecast"},
      {"features", output.result.feature_names},
      {"params", params.forecast.to_json()},
      {"prediction_interval", params.forecast.interval_level},
      {"calibration", false},
      {"baseline_comparison", output.result.metrics.comparison},
      {"dataset", params.dataset_facts},
  };
  model_value.metrics = model_metrics(output.result.metrics);
  const std::shared_ptr<Model> model =
      node.create_model(std::move(model_value));
  if (!model || !model->set_weights(output.result.model.to_json().dump())) {
    output.error = "forecast runner: could not register forecast model";
    return false;
  }
  output.model_id = model->id();
  output.outputs = {
      pipeline::RunOutput{"dataset", "forecast", output.forecast_table},
      pipeline::RunOutput{"model", params.model_name, output.model_id},
  };
  output.lineage.sources = {"dataset:" + source_name};
  output.lineage.steps = {"forecast:temporal-backtest", "forecast:train",
                          "forecast:write"};
  output.lineage.freshness = util::now_iso8601();
  output.lineage.confidence = output.result.metrics.interval_level;
  return true;
}

std::vector<ForecastSeries> series_from_frame(const ddf::DataFrame &frame,
                                              const std::string &series_column,
                                              const std::string &time_column,
                                              const std::string &target_column,
                                              const std::optional<double> as_of,
                                              std::string &error) {
  std::vector<double> times;
  std::vector<double> targets;
  if (const compute::ComputeStatus status = frame.to_host(time_column, times);
      !status.ok()) {
    error = status.message;
    return {};
  }
  if (const compute::ComputeStatus status =
          frame.to_host(target_column, targets);
      !status.ok()) {
    error = status.message;
    return {};
  }
  std::vector<std::int64_t> keys(times.size(), 0);
  if (!series_column.empty()) {
    if (const compute::ComputeStatus status =
            frame.to_host(series_column, keys);
        !status.ok()) {
      error = status.message;
      return {};
    }
  }
  std::map<std::int64_t, ForecastSeries> grouped;
  for (std::size_t row = 0; row < times.size(); ++row) {
    if (as_of && times[row] > *as_of) {
      continue;
    }
    ForecastSeries &series = grouped[keys[row]];
    series.key = keys[row];
    series.times.push_back(times[row]);
    series.values.push_back(targets[row]);
  }
  std::vector<ForecastSeries> result;
  result.reserve(grouped.size());
  for (auto &[ignored, series] : grouped) {
    (void)ignored;
    result.push_back(std::move(series));
  }
  return result;
}

} // namespace

ForecastRunResult
run_forecast_job(compute::JobCoordinator &coordinator, compute::ComputeJob &job,
                 const std::vector<ForecastSeries> &local_series,
                 const ForecastRunParams &params, Node *node,
                 const std::string &source_name,
                 const std::chrono::milliseconds timeout) {
  ForecastRunResult output;
  const std::string tag = "forecast:" + std::to_string(params.forecast.seed) +
                          ":" + params.output_database + ":" +
                          params.output_table;
  output.status = coordinator.run(job, [&](compute::JobRound &round) {
    output.is_root = round.is_root();
    if (!series_ownership_is_disjoint(round, local_series, tag, timeout)) {
      output.error =
          "forecast runner: series ownership is invalid: a series spans "
          "members or its key exceeds the numeric collective range; partition "
          "the table by series so each history has one owner";
      return compute::JobCoordinator::Outcome{false, output.error};
    }
    const AllReduce allreduce = [&](const std::string &round_tag,
                                    const std::vector<double> &local,
                                    const GbdtReduce operation) {
      const auto reduced = round.allreduce(
          round_tag, local, collective_operation(operation), timeout);
      return reduced.value_or(std::vector<double>{});
    };
    output.result = train_forecast_partitions(local_series, params.forecast,
                                              allreduce, tag);
    if (!output.result.ok()) {
      output.error = output.result.status.message;
      return compute::JobCoordinator::Outcome{false, output.error};
    }
    const auto gathered =
        round.gather(tag + ":forecast-rows",
                     flatten_forecast(output.result.forecast), timeout);
    if (!gathered) {
      output.error = "forecast runner: gathering forecast rows failed";
      return compute::JobCoordinator::Outcome{false, output.error};
    }
    double outputs_written = 1.0;
    if (round.is_root()) {
      output.result.forecast = expand_forecast(*gathered);
      if (output.result.forecast.empty()) {
        output.error = "forecast runner: gathered forecast rows were invalid";
        outputs_written = 0.0;
      } else if (node != nullptr) {
        node->run_on_owner([&] {
          if (!write_root_outputs(*node, params, source_name, output)) {
            outputs_written = 0.0;
          }
        });
      }
    }
    const auto agreed =
        round.broadcast(tag + ":outputs-written", {outputs_written}, timeout);
    if (!agreed || agreed->size() != 1 || agreed->front() != 1.0) {
      if (output.error.empty())
        output.error = "forecast runner: root could not write forecast outputs";
      return compute::JobCoordinator::Outcome{false, output.error};
    }
    // The initiating member need not be root. Share the artifact id so its
    // durable Run refers to the same replicated model and output table.
    std::vector<double> model_id_bytes(output.model_id.begin(),
                                       output.model_id.end());
    const auto artifact =
        round.broadcast(tag + ":model-id", model_id_bytes, timeout);
    if (!artifact) {
      output.error = "forecast runner: broadcasting output references failed";
      return compute::JobCoordinator::Outcome{false, output.error};
    }
    if (!round.is_root() && !artifact->empty()) {
      output.model_id.assign(artifact->begin(), artifact->end());
      output.forecast_table =
          params.output_database + "." + params.output_table;
      output.outputs = {{"dataset", "forecast", output.forecast_table},
                        {"model", params.model_name, output.model_id}};
      output.lineage.sources = {"dataset:" + source_name};
      output.lineage.steps = {"forecast:temporal-backtest", "forecast:train",
                              "forecast:write"};
      output.lineage.freshness = util::now_iso8601();
      output.lineage.confidence = params.forecast.interval_level;
    }
    return compute::JobCoordinator::Outcome{
        true, "forecast rows=" + std::to_string(output.result.forecast.size()) +
                  ", backtest_folds=" +
                  std::to_string(output.result.metrics.folds.size())};
  });
  return output;
}

ForecastRunResult run_forecast_table(Node &node, compute::ComputeDevice &device,
                                     const data::ResolvedDataset &dataset,
                                     const ForecastRunParams &params,
                                     const std::chrono::milliseconds timeout) {
  ForecastRunResult output;
  if (!dataset.ok()) {
    output.error = "forecast runner: unresolved dataset: " + dataset.error;
    return output;
  }
  compute::JobCoordinator *coordinator = node.job_coordinator();
  if (coordinator == nullptr) {
    output.error = "forecast runner: no compute plane attached to the node";
    return output;
  }
  const std::string series_column = params.series_column.empty()
                                        ? dataset.contract.entity_column
                                        : params.series_column;
  const std::string time_column = params.time_column.empty()
                                      ? dataset.contract.time_column
                                      : params.time_column;
  const std::string target_column = params.target_column.empty()
                                        ? dataset.contract.label_column
                                        : params.target_column;
  if (time_column.empty() || target_column.empty()) {
    output.error =
        "forecast runner: time_column and target_column are required";
    return output;
  }

  ddf::DataFrameSchema schema;
  if (!series_column.empty()) {
    schema.columns.push_back(ddf::ColumnSchema{
        .name = series_column,
        .field = series_column,
        .dtype = compute::DataType::Int64,
    });
  }
  schema.columns.push_back(ddf::ColumnSchema{
      .name = time_column,
      .field = time_column,
      .dtype = compute::DataType::Float64,
      .timestamp = true,
  });
  schema.columns.push_back(ddf::ColumnSchema{
      .name = target_column,
      .field = target_column,
      .dtype = compute::DataType::Float64,
  });
  ddf::LocalShardResult shard;
  node.run_on_owner([&] {
    shard = ddf::load_local_partitions(node, dataset.table_id, schema, device);
  });
  std::string conversion_error;
  std::vector<ForecastSeries> local_series;
  if (shard.ok()) {
    local_series = series_from_frame(shard.frame, series_column, time_column,
                                     target_column, params.as_of,
                                     conversion_error);
  }
  const std::string preflight = "forecast:load:" + params.run_id + ":" +
                                params.output_database + ":" +
                                params.output_table;
  const auto valid = coordinator->collectives().allreduce(
      preflight + ":valid",
      {shard.ok() && conversion_error.empty() ? 1.0 : 0.0},
      cluster::CollectiveOp::Minimum, timeout);
  if (!valid || valid->size() != 1 || valid->front() != 1.0) {
    output.error = "forecast runner: a member could not load its history: " +
                   (shard.ok() ? conversion_error : shard.status.message);
    return output;
  }
  const auto counts = coordinator->collectives().allreduce(
      preflight + ":counts",
      {static_cast<double>(shard.rows_read),
       static_cast<double>(shard.partitions_read),
       static_cast<double>(shard.bytes_read)},
      cluster::CollectiveOp::Sum, timeout);
  if (!counts || counts->size() != 3 ||
      (dataset.rows != 0 &&
       counts->front() != static_cast<double>(dataset.rows))) {
    output.error = "forecast runner: owned rows do not cover the resolved "
                   "table exactly once";
    return output;
  }
  std::shared_ptr<compute::ComputeJob> job;
  node.run_on_owner([&] {
    job = node.create_job(compute::JobKind::Train, params.project_id, {},
                          params.run_id);
  });
  if (!job) {
    output.error = "forecast runner: could not create train job";
    return output;
  }
  ForecastRunParams resolved_params = params;
  resolved_params.dataset_facts["rows_read"] = counts->at(0);
  resolved_params.dataset_facts["partitions_read"] = counts->at(1);
  resolved_params.dataset_facts["bytes_read"] = counts->at(2);
  output =
      run_forecast_job(*coordinator, *job, local_series, resolved_params, &node,
                       dataset.database + "." + dataset.table, timeout);
  output.rows_read = static_cast<std::uint64_t>(counts->at(0));
  output.partitions_read = static_cast<std::size_t>(counts->at(1));
  output.bytes_read = static_cast<std::uint64_t>(counts->at(2));
  return output;
}

} // namespace zetta::ml
