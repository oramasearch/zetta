#include "zetta/ml/anomaly_table.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kResultsDatabase = "anomaly_results";

struct TableInput {
  std::vector<AnomalyPartition> local_partitions;
  std::vector<std::string> global_partition_ids;
  std::string error;

  bool ok() const noexcept { return error.empty(); }
};

std::optional<std::string> read_blob_body(Node &node,
                                          const std::string &blob_ref) {
  for (const std::string &database_name : node.database_names()) {
    const std::shared_ptr<data::Database> database =
        node.find_database(database_name);
    if (!database) {
      continue;
    }
    if (std::optional<std::string> body =
            database->large_objects().read(blob_ref)) {
      return body;
    }
  }
  return std::nullopt;
}

std::optional<double> numeric_value(const nlohmann::json &row,
                                    const std::string &column) {
  const auto found = row.find(column);
  if (found == row.end() || found->is_null()) {
    return std::nullopt;
  }
  if (found->is_boolean()) {
    return found->get<bool>() ? 1.0 : 0.0;
  }
  if (found->is_number()) {
    return found->get<double>();
  }
  return std::nullopt;
}

std::optional<std::int64_t> integer_value(const nlohmann::json &row,
                                          const std::string &column) {
  const std::optional<double> value = numeric_value(row, column);
  if (!value || !std::isfinite(*value) || std::trunc(*value) != *value ||
      *value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      *value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(*value);
}

std::string row_error(const data::DatasetPartition &partition, std::size_t row,
                      const std::string &reason) {
  return "anomaly table: partition '" + partition.id() + "' row " +
         std::to_string(row) + ": " + reason;
}

bool append_partition_rows(const data::DatasetPartition &catalog,
                           const std::string &body,
                           const data::DatasetContract &contract,
                           AnomalyMode mode, AnomalyPartition &output,
                           std::string &error) {
  data::PartitionBodyReadResult parsed =
      data::read_partition_body(body, catalog.id());
  if (!parsed.ok()) {
    error = "anomaly table: " + parsed.error;
    return false;
  }
  output.id = catalog.id();
  output.columns.assign(contract.feature_columns.size(), {});
  for (std::size_t row_index = 0; row_index < parsed.rows.size(); ++row_index) {
    const nlohmann::json &row = parsed.rows[row_index].doc;
    const std::uint64_t ordinal = catalog.row_range().start + row_index;
    output.ordinals.push_back(ordinal);
    if (contract.entity_column.empty()) {
      output.row_keys.push_back(static_cast<std::int64_t>(ordinal));
    } else if (const std::optional<std::int64_t> entity =
                   integer_value(row, contract.entity_column)) {
      output.row_keys.push_back(*entity);
    } else {
      error = row_error(catalog, row_index,
                        "entity column '" + contract.entity_column +
                            "' is not an integer");
      return false;
    }
    for (std::size_t feature = 0; feature < contract.feature_columns.size();
         ++feature) {
      const std::string &name = contract.feature_columns[feature];
      const std::optional<double> value = numeric_value(row, name);
      if (!value || !std::isfinite(*value)) {
        error = row_error(catalog, row_index,
                          "feature '" + name + "' is not finite numeric data");
        return false;
      }
      output.columns[feature].push_back(*value);
    }
    if (mode == AnomalyMode::KpiTimeSeries) {
      const std::optional<double> period =
          numeric_value(row, contract.time_column);
      if (!period || !std::isfinite(*period)) {
        error = row_error(catalog, row_index,
                          "time column '" + contract.time_column +
                              "' is not finite numeric data");
        return false;
      }
      output.periods.push_back(*period);
    }
    if (!contract.label_column.empty()) {
      const std::optional<double> label =
          numeric_value(row, contract.label_column);
      if (!label || (*label != 0.0 && *label != 1.0)) {
        error = row_error(catalog, row_index,
                          "optional label column '" + contract.label_column +
                              "' is not binary");
        return false;
      }
      output.labels.push_back(*label);
    }
  }
  return true;
}

TableInput load_table_input(Node &node, const data::ResolvedDataset &dataset,
                            AnomalyMode mode) {
  TableInput input;
  std::vector<std::shared_ptr<data::DatasetPartition>> catalog;
  for (const std::shared_ptr<data::DatasetPartition> &partition :
       node.dataset_partitions()) {
    if (partition && partition->table() == dataset.table_id &&
        !partition->blob_ref().empty()) {
      catalog.push_back(partition);
    }
  }
  std::sort(catalog.begin(), catalog.end(),
            [](const auto &left, const auto &right) {
              if (left->row_range().start != right->row_range().start) {
                return left->row_range().start < right->row_range().start;
              }
              return left->id() < right->id();
            });
  if (catalog.empty()) {
    input.error = "anomaly table: dataset '" + dataset.database + "." +
                  dataset.table + "' has no placed partitions";
    return input;
  }
  for (const std::shared_ptr<data::DatasetPartition> &partition : catalog) {
    const std::vector<std::string> &owners = partition->owner_nodes();
    if (owners.empty()) {
      input.error =
          "anomaly table: partition '" + partition->id() + "' has no owner";
      return input;
    }
    input.global_partition_ids.push_back(partition->id());
    if (owners.front() != node.node_id()) {
      continue;
    }
    const std::optional<std::string> body =
        read_blob_body(node, partition->blob_ref());
    if (!body) {
      input.error = "anomaly table: primary-owned partition '" +
                    partition->id() + "' body is not present locally";
      return input;
    }
    AnomalyPartition local;
    if (!append_partition_rows(*partition, *body, dataset.contract, mode, local,
                               input.error)) {
      return input;
    }
    input.local_partitions.push_back(std::move(local));
  }
  return input;
}

std::string mode_name(AnomalyMode mode) {
  return mode == AnomalyMode::Tabular ? "tabular" : "kpi_time_series";
}

nlohmann::json model_weights(const AnomalyModel &model,
                             const AnomalyParams &params) {
  nlohmann::json trees = nlohmann::json::array();
  for (const IsolationTree &tree : model.trees) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const IsolationTreeNode &node : tree.nodes) {
      nodes.push_back({{"feature", node.feature},
                       {"threshold", node.threshold},
                       {"left", node.left},
                       {"right", node.right},
                       {"leaf_size", node.leaf_size}});
    }
    trees.push_back({{"partition", tree.partition_id},
                     {"sample_size", tree.sample_size},
                     {"nodes", std::move(nodes)}});
  }
  return {{"kind", "anomaly"},
          {"mode", mode_name(model.mode)},
          {"features", model.feature_names},
          {"seed", model.seed},
          {"robust_centers", model.robust_centers},
          {"robust_scales", model.robust_scales},
          {"seasonal_period", model.seasonal_period},
          {"trend_window", model.trend_window},
          {"subsample_size", params.subsample_size},
          {"trees", std::move(trees)}};
}

