#include "zetta/ml/label_efficiency_runner.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/cpu_device.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kResultsDatabase = "label_efficiency_results";

ddf::DataFrameSchema runner_schema(const data::DatasetContract& contract) {
    ddf::DataFrameSchema schema;
    schema.columns.push_back(
        {.name = "entity",
         .field = contract.entity_column,
         .dtype = compute::DataType::Int64,
         .entity_keys = contract.entity_keys});
    for (const std::string& feature : contract.feature_columns) {
        schema.columns.push_back(
            {.name = feature,
             .field = feature,
             .dtype = compute::DataType::Float64});
    }
    schema.columns.push_back(
        {.name = "gold_label",
         .field = contract.label_column,
         .dtype = compute::DataType::Int64});
    return schema;
}

LabelEfficiencyRunResult fail_run(
    Node& node, std::shared_ptr<pipeline::Run> run, std::string reason) {
    node.run_on_owner([&] {
        if (run) {
            run->add_log(
                {util::now_iso8601(), "error", "label efficiency: " + reason});
            run->set_status(pipeline::RunStatus::Failed);
        }
    });
    LabelEfficiencyRunResult result;
    result.run = std::move(run);
    result.error = std::move(reason);
    return result;
}

nlohmann::json metrics_json(const LabelEfficiencyMetrics& metrics) {
    return {
        {"active_accuracy_at_budget", metrics.active_accuracy_at_budget},
        {"random_accuracy_at_budget", metrics.random_accuracy_at_budget},
        {"accuracy_gain_at_budget", metrics.accuracy_gain_at_budget},
        {"labels_saved", metrics.labels_saved},
        {"label_model_gold_accuracy", metrics.label_model_gold_accuracy},
        {"labelling_budget", metrics.labelling_budget},
        {"gold_rows", metrics.gold_rows},
        {"unlabelled_rows", metrics.unlabelled_rows},
    };
}

struct LoadedRows {
    std::vector<LabelEfficiencyRow> rows;
    std::string error;
};

LoadedRows load_rows(Node& node, const data::ResolvedDataset& dataset) {
    compute::CpuDevice device;
    ddf::LocalShardResult shard;
    node.run_on_owner([&] {
        shard = ddf::load_local_partitions(
            node, dataset.table_id, runner_schema(dataset.contract), device);
    });
    if (!shard.ok()) {
        return {{}, "loading owned partitions failed: " +
                        shard.status.message};
    }

    std::vector<std::int64_t> entities;
    std::vector<std::int64_t> labels;
    if (!shard.frame.to_host<std::int64_t>("entity", entities).ok() ||
        !shard.frame.to_host<std::int64_t>("gold_label", labels).ok()) {
        return {{},
                "could not read entity and label columns from the shard"};
    }
    std::vector<std::vector<double>> features(
        dataset.contract.feature_columns.size());
    for (std::size_t index = 0;
         index < dataset.contract.feature_columns.size(); ++index) {
        if (!shard.frame
                 .to_host<double>(dataset.contract.feature_columns[index],
                                  features[index])
                 .ok()) {
            return {{}, "could not read feature column '" +
                            dataset.contract.feature_columns[index] + "'"};
        }
    }

    LoadedRows loaded;
    loaded.rows.reserve(entities.size());
    for (std::size_t row = 0; row < entities.size(); ++row) {
        LabelEfficiencyRow input;
        input.entity = entities[row];
        input.gold_label = static_cast<int>(labels[row]);
        input.features.reserve(features.size());
        for (const std::vector<double>& column : features) {
            input.features.push_back(column[row]);
        }
        loaded.rows.push_back(std::move(input));
    }
    return loaded;
}

void materialize_outputs(
    Node& node, const data::ResolvedDataset& dataset,
    const LabelEfficiencyRunOptions& options,
    const LabelEfficiencyResult& trained,
    const std::shared_ptr<pipeline::Run>& run,
    LabelEfficiencyRunResult& result) {
    std::shared_ptr<data::Database> database =
        node.find_database(std::string(kResultsDatabase));
    if (!database) {
        database = node.create_database(std::string(kResultsDatabase));
    }
    if (!database) {
        result.error = "could not create the output database";
        return;
    }
    const std::string suffix = run->id().substr(0, 8);
    const std::string queue_name = "label_queue_" + suffix;
    const std::string weak_name = "weak_labels_" + suffix;
    const std::shared_ptr<data::Table> queue =
        database->create_table(queue_name);
    const std::shared_ptr<data::Table> weak =
        database->create_table(weak_name);
    if (!queue || !weak) {
        result.error = "could not create the output tables";
        return;
    }
    for (const LabelQueueEntry& entry : trained.label_queue) {
        queue->add_row({{"rank", entry.rank},
                        {"row", data::entity_output(
                                    dataset.contract, entry.entity)},
                        {"uncertainty", entry.uncertainty},
                        {"diversity", entry.diversity},
                        {"score", entry.score}});
    }
    for (const WeakLabel& label : trained.local_weak_labels) {
        weak->add_row({{"row", data::entity_output(
                                   dataset.contract, label.entity)},
                       {"probabilistic_label", label.probability},
                       {"contributing_functions",
                        label.contributing_functions}});
    }
    result.label_queue_table =
        std::string(kResultsDatabase) + "." + queue_name;
    result.weak_label_table =
        std::string(kResultsDatabase) + "." + weak_name;

    nlohmann::json functions = nlohmann::json::array();
    for (const LabellingFunctionAccuracy& accuracy :
         trained.labelling_function_accuracies) {
        functions.push_back(
            {{"name", accuracy.name}, {"accuracy", accuracy.estimated}});
    }
    ModelValue model_value;
    model_value.name = options.model_name;
    model_value.kind = "label_efficiency";
    model_value.project = options.project;
    model_value.run = run->id();
    model_value.feature_schema = {
        {"features", dataset.contract.feature_columns},
        {"labelling_functions", std::move(functions)},
        {"gold_split", "disjoint"},
        {"weak_labels_are_gold", false},
    };
    if (dataset.contract.entity_keys) {
        model_value.feature_schema["entity_key_mapping"] =
            dataset.contract.entity_keys->to_json();
        run->set_entity_key_mapping(dataset.contract.entity_keys->to_json());
    }
    model_value.metrics = metrics_json(trained.metrics);
    const std::shared_ptr<Model> model =
        node.create_model(std::move(model_value));
    if (!model) {
        result.error = "could not register the label model";
        return;
    }
    result.model_id = model->id();
    run->set_outputs(
        {{"model", "Label-efficiency model", model->id()},
         {"dataset", "Label queue", result.label_queue_table},
         {"dataset", "Weak labels", result.weak_label_table},
         {"report", "Label-efficiency report", run->id()}});
    run->set_lineage(
        {{"dataset:" + dataset.database + "." + dataset.table},
         {"load_dataset", "active_learning", "weak_supervision",
          "write_label_tables"},
         util::now_iso8601(),
         trained.metrics.label_model_gold_accuracy});
    run->add_log(
        {util::now_iso8601(), "info",
         "label efficiency: excluded " +
             std::to_string(trained.metrics.gold_rows) +
             " gold rows from both output tables"});
    run->set_status(pipeline::RunStatus::Succeeded);
}

} // namespace

