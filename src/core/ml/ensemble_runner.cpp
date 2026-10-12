#include "zetta/ml/ensemble_runner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/ensemble_train_job.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kResultsDatabase = "ensemble_results";
constexpr std::string_view kEntityColumn = "entity";

std::string normalized_table_suffix(const std::string& graph) {
    std::string suffix = graph.empty() ? "pipeline" : graph.substr(0, 8);
    for (char& character : suffix) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (!std::isalnum(value)) {
            character = '_';
        }
    }
    return suffix;
}

ddf::DataFrameSchema ensemble_schema(const data::DatasetContract& contract) {
    using compute::DataType;
    using ddf::ColumnSchema;
    ddf::DataFrameSchema schema;
    schema.columns.push_back(
        ColumnSchema{.name = std::string(kEntityColumn),
                     .field = contract.entity_column,
                     .dtype = DataType::Int64,
                     .entity_keys = contract.entity_keys});
    for (const std::string& feature : contract.feature_columns) {
        schema.columns.push_back(ColumnSchema{.name = feature,
                                              .field = feature,
                                              .dtype = DataType::Float64});
    }
    schema.columns.push_back(ColumnSchema{.name = contract.label_column,
                                          .field = contract.label_column,
                                          .dtype = DataType::Float64});
    if (!contract.time_column.empty()) {
        schema.columns.push_back(ColumnSchema{.name = contract.time_column,
                                              .field = contract.time_column,
                                              .dtype = DataType::Float64,
                                              .timestamp = true});
    }
    return schema;
}

nlohmann::json model_metrics(const EnsembleTrainResult& trained) {
    return nlohmann::json{
        {"held_out_auc", trained.held_out_metrics.auc},
        {"held_out_log_loss", trained.held_out_metrics.log_loss},
        {"held_out_accuracy", trained.held_out_metrics.accuracy},
        {"held_out_rows", trained.held_out_rows},
        {"oof_auc", trained.oof_metrics.auc},
        {"oof_log_loss", trained.oof_metrics.log_loss},
        {"oof_accuracy", trained.oof_metrics.accuracy},
        {"oof_rows", trained.oof_rows},
        {"train_rows", trained.train_rows},
        {"best_single_held_out_auc", trained.best_single_held_out_auc},
        {"held_out_auc_lift_over_best_single",
         trained.held_out_auc_lift_over_best_single},
        {"meta_model_intercept", trained.model.stacker.intercept}};
}

nlohmann::json model_feature_schema(const EnsembleTrainResult& trained) {
    nlohmann::json drivers = nlohmann::json::array();
    nlohmann::json bases = nlohmann::json::array();
    for (std::size_t index = 0; index < trained.model.bases.size(); ++index) {
        const std::string& name = trained.model.bases[index].name;
        const double weight = trained.model.stacker.weights[index];
        drivers.push_back({{"feature", name}, {"importance", weight}});
        bases.push_back(name);
    }
    return nlohmann::json{
        {"objective", "out_of_fold_stacking"},
        {"features", trained.model.feature_names},
        {"base_models", std::move(bases)},
        {"meta_model", "regularized_logistic_regression"},
        {"meta_intercept", trained.model.stacker.intercept},
        {"drivers", std::move(drivers)}};
}

struct WrittenTables {
    std::string scores;
    std::string weights;
};

