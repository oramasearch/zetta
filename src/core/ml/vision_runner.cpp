// SPDX-License-Identifier: MIT

#include "zetta/ml/vision.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/vision_train_job.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

struct LoadedRows {
  std::vector<nlohmann::json> rows;
  std::string error;
};

std::shared_ptr<Model> find_backbone(Node &node, const std::string &reference) {
  if (const std::shared_ptr<Model> by_id = node.find_model(reference)) {
    return by_id;
  }
  for (const std::shared_ptr<Model> &model : node.models()) {
    if (model && model->name() == reference) {
      return model;
    }
  }
  return {};
}

bool permissive_checkpoint_license(const std::string &license) {
  return license == "MIT" || license == "Apache-2.0" ||
         license == "BSD-2-Clause" || license == "BSD-3-Clause";
}

std::optional<std::string> read_blob(Node &node, const std::string &reference) {
  for (const std::string &name : node.database_names()) {
    const std::shared_ptr<data::Database> database = node.find_database(name);
    if (!database) {
      continue;
    }
    if (std::optional<std::string> bytes =
            database->large_objects().read(reference)) {
      return bytes;
    }
  }
  return std::nullopt;
}

LoadedRows load_rows(Node &node, const data::ResolvedDataset &dataset,
                     const std::shared_ptr<data::Table> &table) {
  LoadedRows loaded;
  std::vector<std::shared_ptr<data::DatasetPartition>> owned;
  for (const std::shared_ptr<data::DatasetPartition> &partition :
       node.dataset_partitions()) {
    if (!partition || partition->table() != dataset.table_id) {
      continue;
    }
    const std::vector<std::string> &owners = partition->owner_nodes();
    if (std::find(owners.begin(), owners.end(), node.node_id()) !=
        owners.end()) {
      owned.push_back(partition);
    }
  }
  std::sort(owned.begin(), owned.end(),
            [](const std::shared_ptr<data::DatasetPartition> &left,
               const std::shared_ptr<data::DatasetPartition> &right) {
              return left->row_range().start < right->row_range().start;
            });
  if (!owned.empty()) {
    for (const std::shared_ptr<data::DatasetPartition> &partition : owned) {
      const std::optional<std::string> body =
          read_blob(node, partition->blob_ref());
      if (!body) {
        loaded.error = "vision dataset: owned partition '" + partition->id() +
                       "' is unavailable";
        return loaded;
      }
      data::PartitionBodyReadResult decoded =
          data::read_partition_body(*body, partition->id());
      if (!decoded.ok()) {
        loaded.error = "vision dataset: " + decoded.error;
        return loaded;
      }
      for (data::PartitionRow &row : decoded.rows) {
        loaded.rows.push_back(std::move(row.doc));
      }
    }
    return loaded;
  }
  const storage::StorageEngine &store = table->store();
  loaded.rows.reserve(store.size());
  for (std::size_t index = 0; index < store.size(); ++index) {
    const storage::ParquetLazyPtr row = store.at(index);
    if (!row) {
      loaded.error = "vision dataset: could not read row " +
                     std::to_string(index) + ": " + row.error();
      return loaded;
    }
    loaded.rows.push_back(*row);
  }
  return loaded;
}

