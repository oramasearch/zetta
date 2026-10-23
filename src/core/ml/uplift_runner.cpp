#include "zetta/ml/uplift_runner.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
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
#include "zetta/ml/uplift_job.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

using compute::DataType;

ddf::DataFrameSchema uplift_schema(const data::DatasetContract &contract,
                                   const UpliftRunOptions &options) {
  ddf::DataFrameSchema schema;
  schema.columns.push_back(ddf::ColumnSchema{.name = contract.entity_column,
                                             .field = contract.entity_column,
                                             .dtype = DataType::Int64,
                                             .entity_keys = contract.entity_keys});
  schema.columns.push_back(ddf::ColumnSchema{.name = options.treatment_column,
                                             .field = options.treatment_column,
                                             .dtype = DataType::Float64});
  schema.columns.push_back(ddf::ColumnSchema{.name = options.outcome_column,
                                             .field = options.outcome_column,
                                             .dtype = DataType::Float64});
  for (const std::string &feature : contract.feature_columns) {
    schema.columns.push_back(ddf::ColumnSchema{
        .name = feature, .field = feature, .dtype = DataType::Float64});
  }
  return schema;
}

std::shared_ptr<data::Table>
source_table(Node &node, const data::ResolvedDataset &dataset) {
  const std::shared_ptr<data::Database> database =
      node.find_database(dataset.database);
  return database ? database->find_table(dataset.table) : nullptr;
}

nlohmann::json model_weights(const UpliftModelBundle &models) {
  return nlohmann::json{{"propensity", models.propensity.to_json()},
                        {"treated_outcome", models.treated_outcome.to_json()},
                        {"control_outcome", models.control_outcome.to_json()},
                        {"treated_effect", models.treated_effect.to_json()},
                        {"control_effect", models.control_effect.to_json()}};
}

nlohmann::json metrics(const UpliftResult &result,
                       const UpliftRunOptions &options) {
  return nlohmann::json{
      {"selected_learner",
       result.selected_learner == UpliftLearner::X ? 1.0 : 0.0},
      {"t_auuc", result.t_learner.auuc},
      {"t_qini_coefficient", result.t_learner.qini_coefficient},
      {"t_uplift_at_target", result.t_learner.uplift_at_target},
      {"x_auuc", result.x_learner.auuc},
      {"x_qini_coefficient", result.x_learner.qini_coefficient},
      {"x_uplift_at_target", result.x_learner.uplift_at_target},
      {"uplift_lower", result.uplift_lower},
      {"uplift_upper", result.uplift_upper},
      {"targeting_fraction", options.params.targeting_fraction},
      {"treated_rows", result.treated_rows},
      {"control_rows", result.control_rows},
      {"held_out_rows", result.held_out_rows},
      {"overlap_excluded", result.overlap_excluded},
      {"propensity_min", result.propensity.minimum},
      {"propensity_max", result.propensity.maximum},
      {"propensity_mean", result.propensity.mean},
      {"propensity_stddev", result.propensity.standard_deviation},
      {"observational", result.observational ? 1.0 : 0.0}};
}

void fail_run(const std::shared_ptr<pipeline::Run> &run,
              const std::string &error) {
  if (!run) {
    return;
  }
  run->add_log({util::now_iso8601(), "error", error});
  run->set_status(pipeline::RunStatus::Failed);
}

} // namespace