WrittenTables write_result_tables(Node& node, const std::string& graph,
                                  const EnsembleTrainResult& trained,
                                  const data::DatasetContract& contract) {
    WrittenTables written;
    const std::string suffix = normalized_table_suffix(graph);
    std::shared_ptr<data::Database> database =
        node.find_database(std::string(kResultsDatabase));
    if (!database) {
        database = node.create_database(std::string(kResultsDatabase));
    }
    if (!database) {
        return written;
    }

    const std::string scores_name = "blended_scores_" + suffix;
    database->drop_table(scores_name);
    std::shared_ptr<data::Table> scores = database->create_table(scores_name);
    if (scores) {
        std::vector<EnsembleScore> ranked = trained.scores;
        std::sort(ranked.begin(), ranked.end(),
                  [](const EnsembleScore& left, const EnsembleScore& right) {
                      if (left.blended_score != right.blended_score) {
                          return left.blended_score > right.blended_score;
                      }
                      return left.entity < right.entity;
                  });
        for (std::size_t index = 0; index < ranked.size(); ++index) {
            const EnsembleScore& score = ranked[index];
            nlohmann::json row = {{"rank", index + 1},
                                  {"account", data::entity_output(
                                                  contract, score.entity)},
                                  {"risk", score.blended_score},
                                  {"label", score.label},
                                  {"held_out", score.held_out}};
            for (std::size_t base = 0; base < score.base_scores.size(); ++base) {
                row["base_" + std::to_string(base) + "_score"] =
                    score.base_scores[base];
            }
            scores->add_row(std::move(row));
        }
        written.scores = std::string(kResultsDatabase) + "." + scores_name;
    }

    const std::string weights_name = "meta_weights_" + suffix;
    database->drop_table(weights_name);
    std::shared_ptr<data::Table> weights = database->create_table(weights_name);
    if (weights) {
        for (std::size_t index = 0; index < trained.model.bases.size(); ++index) {
            weights->add_row(
                {{"base_model", trained.model.bases[index].name},
                 {"weight", trained.model.stacker.weights[index]},
                 {"intercept", trained.model.stacker.intercept}});
        }
        written.weights = std::string(kResultsDatabase) + "." + weights_name;
    }
    return written;
}

class EnsembleStepExecutor final : public pipeline::StepExecutor {
public:
    EnsembleStepExecutor(Node& node, compute::ComputeDevice& device,
                         data::ResolvedDataset dataset,
                         EnsembleRunOptions options)
        : node_(node),
          device_(device),
          dataset_(std::move(dataset)),
          options_(std::move(options)) {}

    pipeline::StepOutcome run_step(const ir::Node& step) const override {
        switch (step.kind) {
            case ir::StepKind::Connect:
                return run_connect(step);
            case ir::StepKind::Clean:
                return run_clean(step);
            case ir::StepKind::Model:
                return run_model(step);
            case ir::StepKind::Serve:
                return run_serve(step);
            case ir::StepKind::Index:
            case ir::StepKind::Act:
                return pipeline::StepOutcome{
                    false, "M-18 received a step outside its pipeline shape", {}, {}};
        }
        return pipeline::StepOutcome{false, "M-18 received an unknown step", {}, {}};
    }

    const std::string& model_id() const {
        return model_id_;
    }

    const WrittenTables& tables() const {
        return tables_;
    }

private:
    pipeline::StepOutcome run_connect(const ir::Node& step) const {
        const std::string name = dataset_.database + "." + dataset_.table;
        pipeline::StepOutcome outcome;
        outcome.sources.push_back("dataset:" + name);
        outcome.outputs.push_back(
            pipeline::RunOutput{"dataset", name, "connect:" + step.id});
        outcome.notes.push_back("dataset: trained on " + name);
        for (const std::string& note : dataset_.notes) {
            outcome.notes.push_back("dataset: " + note);
        }
        return outcome;
    }

    pipeline::StepOutcome run_clean(const ir::Node& step) const {
        if (!load_frame()) {
            return pipeline::StepOutcome{false, error_, {}, {}};
        }
        pipeline::StepOutcome outcome;
        outcome.outputs.push_back(pipeline::RunOutput{
            "dataset", "validated numeric ensemble inputs", "features:" + step.id});
        return outcome;
    }

