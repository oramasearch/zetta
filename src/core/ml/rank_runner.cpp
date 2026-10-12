#include "zetta/ml/rank.hpp"

#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

#include "feature_columns.hpp"

namespace zetta::ml {
namespace {

constexpr const char* kRankResultsDatabase = "rank_results";

ddf::DataFrameSchema rank_schema(const data::ResolvedDataset& dataset, const RankParams& params) {
    using compute::DataType;
    ddf::DataFrameSchema schema;
    schema.columns.push_back({.name = dataset.contract.entity_column,
                              .dtype = DataType::Int64,
                              .entity_keys = dataset.contract.entity_keys});
    schema.columns.push_back({.name = params.group_column, .dtype = DataType::Int64});
    schema.columns.push_back({.name = dataset.contract.label_column, .dtype = DataType::Float64});
    for (const std::string& feature : dataset.contract.feature_columns) {
        if (feature == params.group_column) {
            continue;
        }
        schema.columns.push_back({.name = feature, .dtype = DataType::Float64});
    }
    return schema;
}

compute::ComputeStatus read_integer_column(const ddf::DataFrame& frame, const std::string& column,
                                           std::vector<std::int64_t>& output) {
    std::vector<double> values;
    const compute::ComputeStatus status = detail::read_column_as_double(frame, column, values);
    if (!status.ok()) {
        return status;
    }
    output.reserve(values.size());
    for (const double value : values) {
        if (!std::isfinite(value) || std::trunc(value) != value) {
            return compute::ComputeStatus::invalid_argument("rank: column '" + column +
                                                            "' must contain integer values");
        }
        output.push_back(static_cast<std::int64_t>(value));
    }
    return compute::ComputeStatus::success();
}

compute::ComputeStatus frame_to_rank_dataset(const ddf::DataFrame& frame,
                                             const data::ResolvedDataset& resolved,
                                             const RankParams& params, RankDataset& dataset) {
    for (const std::string& feature : resolved.contract.feature_columns) {
        if (feature != params.group_column) {
            dataset.feature_names.push_back(feature);
        }
    }
    dataset.features.resize(dataset.feature_names.size());
    dataset.label_name = resolved.contract.label_column;
    dataset.entity_name = resolved.contract.entity_column;
    for (std::size_t index = 0; index < dataset.feature_names.size(); ++index) {
        const compute::ComputeStatus status = detail::read_column_as_double(
            frame, dataset.feature_names[index], dataset.features[index]);
        if (!status.ok()) {
            return status;
        }
    }
    if (const compute::ComputeStatus status =
            detail::read_column_as_double(frame, resolved.contract.label_column, dataset.labels);
        !status.ok()) {
        return status;
    }
    if (const compute::ComputeStatus status =
            read_integer_column(frame, params.group_column, dataset.groups);
        !status.ok()) {
        return status;
    }
    return read_integer_column(frame, resolved.contract.entity_column, dataset.entities);
}

std::string graph_suffix(const RankRunOptions& options) {
    if (options.graph.empty()) {
        return "pipeline";
    }
    return options.graph.substr(0, std::min<std::size_t>(8, options.graph.size()));
}

void fail_run(Node& node, const std::shared_ptr<pipeline::Run>& run, const std::string& message) {
    node.run_on_owner([&] {
        run->add_log({util::now_iso8601(), "error", message});
        run->set_status(pipeline::RunStatus::Failed);
    });
}

} // namespace

RankRunResult run_rank_on_table(Node& node, compute::ComputeDevice& device,
                                const ir::ExecutionGraph& graph,
                                const data::ResolvedDataset& dataset, const RankParams& params,
                                const RankRunOptions& options) {
    RankRunResult output;
    if (!dataset.ok()) {
        output.error = dataset.error;
        return output;
    }
    if (params.group_column.empty()) {
        output.error = "rank: group column is required";
        return output;
    }
    if (dataset.contract.entity_column == params.group_column ||
        dataset.contract.label_column == params.group_column) {
        output.error = "rank: group, entity, and label columns must be distinct";
        return output;
    }
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        output.error = "rank: no compute plane attached to this node";
        return output;
    }

    node.run_on_owner([&] {
        output.run = node.create_run(options.project, options.graph);
        if (output.run) {
            output.run->set_status(pipeline::RunStatus::Running);
            output.run->add_log({util::now_iso8601(), "info", "LambdaMART run started"});
        }
    });
    if (!output.run) {
        output.error = "rank: could not create run record";
        return output;
    }

    ddf::LocalShardResult shard;
    node.run_on_owner([&] {
        shard = ddf::load_local_partitions(node, dataset.table_id, rank_schema(dataset, params),
                                           device);
    });
    if (!shard.ok()) {
        output.error = "rank: load dataset partitions: " + shard.status.message;
        fail_run(node, output.run, output.error);
        return output;
    }