nlohmann::json
ranked_driver_json(const std::vector<AnomalyRecord> &records,
                   const std::vector<std::string> &feature_names) {
  std::unordered_map<std::string, double> totals;
  for (const AnomalyRecord &record : records) {
    for (const AnomalyContribution &contribution : record.contributions) {
      totals[contribution.feature] += contribution.deviation;
    }
  }
  std::vector<std::pair<std::string, double>> drivers;
  for (const std::string &feature : feature_names) {
    drivers.emplace_back(feature, totals[feature]);
  }
  std::sort(drivers.begin(), drivers.end(),
            [](const auto &left, const auto &right) {
              if (left.second != right.second) {
                return left.second > right.second;
              }
              return left.first < right.first;
            });
  nlohmann::json json = nlohmann::json::array();
  for (const auto &[feature, importance] : drivers) {
    json.push_back(
        {{"feature", feature},
         {"importance",
          records.empty() ? 0.0
                          : importance / static_cast<double>(records.size())}});
  }
  return json;
}

std::string safe_suffix(std::string value) {
  for (char &character : value) {
    if (!std::isalnum(static_cast<unsigned char>(character))) {
      character = '_';
    }
  }
  if (value.size() > 24) {
    value.resize(24);
  }
  return value.empty() ? "run" : value;
}

