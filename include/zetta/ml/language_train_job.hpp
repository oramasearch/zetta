// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/language.hpp"

namespace zetta::ml {

struct DistributedClassifierResult {
    LinearClassifierHead head;
    compute::JobStatus status = compute::JobStatus::Failed;
    bool is_root = false;
    std::uint64_t rows = 0;

    bool ok() const noexcept {
        return status == compute::JobStatus::Succeeded;
    }
};

DistributedClassifierResult
run_language_classifier_job(compute::JobCoordinator& coordinator, compute::ComputeJob& job,
                            const ClassifierPartials& local, double regularization,
                            std::string embedder_version,
                            const std::string& round_tag = "ml.language.classifier",
                            std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

} // namespace zetta::ml