    RankDataset local_dataset;
    if (const compute::ComputeStatus status =
            frame_to_rank_dataset(shard.frame, dataset, params, local_dataset);
        !status.ok()) {
        output.error = status.message;
        fail_run(node, output.run, output.error);
        return output;
    }

    std::shared_ptr<compute::ComputeJob> job;
    node.run_on_owner([&] {
        job = node.create_job(compute::JobKind::Train, options.project, options.graph,
                              output.run->id());
    });
    if (!job) {
        output.error = "rank: could not create training job";
        fail_run(node, output.run, output.error);
        return output;
    }
    output.training = run_lambda_mart_job(*coordinator, *job, local_dataset, params, &node,
                                          options.model_name, "ml.rank:" + options.graph);
    if (!output.training.ok()) {
        output.error = "rank: training failed: " + job->detail();
        fail_run(node, output.run, output.error);
        return output;
    }

    if (output.training.is_root) {
        node.run_on_owner([&] {
            std::shared_ptr<data::Database> database = node.find_database(kRankResultsDatabase);
            if (!database) {
                database = node.create_database(kRankResultsDatabase);
            }
            if (!database) {
                return;
            }
            const std::string table_name = "ranked_" + graph_suffix(options);
            database->drop_table(table_name);
            const std::shared_ptr<data::Table> table = database->create_table(table_name);
            if (!table) {
                return;
            }
            for (const RankedEntity& row : output.training.ranked) {
                table->add_row({{"group", row.group},
                                {"entity", data::entity_output(
                                               dataset.contract, row.entity)},
                                {"score", row.score},
                                {"rank", row.rank},
                                {"relevance", row.relevance}});
            }
            output.ranked_table = std::string(kRankResultsDatabase) + "." + table_name;
        });
    }
    if (output.ranked_table.empty()) {
        output.error = "rank: could not materialize ranked-queue table";
        fail_run(node, output.run, output.error);
        return output;
    }

    std::vector<pipeline::RunOutput> outputs = {
        {"dataset", "training dataset", dataset.database + "." + dataset.table},
        {"model", options.model_name, output.training.model_id},
        {"dataset", "ranked queue", output.ranked_table},
        {"report", "run report", output.run->id()}};
    pipeline::RunLineage lineage;
    lineage.sources = {"dataset:" + dataset.database + "." + dataset.table};
    for (const auto& step : graph.nodes()) {
        lineage.steps.push_back(step->id);
    }
    lineage.freshness = util::now_iso8601();
    lineage.confidence = output.training.validation.ndcg_at_k;
    node.run_on_owner([&] {
        if (dataset.contract.entity_keys) {
            output.run->set_entity_key_mapping(
                dataset.contract.entity_keys->to_json());
            if (const std::shared_ptr<Model> model =
                    node.find_model(output.training.model_id)) {
                nlohmann::json feature_schema = model->feature_schema();
                feature_schema["entity_key_mapping"] =
                    dataset.contract.entity_keys->to_json();
                model->set_feature_schema(std::move(feature_schema));
            }
        }
        output.run->set_outputs(std::move(outputs));
        output.run->set_lineage(std::move(lineage));
        output.run->add_log({util::now_iso8601(), "info",
                             "NDCG@" + std::to_string(params.cutoff) + "=" +
                                 std::to_string(output.training.validation.ndcg_at_k) + ", yield@" +
                                 std::to_string(params.cutoff) + "=" +
                                 std::to_string(output.training.validation.yield_at_k)});
        output.run->set_status(pipeline::RunStatus::Succeeded);
    });
    return output;
}

struct AsyncRankRun::State {
    Node* node = nullptr;
    std::atomic<bool> done = false;
    RankRunResult result;
    std::thread worker;
};

std::shared_ptr<AsyncRankRun> AsyncRankRun::start(Node& node, ir::ExecutionGraph graph,
                                                  data::ResolvedDataset dataset, RankParams params,
                                                  RankRunOptions options) {
    auto state = std::make_shared<State>();
    state->node = &node;
    state->worker = std::thread([state, graph = std::move(graph), dataset = std::move(dataset),
                                 params = std::move(params),
                                 options = std::move(options)]() mutable {
        compute::CpuDevice device;
        state->result = run_rank_on_table(*state->node, device, graph, dataset, params, options);
        state->done.store(true, std::memory_order_release);
    });
    return std::shared_ptr<AsyncRankRun>(new AsyncRankRun(std::move(state)));
}

AsyncRankRun::AsyncRankRun(std::shared_ptr<State> state) : state_(std::move(state)) {}

AsyncRankRun::~AsyncRankRun() {
    while (!state_->done.load(std::memory_order_acquire)) {
        state_->node->drain_owner_work();
        std::this_thread::yield();
    }
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
}

bool AsyncRankRun::done() const noexcept {
    return state_->done.load(std::memory_order_acquire);
}

const RankRunResult& AsyncRankRun::result() const {
    return state_->result;
}

} // namespace zetta::ml
