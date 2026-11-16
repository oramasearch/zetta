// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/ensemble.hpp"

// Distributed ComputeJob adapter for native OOF stacking (#1057). Each
// participant supplies the rows from its owned partitions. Base GBDT statistics
// and meta-model gradients use JobRound::allreduce; score/label pairs alone are
// gathered for exact metrics. See docs/ml/ensemble.md.
namespace zetta::ml {

struct DistributedEnsembleResult {
    EnsembleTrainResult training;
    compute::JobStatus status = compute::JobStatus::Failed;
    bool is_root = false;

    bool ok() const noexcept {
        return status == compute::JobStatus::Succeeded && training.ok();
    }
};

DistributedEnsembleResult run_ensemble_train_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const ddf::DataFrame& frame,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const std::string& entity_column,
    const std::string& time_column, const EnsembleParams& params,
    std::optional<double> as_of = std::nullopt,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

}  // namespace zetta::ml
