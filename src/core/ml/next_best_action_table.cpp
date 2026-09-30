#include "zetta/ml/next_best_action_table.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/cpu_device.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

using compute::DataType;
using ddf::ColumnSchema;
using ddf::DataFrameSchema;

constexpr std::string_view kEntity = "entity";
constexpr std::string_view kAction = "action";
constexpr std::string_view kResponse = "response";
constexpr std::string_view kValue = "action_value";
constexpr std::string_view kCost = "action_cost";
constexpr std::string_view kEligible = "eligible";
constexpr std::string_view kContacts = "contacts";
constexpr std::string_view kContactCap = "contact_cap";
constexpr std::string_view kInteractionCount = "interaction_count";
constexpr std::string_view kInteractionTime = "interaction_time";
constexpr std::string_view kLoggingPropensity = "logging_propensity";
constexpr std::string_view kResultsDatabase = "recommendation_results";

DataFrameSchema table_schema(const data::ResolvedDataset &dataset,
                             const NextBestActionTableParams &params) {
  DataFrameSchema schema;
  schema.columns = {{.name = std::string(kEntity),
                     .field = dataset.contract.entity_column,
                     .dtype = DataType::Int64},
                    {.name = std::string(kAction),
                     .field = params.action_column,
                     .dtype = DataType::Int64},
                    {.name = std::string(kResponse),
                     .field = dataset.contract.label_column,
                     .dtype = DataType::Float64},
                    {.name = std::string(kValue),
                     .field = params.value_column,
                     .dtype = DataType::Float64},
                    {.name = std::string(kCost),
                     .field = params.cost_column,
                     .dtype = DataType::Float64},
                    {.name = std::string(kEligible),
                     .field = params.eligibility_column,
                     .dtype = DataType::Float64},
                    {.name = std::string(kContacts),
                     .field = params.contacts_column,
                     .dtype = DataType::Float64},
                    {.name = std::string(kContactCap),
                     .field = params.contact_cap_column,
                     .dtype = DataType::Float64},
                    {.name = std::string(kInteractionCount),
                     .field = params.interaction_count_column,
                     .dtype = DataType::Float64},
                    {.name = std::string(kInteractionTime),
                     .field = params.interaction_time_column,
                     .dtype = DataType::Float64,
                     .timestamp = true}};
  if (!params.logging_propensity_column.empty()) {
    schema.columns.push_back({.name = std::string(kLoggingPropensity),
                              .field = params.logging_propensity_column,
                              .dtype = DataType::Float64});
  }
  for (const std::string &feature : dataset.contract.feature_columns) {
    schema.columns.push_back(
        {.name = feature, .field = feature, .dtype = DataType::Float64});
  }
  return schema;
}

template <class Element>
bool read_column(const ddf::DataFrame &frame, std::string_view name,
                 std::vector<Element> &values, std::string &error) {
  const compute::ComputeStatus status = frame.to_host<Element>(name, values);
  if (status.ok()) {
    return true;
  }
  error = "next best action: read column '" + std::string(name) +
          "': " + status.message;
  return false;
}

} // namespace