void materialize_outputs(Node &node, const data::ResolvedDataset &dataset,
                         const AnomalyParams &params, pipeline::Run *run,
                         DistributedAnomalyResult &distributed,
                         AnomalyTableResult &result,
                         const compute::ComputeJob &job) {
  ModelValue model_value;
  model_value.name = "anomaly-" + dataset.table;
  model_value.kind = "anomaly";
  model_value.project = job.project();
  model_value.run = run == nullptr ? std::string{} : run->id();
  model_value.feature_schema = {
      {"mode", mode_name(params.mode)},
      {"features", dataset.contract.feature_columns},
      {"time_column", dataset.contract.time_column},
      {"drivers", ranked_driver_json(distributed.records,
                                     dataset.contract.feature_columns)}};
  model_value.metrics = {{"threshold", distributed.report.threshold},
                         {"rows_scored", distributed.report.rows_scored},
                         {"flagged", distributed.report.flagged},
                         {"alert_rate", distributed.report.alert_rate}};
  if (distributed.report.contamination) {
    model_value.metrics["contamination"] = *distributed.report.contamination;
  }
  if (distributed.report.alert_budget) {
    model_value.metrics["alert_budget"] =
        static_cast<double>(*distributed.report.alert_budget);
  }
  if (distributed.report.precision_at_k) {
    model_value.metrics["precision_at_k"] = *distributed.report.precision_at_k;
  }
  std::shared_ptr<Model> model = node.create_model(std::move(model_value));
  if (!model ||
      !model->set_weights(model_weights(distributed.model, params).dump())) {
    result.error = "anomaly table: could not register the anomaly model";
    return;
  }
  result.model_id = model->id();

  std::shared_ptr<data::Database> database =
      node.find_database(std::string(kResultsDatabase));
  if (!database) {
    database = node.create_database(std::string(kResultsDatabase));
  }
  if (!database) {
    result.error = "anomaly table: could not create the results database";
    return;
  }
  const std::string identity = run == nullptr ? dataset.table_id : run->id();
  const std::string suffix = safe_suffix(identity);
  const std::string anomaly_name = "anomalies_" + suffix;
  const std::string report_name = "report_" + suffix;
  database->drop_table(anomaly_name);
  database->drop_table(report_name);
  const std::shared_ptr<data::Table> anomaly_table =
      database->create_table(anomaly_name);
  const std::shared_ptr<data::Table> report_table =
      database->create_table(report_name);
  if (!anomaly_table || !report_table) {
    result.error = "anomaly table: could not create output tables";
    return;
  }

  std::size_t rank = 0;
  std::vector<std::size_t> ranking(distributed.records.size());
  std::iota(ranking.begin(), ranking.end(), 0);
  std::sort(ranking.begin(), ranking.end(),
            [&](std::size_t left, std::size_t right) {
              if (distributed.records[left].score !=
                  distributed.records[right].score) {
                return distributed.records[left].score >
                       distributed.records[right].score;
              }
              return distributed.records[left].ordinal <
                     distributed.records[right].ordinal;
            });
  std::vector<std::size_t> rank_by_record(distributed.records.size());
  for (const std::size_t index : ranking) {
    rank_by_record[index] = ++rank;
  }
  for (std::size_t index = 0; index < distributed.records.size(); ++index) {
    const AnomalyRecord &record = distributed.records[index];
    nlohmann::json contributions = nlohmann::json::array();
    for (const AnomalyContribution &contribution : record.contributions) {
      contributions.push_back({{"feature", contribution.feature},
                               {"deviation", contribution.deviation}});
    }
    nlohmann::json row = {{"rank", rank_by_record[index]},
                          {"row", record.row},
                          {"score", record.score},
                          {"flagged", record.flagged},
                          {"threshold", distributed.report.threshold},
                          {"contributors", std::move(contributions)}};
    if (record.period) {
      row["period"] = *record.period;
    }
    if (!record.contributions.empty()) {
      row["top_contributor"] = record.contributions.front().feature;
      row["deviation"] = record.contributions.front().deviation;
    }
    if (record.label) {
      row["label"] = *record.label;
    }
    anomaly_table->add_row(std::move(row));
  }

  nlohmann::json report_row = {
      {"threshold", distributed.report.threshold},
      {"rows_scored", distributed.report.rows_scored},
      {"flagged", distributed.report.flagged},
      {"alert_rate", distributed.report.alert_rate},
      {"contamination", distributed.report.contamination
                            ? nlohmann::json(*distributed.report.contamination)
                            : nlohmann::json(nullptr)},
      {"alert_budget", distributed.report.alert_budget
                           ? nlohmann::json(*distributed.report.alert_budget)
                           : nlohmann::json(nullptr)},
      {"precision_at_k",
       distributed.report.precision_at_k
           ? nlohmann::json(*distributed.report.precision_at_k)
           : nlohmann::json(nullptr)}};
  report_table->add_row(std::move(report_row));
  result.anomaly_table = std::string(kResultsDatabase) + "." + anomaly_name;
  result.report_table = std::string(kResultsDatabase) + "." + report_name;

  if (run != nullptr) {
    std::vector<pipeline::RunOutput> outputs = run->outputs();
    outputs.push_back({"model", "anomaly model", result.model_id});
    outputs.push_back({"dataset", "anomaly table", result.anomaly_table});
    outputs.push_back({"dataset", "anomaly report", result.report_table});
    run->set_outputs(std::move(outputs));
    pipeline::RunLineage lineage = run->lineage();
    lineage.sources.push_back(dataset.database + "." + dataset.table);
    lineage.steps.push_back("anomaly:train");
    lineage.steps.push_back("anomaly:score");
    lineage.freshness = util::now_iso8601();
    lineage.confidence = 1.0;
    run->set_lineage(std::move(lineage));
  }
}

} // namespace