UpliftRunResult run_uplift_on_table(Node &node,
                                    const data::ResolvedDataset &dataset,
                                    const UpliftRunOptions &options) {
  UpliftRunResult output;
  output.run = node.create_run(options.project, options.graph);
  if (!output.run) {
    output.error = "could not create the uplift run";
    return output;
  }
  // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
  output.run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
  output.run->set_status(pipeline::RunStatus::Running);
  output.run->add_log(
      {util::now_iso8601(), "info", "training native T- and X-learners"});

  const auto fail = [&](std::string error) {
    output.error = std::move(error);
    output.uplift.status = compute::ComputeStatus::backend_error(output.error);
    fail_run(output.run, output.error);
    return output;
  };
  if (!dataset.ok()) {
    return fail(dataset.error);
  }
  if (options.treatment_column.empty() || options.outcome_column.empty()) {
    return fail("uplift needs treatment and outcome columns");
  }
  if (dataset.contract.entity_column.empty() ||
      dataset.contract.feature_columns.empty()) {
    return fail("uplift needs an entity column and pre-treatment features");
  }
  compute::JobCoordinator *coordinator = node.job_coordinator();
  if (coordinator == nullptr) {
    return fail("no compute plane attached to this node");
  }
  const std::vector<std::string> participants =
      coordinator->collectives().participants();
  if (options.coordinate && participants.size() > 1) {
    return fail("registry co-launch for an uplift table is not available");
  }

  compute::CpuDevice device;
  const ddf::DataFrameSchema schema = uplift_schema(dataset.contract, options);
  ddf::LocalShardResult local =
      ddf::load_local_partitions(node, dataset.table_id, schema, device);
  ddf::DataFrame frame;
  if (!local.ok()) {
    return fail(local.status.message);
  }
  if (local.rows_read == 0) {
    const std::shared_ptr<data::Table> table = source_table(node, dataset);
    if (!table) {
      return fail("the resolved uplift table is not available locally");
    }
    ddf::DataFrameResult loaded =
        ddf::load_data_frame(table->store(), schema, device);
    if (!loaded.ok()) {
      return fail(loaded.status.message);
    }
    frame = std::move(loaded.frame);
  } else {
    frame = std::move(local.frame);
  }

  const std::shared_ptr<compute::ComputeJob> job =
      node.create_job(compute::JobKind::Train, options.project, options.graph,
                      output.run->id());
  if (!job) {
    return fail("could not create the uplift training job");
  }
  const DistributedUpliftResult trained =
      run_uplift_job(*coordinator, *job, frame, dataset.contract.entity_column,
                     options.treatment_column, options.outcome_column,
                     dataset.contract.feature_columns, options.params,
                     "uplift.run." + output.run->id());
  if (!trained.ok()) {
    return fail(job->detail());
  }
  output.uplift = trained.uplift;

  const std::string suffix = output.run->id().substr(0, 8);
  const std::string effects_name = "effects_" + suffix;
  const std::string qini_name = "qini_" + suffix;
  std::shared_ptr<ml::Model> model;
  node.run_on_owner([&] {
    std::shared_ptr<data::Database> database =
        node.find_database("uplift_results");
    if (!database) {
      database = node.create_database("uplift_results");
    }
    if (!database) {
      return;
    }
    std::shared_ptr<data::Table> effects = database->create_table(effects_name);
    std::shared_ptr<data::Table> qini = database->create_table(qini_name);
    if (!effects || !qini) {
      return;
    }
    std::size_t rank = 0;
    for (const TreatmentEffect &effect : output.uplift.treatment_effects) {
      effects->add_row(
          {{"rank", ++rank},
           {dataset.contract.entity_column,
            data::entity_output(dataset.contract, effect.entity)},
           {"estimated_uplift", effect.estimated_uplift},
           {"propensity", effect.propensity},
           {"treatment", effect.treatment},
           {"outcome", effect.outcome},
           {"overlap_eligible", effect.overlap_eligible},
           {"recommended", effect.recommended},
           {"targeting_fraction", options.params.targeting_fraction}});
    }
    for (const QiniPoint &point : output.uplift.qini_curve) {
      qini->add_row({{"fraction", point.fraction},
                     {"incremental_gain", point.incremental_gain},
                     {"uplift", point.uplift}});
    }

    ModelValue value;
    value.name = options.model_name;
    value.kind = "uplift";
    value.project = options.project;
    value.run = output.run->id();
    value.feature_schema =
        nlohmann::json{{"entity", dataset.contract.entity_column},
                       {"treatment", options.treatment_column},
                       {"outcome", options.outcome_column},
                       {"features", dataset.contract.feature_columns}};
    if (dataset.contract.entity_keys) {
      value.feature_schema["entity_key_mapping"] =
          dataset.contract.entity_keys->to_json();
    }
    value.metrics = metrics(output.uplift, options);
    model = node.create_model(std::move(value));
    if (model) {
      model->set_weights(model_weights(output.uplift.models).dump());
    }
    output.treatment_effects_table = "uplift_results." + effects_name;
    output.qini_table = "uplift_results." + qini_name;
  });
  if (!model) {
    return fail("could not materialize uplift outputs");
  }
  output.model_id = model->id();

  for (const std::string &note : dataset.notes) {
    output.run->add_log({util::now_iso8601(), "info", "dataset: " + note});
  }
  if (dataset.contract.entity_keys) {
    output.run->set_entity_key_mapping(
        dataset.contract.entity_keys->to_json());
  }
  if (output.uplift.observational) {
    output.run->add_log({util::now_iso8601(), "warn",
                         "causal claims assume no unmeasured confounding"});
  }
  output.run->set_outputs(
      {{"model", options.model_name, output.model_id},
       {"dataset", "treatment effects", output.treatment_effects_table},
       {"dataset", "Qini curve", output.qini_table},
       {"report", "uplift report", output.run->id()}});
  output.run->set_lineage(
      {{"dataset:" + dataset.database + "." + dataset.table},
       {"resolve_dataset", "propensity", "T-learner", "X-learner",
        "held-out Qini", "targeting policy"},
       util::now_iso8601(),
       output.uplift.held_out_rows > 0 ? 1.0 : 0.0});
  output.run->add_log(
      {util::now_iso8601(), "info",
       "selected " + std::string(to_string(output.uplift.selected_learner)) +
           "; excluded " + std::to_string(output.uplift.overlap_excluded) +
           " profiles outside overlap"});
  output.run->set_status(pipeline::RunStatus::Succeeded);
  return output;
}

} // namespace zetta::ml
