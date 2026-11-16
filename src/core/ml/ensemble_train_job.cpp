// SPDX-License-Identifier: MIT

#include "zetta/ml/ensemble_train_job.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "feature_columns.hpp"
#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {
namespace {

zetta::cluster::CollectiveOp operation_for(GbdtReduce operation) {
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

compute::ComputeStatus read_training_data(
    const ddf::DataFrame& frame,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const std::string& entity_column,
    const std::string& time_column, std::optional<double> as_of,
    EnsembleTrainingData& data) {
    data.feature_names = feature_columns;
    data.columns.resize(feature_columns.size());
    for (std::size_t feature = 0; feature < feature_columns.size(); ++feature) {
        const compute::ComputeStatus status = detail::read_column_as_double(
            frame, feature_columns[feature], data.columns[feature]);
        if (!status.ok()) {
            return status;
        }
    }
    compute::ComputeStatus status =
        detail::read_column_as_double(frame, label_column, data.labels);
    if (!status.ok()) {
        return status;
    }
    const ddf::Column* entity = frame.column(entity_column);
    if (entity == nullptr || entity->dtype() != compute::DataType::Int64) {
        return compute::ComputeStatus::invalid_argument(
            "ensemble job: entity column must be Int64");
    }
    status = frame.to_host<std::int64_t>(entity_column, data.entities);
    if (!status.ok()) {
        return status;
    }
    if (!time_column.empty()) {
        status = detail::read_column_as_double(frame, time_column, data.times);
        if (!status.ok()) {
            return status;
        }
    }
    if (!as_of || time_column.empty()) {
        return compute::ComputeStatus::success();
    }

    EnsembleTrainingData filtered;
    filtered.feature_names = data.feature_names;
    filtered.columns.resize(data.columns.size());
    for (std::size_t row = 0; row < data.row_count(); ++row) {
        if (data.times[row] > *as_of) {
            continue;
        }
        for (std::size_t feature = 0; feature < data.columns.size(); ++feature) {
            filtered.columns[feature].push_back(data.columns[feature][row]);
        }
        filtered.labels.push_back(data.labels[row]);
        filtered.entities.push_back(data.entities[row]);
        filtered.times.push_back(data.times[row]);
    }
    data = std::move(filtered);
    return compute::ComputeStatus::success();
}

}  // namespace

DistributedEnsembleResult run_ensemble_train_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const ddf::DataFrame& frame,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const std::string& entity_column,
    const std::string& time_column, const EnsembleParams& params,
    std::optional<double> as_of, const std::string& round_tag,
    std::chrono::milliseconds timeout) {
    DistributedEnsembleResult result;
    EnsembleTrainingData data;
    const compute::ComputeStatus read = read_training_data(
        frame, feature_columns, label_column, entity_column, time_column, as_of,
        data);
    if (!read.ok()) {
        coordinator.run(job, [&](compute::JobRound&) {
            return compute::JobCoordinator::Outcome{
                false, "ensemble input: " + read.message};
        });
        result.status = job.status();
        return result;
    }

    const std::string tag =
        round_tag.empty() ? "ensemble:" + job.id() : round_tag;
    coordinator.run(job, [&](compute::JobRound& round) {
        result.is_root = round.is_root();
        const AllReduce allreduce =
            [&](const std::string& name, const std::vector<double>& local,
                GbdtReduce operation) {
                const std::optional<std::vector<double>> reduced =
                    round.allreduce(name, local, operation_for(operation),
                                    timeout);
                return reduced.value_or(std::vector<double>{});
            };
        const EnsembleAllGather allgather =
            [&](const std::string& name, const std::vector<double>& local) {
                const std::optional<std::vector<std::vector<double>>> gathered =
                    round.gather(name + ".gather", local, timeout);
                if (!gathered) {
                    return std::vector<double>{};
                }
                std::vector<double> flattened;
                if (round.is_root()) {
                    for (const std::vector<double>& participant : *gathered) {
                        flattened.insert(flattened.end(), participant.begin(),
                                         participant.end());
                    }
                }
                const std::optional<std::vector<double>> broadcast =
                    round.broadcast(name + ".broadcast", flattened, timeout);
                return broadcast.value_or(std::vector<double>{});
            };
        result.training =
            train_ensemble(data, params, allreduce, allgather, tag);
        if (!result.training.ok()) {
            return compute::JobCoordinator::Outcome{
                false, "ensemble train: " + result.training.status.message};
        }
        return compute::JobCoordinator::Outcome{
            true,
            "out-of-fold stack fit on " +
                std::to_string(result.training.oof_rows) +
                " rows; held-out AUC lift over best base=" +
                std::to_string(
                    result.training.held_out_auc_lift_over_best_single)};
    });
    result.status = job.status();
    return result;
}

}  // namespace zetta::ml