std::optional<std::size_t> read_label(const nlohmann::json &row,
                                      const std::string &column) {
  const auto found = row.find(column);
  if (found == row.end() || !found->is_number_integer()) {
    return std::nullopt;
  }
  const std::int64_t value = found->get<std::int64_t>();
  if (value < 0) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

std::string prediction_table_name(std::string run_id) {
  std::replace(run_id.begin(), run_id.end(), '-', '_');
  return "predictions_" + run_id;
}

void fail_run(const std::shared_ptr<pipeline::Run> &run,
              const std::string &reason) {
  if (!run) {
    return;
  }
  run->add_log({util::now_iso8601(), "error", reason});
  run->set_status(pipeline::RunStatus::Failed);
}

std::vector<nlohmann::json> confusion_rows(const VisionMetrics &metrics,
                                           std::size_t classes) {
  std::vector<nlohmann::json> rows;
  for (std::size_t actual = 0; actual < classes; ++actual) {
    nlohmann::json row = nlohmann::json::array();
    for (std::size_t predicted = 0; predicted < classes; ++predicted) {
      row.push_back(metrics.confusion_matrix[actual * classes + predicted]);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

} // namespace

VisionRunResult run_vision_on_table(Node &node,
                                    const data::ResolvedDataset &dataset,
                                    const VisionRunParams &params,
                                    const VisionReadAuthorizer &authorizer) {
  VisionRunResult result;
  const std::shared_ptr<pipeline::Run> run =
      node.create_run(params.project, params.graph);
  if (!run) {
    result.error = "vision run: could not create run record";
    return result;
  }
  // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
  run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
  result.run_id = run->id();
  run->set_status(pipeline::RunStatus::Running);
  run->add_log({util::now_iso8601(), "info", "vision run started"});
  const auto fail = [&](std::string reason) {
    result.error = std::move(reason);
    fail_run(run, result.error);
    return result;
  };

  if (!dataset.ok()) {
    return fail("vision dataset: " + dataset.error);
  }
  // The trainer below is SPMD and allreduces partition-local statistics. The
  // registry callback does not yet co-launch the runner on every owner, so it
  // must refuse a partial launch instead of entering collectives alone.
  if (node.cluster_members().size() > 1) {
    return fail("vision run: registry execution requires one member; the "
                "head trainer supports coordinated multi-member launches");
  }
  compute::JobCoordinator *coordinator = node.job_coordinator();
  if (coordinator == nullptr) {
    return fail("vision run: the node has no compute coordinator");
  }
  const std::shared_ptr<data::Database> database =
      node.find_database(dataset.database);
  const std::shared_ptr<data::Table> table =
      database ? database->find_table(dataset.table) : nullptr;
  if (!database || !table || table->id() != dataset.table_id) {
    return fail("vision dataset: resolved table is no longer available");
  }
  const std::shared_ptr<Model> backbone_model =
      find_backbone(node, params.backbone_model);
  if (!backbone_model || backbone_model->kind() != "vision_backbone") {
    return fail("vision backbone: expected a registered vision_backbone model");
  }
  const nlohmann::json &schema = backbone_model->feature_schema();
  const auto config_value = schema.find("vit_config");
  if (config_value == schema.end()) {
    return fail("vision backbone: feature_schema.vit_config is required");
  }
  const std::string origin = schema.value("weights_origin", "pretrained");
  if (origin != "random") {
    const std::string revision = schema.value("checkpoint_revision", "");
    const std::string weights_license = schema.value("weights_license", "");
    const std::string config_license = schema.value("config_license", "");
    if (revision.empty() || !permissive_checkpoint_license(weights_license) ||
        !permissive_checkpoint_license(config_license)) {
      return fail("vision backbone: pretrained weights require a pinned "
                  "checkpoint_revision and permissive weights_license and "
                  "config_license metadata");
    }
  }
  std::string config_error;
  const std::optional<VitConfig> config =
      vit_config_from_json(*config_value, &config_error);
  if (!config) {
    return fail(config_error);
  }
  const std::optional<std::string> weights = backbone_model->read_weights();
  if (!weights) {
    return fail("vision backbone: weights are not present on this member");
  }
  std::string backbone_error;
  const std::optional<VitBackbone> backbone =
      load_vit_backbone(*config, *weights, &backbone_error);
  if (!backbone) {
    return fail(backbone_error);
  }
  LoadedRows loaded = load_rows(node, dataset, table);
  if (!loaded.error.empty()) {
    return fail(std::move(loaded.error));
  }

  std::vector<VisionExample> examples;
  examples.reserve(loaded.rows.size());
  std::size_t classes = 0;
  std::set<std::size_t> labels;
  for (std::size_t index = 0; index < loaded.rows.size(); ++index) {
    const nlohmann::json &row = loaded.rows[index];
    const auto image = row.find(params.image_column);
    const auto source = row.find(params.source_column);
    const std::optional<std::size_t> label =
        read_label(row, params.label_column);
    if (image == row.end() || !image->is_string() || source == row.end() ||
        !source->is_string() || !label) {
      return fail("vision dataset: row " + std::to_string(index) +
                  " must contain string image/source and a non-negative "
                  "integer label");
    }
    const std::string image_ref = image->get<std::string>();
    std::string refusal;
    if (authorizer && !authorizer(image_ref, refusal)) {
      return fail("vision image '" + image_ref + "' was refused" +
                  (refusal.empty() ? std::string{} : ": " + refusal));
    }
    const std::optional<std::string> bytes =
        database->large_objects().read(image_ref);
    if (!bytes) {
      ++result.images_excluded;
      run->add_log({util::now_iso8601(), "warn",
                    "excluded unavailable image '" + image_ref + "'"});
      continue;
    }
    ++result.images_read;
    ImageDecodeResult decoded = decode_image(*bytes);
    if (!decoded.ok()) {
      ++result.images_excluded;
      run->add_log({util::now_iso8601(), "warn",
                    "excluded image '" + image_ref + "': " + decoded.error});
      continue;
    }
    const std::vector<float> normalized = resize_normalize(
        decoded.image, config->image_width, config->image_height,
        {0.5F, 0.5F, 0.5F}, {0.5F, 0.5F, 0.5F});
    std::string embed_error;
    std::vector<float> embedding =
        vit_embed(*backbone, normalized, &embed_error);
    if (!embed_error.empty()) {
      return fail(embed_error);
    }
    classes = std::max(classes, *label + 1);
    labels.insert(*label);
    examples.push_back({image_ref, source->get<std::string>(), *label,
                        std::move(embedding), false});
  }
  if (examples.empty()) {
    return fail("vision dataset: no decodable, authorized images remained");
  }
  std::string split_error;
  if (!assign_source_disjoint_split(examples, params.holdout_fraction,
                                    params.seed, &split_error)) {
    return fail(split_error);
  }
  if (labels.size() < 2) {
    return fail("vision dataset: at least two distinct labels are required");
  }

  const std::shared_ptr<compute::ComputeJob> job = node.create_job(
      compute::JobKind::Train, params.project, params.graph, run->id());
  if (!job) {
    return fail("vision run: could not create train job");
  }
  VisionTrainParams train_params;
  train_params.classes = classes;
  train_params.epochs = params.epochs;
  train_params.learning_rate = params.learning_rate;
  train_params.seed = params.seed;
  VisionTrainResult trained = run_vision_train_job(
      *coordinator, *job, examples, train_params, "ml.vision." + run->id());
  if (!trained.ok()) {
    return fail(trained.error);
  }
  result.metrics = trained.metrics;

  nlohmann::json feature_schema = {
      {"architecture", "vit_linear_transfer"},
      {"backbone_model", backbone_model->id()},
      {"vit_config", to_json(*config)},
      {"image_column", params.image_column},
      {"label_column", params.label_column},
      {"source_column", params.source_column},
      {"source_disjoint", true},
      {"confusion_matrix", confusion_rows(trained.metrics, classes)}};
  nlohmann::json metrics = {{"accuracy", trained.metrics.accuracy},
                            {"macro_f1", trained.metrics.macro_f1},
                            {"train_rows", trained.metrics.train_rows},
                            {"held_out_rows", trained.metrics.held_out_rows},
                            {"images_read", result.images_read},
                            {"images_excluded", result.images_excluded}};
  ModelValue model_value;
  model_value.name = params.model_name;
  model_value.kind = "vision";
  model_value.project = params.project;
  model_value.run = run->id();
  model_value.feature_schema = std::move(feature_schema);
  model_value.metrics = metrics;
  const std::shared_ptr<Model> model =
      node.create_model(std::move(model_value));
  if (!model) {
    return fail("vision run: could not register the trained model");
  }
  zetta::autograd::SafeTensors head_tensors;
  head_tensors["head.weight"] = {
      {static_cast<int>(classes),
       static_cast<int>(trained.head.embedding_size)},
      std::vector<float>(trained.head.weights.begin(),
                         trained.head.weights.end())};
  head_tensors["head.bias"] = {
      {static_cast<int>(classes)},
      std::vector<float>(trained.head.bias.begin(), trained.head.bias.end())};
  if (!model->set_weights(
          zetta::autograd::serialize_safetensors(head_tensors))) {
    return fail("vision run: could not persist trained head weights");
  }
  result.model_id = model->id();

  std::shared_ptr<data::Database> output_database =
      node.find_database("vision_results");
  if (!output_database) {
    output_database = node.create_database("vision_results");
  }
  if (!output_database) {
    return fail("vision run: could not create the output database");
  }
  const std::string output_name = prediction_table_name(run->id());
  const std::shared_ptr<data::Table> output_table =
      output_database->create_table(output_name);
  if (!output_table) {
    return fail("vision run: could not create the prediction table");
  }
  for (const VisionExample &example : examples) {
    const std::vector<double> probabilities =
        trained.head.probabilities(example.embedding);
    const auto best =
        std::max_element(probabilities.begin(), probabilities.end());
    const std::size_t predicted =
        static_cast<std::size_t>(std::distance(probabilities.begin(), best));
    output_table->add_row({{"image", example.image_ref},
                           {"label", example.label},
                           {"predicted_label", predicted},
                           {"score", *best},
                           {"source", example.source},
                           {"split", example.held_out ? "test" : "train"}});
  }
  result.prediction_table = "vision_results." + output_name;

  run->set_outputs({{"model", params.model_name, model->id()},
                    {"dataset", "image predictions", result.prediction_table},
                    {"report", "vision evaluation", run->id()}});
  run->set_lineage({{"dataset:" + dataset.database + "." + dataset.table,
                     "model:" + backbone_model->id()},
                    {"decode", "resize_normalize", "vit_embed", "source_split",
                     "linear_head", "predict"},
                    util::now_iso8601(),
                    trained.metrics.accuracy});
  run->add_log({util::now_iso8601(), "info",
                "read " + std::to_string(result.images_read) +
                    " images; excluded " +
                    std::to_string(result.images_excluded)});
  run->set_status(pipeline::RunStatus::Succeeded);
  return result;
}

} // namespace zetta::ml