    pipeline::StepOutcome run_model(const ir::Node& step) const {
        if (!load_frame()) {
            return pipeline::StepOutcome{false, error_, {}, {}};
        }
        compute::JobCoordinator* coordinator = node_.job_coordinator();
        if (coordinator == nullptr) {
            return pipeline::StepOutcome{
                false, "M-18 needs an attached compute plane", {}, {}};
        }
        // The native trainer is SPMD, but the registry adapter does not yet fan
        // a table launch to peers. Refuse instead of reporting one member's shard
        // as the whole dataset.
        if (coordinator->collectives().participants().size() > 1) {
            return pipeline::StepOutcome{
                false,
                "M-18 table launch across multiple members is not wired; run on "
                "a single-member cluster or invoke run_ensemble_train_job on all members",
                {}, {}};
        }

        std::shared_ptr<compute::ComputeJob> job;
        node_.run_on_owner([&] {
            job = node_.create_job(compute::JobKind::Train, options_.project,
                                   options_.graph);
        });
        if (!job) {
            return pipeline::StepOutcome{
                false, "M-18 could not create its training job", {}, {}};
        }
        const std::optional<double> as_of =
            dataset_.contract.time_column.empty()
                ? std::nullopt
                : std::optional<double>{dataset_.contract.as_of};
        trained_ = run_ensemble_train_job(
            *coordinator, *job, frame_, dataset_.contract.feature_columns,
            dataset_.contract.label_column, std::string(kEntityColumn),
            dataset_.contract.time_column, options_.params, as_of);
        if (!trained_.ok()) {
            return pipeline::StepOutcome{
                false, "M-18 training failed: " + job->detail(), {}, {}};
        }

        ModelValue value;
        value.name = options_.model_name;
        value.kind = "ensemble";
        value.project = options_.project;
        value.feature_schema = model_feature_schema(trained_.training);
        if (dataset_.contract.entity_keys) {
            value.feature_schema["entity_key_mapping"] =
                dataset_.contract.entity_keys->to_json();
        }
        value.metrics = model_metrics(trained_.training);
        std::shared_ptr<Model> model;
        node_.run_on_owner([&] {
            model = node_.create_model(std::move(value));
            if (model) {
                model->set_weights(trained_.training.model.to_json().dump());
            }
        });
        if (!model || !model->has_weights_body()) {
            return pipeline::StepOutcome{
                false, "M-18 could not persist its model", {}, {}};
        }
        model_id_ = model->id();
        ready_ = true;

        pipeline::StepOutcome outcome;
        outcome.outputs.push_back(
            pipeline::RunOutput{"model", step.name, model_id_});
        outcome.notes.push_back(
            "ensemble: stack holdout AUC lift over best base=" +
            std::to_string(
                trained_.training.held_out_auc_lift_over_best_single));
        return outcome;
    }

    pipeline::StepOutcome run_serve(const ir::Node&) const {
        if (!ready_) {
            return pipeline::StepOutcome{
                false, "M-18 output step ran before training", {}, {}};
        }
        node_.run_on_owner([&] {
            tables_ =
                write_result_tables(node_, options_.graph, trained_.training,
                                    dataset_.contract);
        });
        if (tables_.scores.empty() || tables_.weights.empty()) {
            return pipeline::StepOutcome{
                false, "M-18 could not persist its result tables", {}, {}};
        }
        pipeline::StepOutcome outcome;
        outcome.outputs.push_back(pipeline::RunOutput{
            "dataset", "blended scores", tables_.scores});
        outcome.outputs.push_back(pipeline::RunOutput{
            "dataset", "meta-model weights", tables_.weights});
        outcome.outputs.push_back(
            pipeline::RunOutput{"report", "run report", ""});
        return outcome;
    }

    bool load_frame() const {
        if (loaded_) {
            return error_.empty();
        }
        loaded_ = true;
        ddf::LocalShardResult shard;
        node_.run_on_owner([&] {
            shard = ddf::load_local_partitions(
                node_, dataset_.table_id, ensemble_schema(dataset_.contract),
                device_);
        });
        if (!shard.ok()) {
            error_ = "M-18 could not load owned dataset partitions: " +
                     shard.status.message;
            return false;
        }
        frame_ = std::move(shard.frame);
        if (frame_.row_count() == 0) {
            error_ = "M-18 found no rows in this member's owned partitions";
            return false;
        }
        return true;
    }

    Node& node_;
    compute::ComputeDevice& device_;
    data::ResolvedDataset dataset_;
    EnsembleRunOptions options_;
    mutable bool loaded_ = false;
    mutable bool ready_ = false;
    mutable std::string error_;
    mutable ddf::DataFrame frame_;
    mutable DistributedEnsembleResult trained_;
    mutable std::string model_id_;
    mutable WrittenTables tables_;
};

}  // namespace

EnsembleRunResult run_ensemble_on_dataset(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const data::ResolvedDataset& dataset,
    const EnsembleRunOptions& options) {
    auto steps = std::make_shared<EnsembleStepExecutor>(
        node, device, dataset, options);
    pipeline::GraphExecutor executor(steps);
    EnsembleRunResult result;
    result.execution =
        executor.execute(node, graph, options.project, options.graph);
    if (result.execution.run && dataset.contract.entity_keys) {
        result.execution.run->set_entity_key_mapping(
            dataset.contract.entity_keys->to_json());
    }
    result.model_id = steps->model_id();
    result.scores_table = steps->tables().scores;
    result.weights_table = steps->tables().weights;
    return result;
}

}  // namespace zetta::ml