LabelEfficiencyRunResult run_label_efficiency_on_dataset(
    Node& node, const data::ResolvedDataset& dataset,
    const LabelEfficiencyRunOptions& options) {
    std::shared_ptr<pipeline::Run> run;
    node.run_on_owner([&] {
        run = node.create_run(options.project, options.graph);
        if (run) {
            // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
            run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
            run->set_status(pipeline::RunStatus::Running);
            run->add_log({util::now_iso8601(), "info",
                          "label efficiency: reading dataset " +
                              dataset.database + "." + dataset.table});
            for (const std::string& note : dataset.notes) {
                run->add_log({util::now_iso8601(), "info",
                              "dataset: " + note});
            }
        }
    });
    if (!run) {
        return fail_run(node, {}, "could not create the Run record");
    }
    if (!dataset.ok()) {
        return fail_run(node, run, dataset.error);
    }
    if (dataset.contract.entity_column.empty() ||
        dataset.contract.label_column.empty() ||
        dataset.contract.feature_columns.empty()) {
        return fail_run(
            node, run,
            "the resolved dataset needs entity, label, and feature columns");
    }
    if (options.params.labelling_functions.empty()) {
        return fail_run(node, run,
                        "at least one labelling function is required");
    }

    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        return fail_run(node, run,
                        "no compute plane is attached to this node");
    }
    const std::vector<std::string> participants =
        coordinator->collectives().participants();
    if (participants.size() != 1) {
        // The trainer itself is SPMD and combines all statistics. Publishing one
        // logical output table still needs a coordinated table writer; refusing
        // avoids presenting the initiator's local weak-label shard as global.
        return fail_run(
            node, run,
            "table output is currently supported only on a single-member "
            "cluster; the distributed trainer must be co-launched with a "
            "coordinated table writer");
    }

    LoadedRows loaded = load_rows(node, dataset);
    if (!loaded.error.empty()) {
        return fail_run(node, run, loaded.error);
    }

    std::shared_ptr<compute::ComputeJob> job;
    node.run_on_owner([&] {
        job = node.create_job(compute::JobKind::Train, options.project,
                              options.graph, run->id());
    });
    if (!job) {
        return fail_run(node, run, "could not create the training job");
    }
    LabelEfficiencyResult trained = run_label_efficiency_job(
        *coordinator, *job, loaded.rows, dataset.contract.feature_columns,
        options.params, "ml.label_efficiency." + run->id());
    if (!trained.ok()) {
        return fail_run(node, run, trained.error);
    }

    LabelEfficiencyRunResult result;
    result.run = run;
    node.run_on_owner([&] {
        materialize_outputs(node, dataset, options, trained, run, result);
    });
    if (!result.error.empty()) {
        return fail_run(node, run, result.error);
    }
    return result;
}

std::shared_ptr<AsyncLabelEfficiencyRun> AsyncLabelEfficiencyRun::start(
    Node& node, data::ResolvedDataset dataset,
    LabelEfficiencyRunOptions options) {
    return std::shared_ptr<AsyncLabelEfficiencyRun>(
        new AsyncLabelEfficiencyRun(node, std::move(dataset),
                                    std::move(options)));
}

AsyncLabelEfficiencyRun::AsyncLabelEfficiencyRun(
    Node& node, data::ResolvedDataset dataset,
    LabelEfficiencyRunOptions options)
    : node_(node),
      dataset_(std::move(dataset)),
      options_(std::move(options)) {
    worker_ = std::thread([this] {
        result_ =
            run_label_efficiency_on_dataset(node_, dataset_, options_);
        done_.store(true, std::memory_order_release);
    });
}

AsyncLabelEfficiencyRun::~AsyncLabelEfficiencyRun() {
    while (!done_.load(std::memory_order_acquire)) {
        node_.drain_owner_work();
        std::this_thread::yield();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace zetta::ml
