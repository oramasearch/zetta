// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/gbdt.hpp"

// Native LambdaMART learning-to-rank (#1048). Groups are the indivisible query
// or queue unit: every group is owned by one participant and is assigned wholly
// to training or validation. Lambda gradients weight pair swaps by their
// NDCG@k effect, while the existing GBDT core grows each tree from allreduced
// gradient/Hessian histograms. See docs/ml/rank.md.
namespace zetta {
class Node;
}

namespace zetta::compute {
class ComputeDevice;
}

namespace zetta::ir {
class ExecutionGraph;
}

namespace zetta::pipeline {
class Run;
}

namespace zetta::ml {

struct RankParams {
    GbdtParams gbdt;
    std::size_t cutoff = 5;
    double holdout_fraction = 0.25;
    std::uint64_t seed = 1048;
    std::string group_column = "group_id";

    RankParams();
};

struct RankDataset {
    std::vector<std::vector<double>> features;
    std::vector<std::string> feature_names;
    std::vector<double> labels;
    std::vector<std::int64_t> groups;
    std::vector<std::int64_t> entities;
    std::string label_name;
    std::string entity_name;

    std::size_t row_count() const noexcept {
        return labels.size();
    }
};

struct RankGroupSplit {
    std::vector<std::size_t> training_rows;
    std::vector<std::size_t> validation_rows;
    std::vector<std::int64_t> training_groups;
    std::vector<std::int64_t> validation_groups;
};

RankGroupSplit split_rank_groups(const std::vector<std::int64_t>& groups, double holdout_fraction,
                                 std::uint64_t seed);

struct RankMetrics {
    double ndcg_at_k = 0.0;
    double yield_at_k = 0.0;
    std::uint64_t groups = 0;
    std::uint64_t rows = 0;
};

RankMetrics evaluate_ranking(const std::vector<double>& scores, const std::vector<double>& labels,
                             const std::vector<std::int64_t>& groups, std::size_t cutoff);

struct RankedEntity {
    std::int64_t group = 0;
    std::int64_t entity = 0;
    double score = 0.0;
    double relevance = 0.0;
    std::size_t rank = 0;
};

std::vector<RankedEntity> rank_entities(const GbdtModel& model, const RankDataset& dataset);

struct LambdaMartTrainResult {
    GbdtModel model;
    std::vector<double> feature_importances;
    compute::ComputeStatus status;

    bool ok() const noexcept {
        return status.ok();
    }
};

LambdaMartTrainResult train_lambda_mart(const RankDataset& dataset, const RankParams& params,
                                        const AllReduce& allreduce,
                                        const std::string& tag = "lambdamart");

LambdaMartTrainResult train_lambda_mart(const RankDataset& dataset, const RankParams& params);

struct DistributedRankResult {
    GbdtModel model;
    std::vector<double> feature_importances;
    RankMetrics validation;
    std::vector<RankedEntity> ranked;
    std::string model_id;
    compute::JobStatus status = compute::JobStatus::Failed;
    bool is_root = false;
    std::uint64_t train_rows = 0;
    std::uint64_t train_groups = 0;

    bool ok() const noexcept {
        return status == compute::JobStatus::Succeeded;
    }
};

DistributedRankResult
run_lambda_mart_job(compute::JobCoordinator& coordinator, compute::ComputeJob& job,
                    const RankDataset& local_dataset, const RankParams& params,
                    Node* node = nullptr, const std::string& model_name = {},
                    const std::string& round_tag = "ml.rank",
                    std::chrono::milliseconds timeout = std::chrono::seconds(10));

struct RankRunOptions {
    std::string project;
    std::string graph;
    std::string model_name = "rank_model";
};

struct RankRunResult {
    std::shared_ptr<pipeline::Run> run;
    DistributedRankResult training;
    std::string ranked_table;
    std::string error;

    bool ok() const noexcept {
        return error.empty();
    }
};

// Phase-B adapter target: the dataset is already resolved through the shared
// dataset_path contract. This function reads owned partitions, trains through
// the node's live JobCoordinator, and writes a ranked-queue table plus lineage.
RankRunResult run_rank_on_table(Node& node, compute::ComputeDevice& device,
                                const ir::ExecutionGraph& graph,
                                const data::ResolvedDataset& dataset, const RankParams& params,
                                const RankRunOptions& options = {});

class AsyncRankRun {
  public:
    static std::shared_ptr<AsyncRankRun> start(Node& node, ir::ExecutionGraph graph,
                                               data::ResolvedDataset dataset, RankParams params,
                                               RankRunOptions options = {});
    ~AsyncRankRun();

    AsyncRankRun(const AsyncRankRun&) = delete;
    AsyncRankRun& operator=(const AsyncRankRun&) = delete;

    bool done() const noexcept;
    const RankRunResult& result() const;

  private:
    struct State;
    explicit AsyncRankRun(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

} // namespace zetta::ml