AnomalyTableResult run_anomaly_on_table(Node &node,
                                        const data::ResolvedDataset &dataset,
                                        const AnomalyParams &params,
                                        pipeline::Run *run,
                                        std::chrono::milliseconds timeout) {
  AnomalyTableResult result;
  if (!dataset.ok()) {
    result.error = "anomaly table: unresolved dataset: " + dataset.error;
    return result;
  }
  if (dataset.table_id.empty() || dataset.contract.feature_columns.empty()) {
    result.error =
        "anomaly table: the resolved dataset needs a table id and features";
    return result;
  }
  if (params.mode == AnomalyMode::KpiTimeSeries &&
      dataset.contract.time_column.empty()) {
    result.error = "anomaly table: KPI mode needs an explicit time column";
    return result;
  }
  compute::JobCoordinator *coordinator = node.job_coordinator();
  if (coordinator == nullptr) {
    result.error = "anomaly table: no compute plane is attached to this node";
    return result;
  }

  TableInput input;
  std::shared_ptr<compute::ComputeJob> job;
  node.run_on_owner([&] {
    input = load_table_input(node, dataset, params.mode);
    job = node.create_job(compute::JobKind::Train,
                          run == nullptr ? std::string{} : run->project(),
                          run == nullptr ? std::string{} : run->graph(),
                          run == nullptr ? std::string{} : run->id());
  });
  if (!input.ok()) {
    result.error = input.error;
    return result;
  }
  if (!job) {
    result.error = "anomaly table: could not create the training job";
    return result;
  }

  DistributedAnomalyResult distributed = run_anomaly_job(
      *coordinator, *job, input.local_partitions, input.global_partition_ids,
      dataset.contract.feature_columns, params,
      "ml.anomaly:" + dataset.table_id, timeout);
  result.status = distributed.status;
  result.records = distributed.records;
  result.report = distributed.report;
  if (!distributed.ok()) {
    result.error = job->detail();
    return result;
  }
  if (distributed.is_root) {
    node.run_on_owner([&] {
      materialize_outputs(node, dataset, params, run, distributed, result,
                          *job);
    });
  }
  return result;
}

} // namespace zetta::ml