namespace {

NextBestActionTableResult
run_table(Node &node, const data::ResolvedDataset &dataset,
          const NextBestActionTableParams &params, pipeline::Run *run,
          const std::string &project_id, const std::string &graph_id,
          const std::string &run_id, bool materialize_outputs,
          std::chrono::milliseconds timeout) {
  NextBestActionTableResult output;
  if (!dataset.ok() || dataset.table_id.empty()) {
    output.error = dataset.error.empty()
                       ? "next best action: unresolved dataset"
                       : dataset.error;
    return output;
  }
  if (dataset.contract.entity_column.empty() ||
      dataset.contract.label_column.empty() ||
      dataset.contract.feature_columns.empty()) {
    output.error = "next best action: dataset contract requires entity, label "
                   "and features";
    return output;
  }
  compute::JobCoordinator *coordinator = node.job_coordinator();
  if (coordinator == nullptr) {
    output.error = "next best action: attach the compute plane before running";
    return output;
  }

  compute::CpuDevice device;
  ddf::LocalShardResult shard;
  node.run_on_owner([&] {
    shard = ddf::load_local_partitions(node, dataset.table_id,
                                       table_schema(dataset, params), device);
  });
  if (!shard.ok()) {
    output.error =
        "next best action: load owned partitions: " + shard.status.message;
    return output;
  }
  output.partitions_read = shard.partitions_read;
  output.bytes_read = shard.bytes_read;

  const std::size_t rows = shard.frame.row_count();
  std::vector<std::int64_t> entities;
  std::vector<std::int64_t> actions;
  std::vector<double> responses;
  std::vector<double> values;
  std::vector<double> costs;
  std::vector<double> eligible;
  std::vector<double> contacts;
  std::vector<double> caps;
  std::vector<double> counts;
  std::vector<double> times;
  std::vector<double> propensities;
  std::string error;
  if (!read_column(shard.frame, kEntity, entities, error) ||
      !read_column(shard.frame, kAction, actions, error) ||
      !read_column(shard.frame, kResponse, responses, error) ||
      !read_column(shard.frame, kValue, values, error) ||
      !read_column(shard.frame, kCost, costs, error) ||
      !read_column(shard.frame, kEligible, eligible, error) ||
      !read_column(shard.frame, kContacts, contacts, error) ||
      !read_column(shard.frame, kContactCap, caps, error) ||
      !read_column(shard.frame, kInteractionCount, counts, error) ||
      !read_column(shard.frame, kInteractionTime, times, error)) {
    output.error = std::move(error);
    return output;
  }
  if (!params.logging_propensity_column.empty() &&
      !read_column(shard.frame, kLoggingPropensity, propensities, error)) {
    output.error = std::move(error);
    return output;
  }
  std::vector<std::vector<double>> features(
      dataset.contract.feature_columns.size());
  for (std::size_t index = 0; index < features.size(); ++index) {
    if (!read_column(shard.frame, dataset.contract.feature_columns[index],
                     features[index], error)) {
      output.error = std::move(error);
      return output;
    }
  }

  std::vector<NextBestActionObservation> observations;
  observations.reserve(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    NextBestActionObservation observation;
    observation.entity = entities[row];
    observation.action = actions[row];
    observation.response = responses[row];
    observation.value = values[row];
    observation.cost = costs[row];
    observation.eligible = eligible[row] > 0.5;
    observation.contacts = contacts[row];
    observation.contact_cap = caps[row];
    observation.interaction_count = counts[row];
    observation.interaction_time = times[row];
    if (!propensities.empty()) {
      observation.logging_propensity = propensities[row];
    }
    observation.features.reserve(features.size());
    for (const std::vector<double> &feature : features) {
      observation.features.push_back(feature[row]);
    }
    observations.push_back(std::move(observation));
  }

  std::shared_ptr<compute::ComputeJob> job;
  node.run_on_owner([&] {
    job =
        node.create_job(compute::JobKind::Train, project_id, graph_id, run_id);
  });
  if (!job) {
    output.error = "next best action: could not create train job";
    return output;
  }
  DistributedNextBestActionResult trained = run_next_best_action_job(
      *coordinator, *job, observations, dataset.contract.feature_columns,
      params.model, params.round_tag, timeout);
  static_cast<DistributedNextBestActionResult &>(output) = std::move(trained);
  if (!output.ok() || !materialize_outputs) {
    return output;
  }
  if (run == nullptr) {
    output.error = "next best action: output materialization requires a Run";
    output.status = compute::JobStatus::Failed;
    return output;
  }

  node.run_on_owner([&] {
    std::shared_ptr<data::Database> database =
        node.find_database(std::string(kResultsDatabase));
    if (!database) {
      database = node.create_database(std::string(kResultsDatabase));
    }
    if (!database) {
      output.error = "next best action: could not create results database";
      return;
    }
    const std::string suffix = run->id().substr(0, 8);
    const std::string recommendations_name = "recommendations_" + suffix;
    const std::string report_name = "policy_report_" + suffix;
    std::shared_ptr<data::Table> recommendations =
        database->create_table(recommendations_name);
    std::shared_ptr<data::Table> report = database->create_table(report_name);
    if (!recommendations || !report) {
      output.error = "next best action: could not create output tables";
      return;
    }
    for (const RankedAction &recommendation : output.recommendations) {
      recommendations->add_row(
          {{"entity", recommendation.entity},
           {"action", recommendation.action},
           {"rank", recommendation.rank},
           {"propensity", recommendation.propensity},
           {"affinity", recommendation.affinity},
           {"expected_value", recommendation.expected_value},
           {"policy_score", recommendation.policy_score},
           {"cost", recommendation.cost},
           {"eligible", recommendation.eligible},
           {"capped", recommendation.capped},
           {"chosen", recommendation.chosen},
           {"why", recommendation.why}});
    }
    nlohmann::json report_row = {
        {"rows", output.rows},
        {"action_models", output.propensity_models.size()},
        {"hit_rate_at_k", output.hit_rate_at_k},
        {"popularity_hit_rate_at_k", output.popularity_hit_rate_at_k},
        {"holdout_interactions", output.holdout_interactions},
        {"holdout_cutoff", output.holdout_cutoff},
        {"offline_evaluation_available", output.offline.available},
        {"inverse_propensity_value", output.offline.inverse_propensity_value},
        {"doubly_robust_value", output.offline.doubly_robust_value},
        {"effective_sample_size", output.offline.effective_sample_size},
        {"maximum_clipped_weight", output.offline.maximum_weight}};
    if (!output.offline.refusal.empty()) {
      report_row["offline_evaluation_refusal"] = output.offline.refusal;
    }
    report_row["causal_caveat"] =
        "propensity is not causal lift; use M-6 to decide whether to treat";
    report->add_row(std::move(report_row));

    ml::ModelValue model;
    model.name = params.model_name;
    model.kind = "next_best_action";
    model.project = run->project();
    model.run = run->id();
    model.created = util::now_iso8601();
    nlohmann::json dataset_facts = params.dataset_facts;
    if (!dataset_facts.is_object() || dataset_facts.empty()) {
      dataset_facts = {{"source", dataset.database + "." + dataset.table},
                       {"catalog_rows", dataset.rows}};
    }
    dataset_facts["rows_read"] = output.rows;
    dataset_facts["local_partitions_read"] = output.partitions_read;
    dataset_facts["local_bytes_read"] = output.bytes_read;
    model.feature_schema = {
        {"features", dataset.contract.feature_columns},
        {"action_column", params.action_column},
        {"dataset", std::move(dataset_facts)},
        {"offline_evaluation_refusal", output.offline.refusal},
        {"causal_caveat",
         "propensity is not causal lift; use M-6 to decide whether to treat"}};
    model.metrics = {
        {"rows", output.rows},
        {"action_models", output.propensity_models.size()},
        {"hit_rate_at_k", output.hit_rate_at_k},
        {"popularity_hit_rate_at_k", output.popularity_hit_rate_at_k},
        {"holdout_interactions", output.holdout_interactions},
        {"offline_evaluation_available", output.offline.available ? 1.0 : 0.0},
        {"inverse_propensity_value", output.offline.inverse_propensity_value},
        {"doubly_robust_value", output.offline.doubly_robust_value},
        {"effective_sample_size", output.offline.effective_sample_size},
        {"maximum_clipped_weight", output.offline.maximum_weight}};
    const std::shared_ptr<ml::Model> stored =
        node.create_model(std::move(model));
    if (!stored) {
      output.error = "next best action: could not register model";
      return;
    }
    output.model_id = stored->id();
    output.recommendation_table =
        std::string(kResultsDatabase) + "." + recommendations_name;
    output.policy_report_table =
        std::string(kResultsDatabase) + "." + report_name;
    run->set_outputs(
        {{"dataset", "recommendation table", output.recommendation_table},
         {"dataset", "policy report", output.policy_report_table},
         {"model", params.model_name, output.model_id},
         {"report", "next-best-action report", run->id()}});
    run->set_lineage(
        {.sources = {"dataset:" + dataset.database + "." + dataset.table},
         .steps = {"propensity GBDT", "implicit ALS",
                   "eligibility/contact-cap policy", "budget"},
         .freshness = util::now_iso8601(),
         .confidence = output.offline.available ? 1.0 : 0.8});
  });
  if (!output.error.empty()) {
    output.status = compute::JobStatus::Failed;
  }
  return output;
}

} // namespace

NextBestActionTableResult
run_next_best_action_on_table(Node &node, const data::ResolvedDataset &dataset,
                              const NextBestActionTableParams &params,
                              pipeline::Run &run,
                              std::chrono::milliseconds timeout) {
  return run_table(node, dataset, params, &run, run.project(), run.graph(),
                   run.id(), true, timeout);
}

NextBestActionTableResult run_next_best_action_table_participant(
    Node &node, const data::ResolvedDataset &dataset,
    const NextBestActionTableParams &params, const std::string &project_id,
    const std::string &graph_id, const std::string &run_id,
    std::chrono::milliseconds timeout) {
  return run_table(node, dataset, params, nullptr, project_id, graph_id, run_id,
                   false, timeout);
}

} // namespace zetta::ml
