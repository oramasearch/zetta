// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/vision.hpp"

namespace zetta::ml {

inline constexpr std::chrono::milliseconds kDefaultVisionTimeout{30000};

// Drive the source-disjoint linear-head trainer through the distributed job
// coordinator. Every participant supplies embeddings from its owned partitions;
// gradients and confusion counts cross the existing collective channel.
VisionTrainResult run_vision_train_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<VisionExample> &local_examples,
    const VisionTrainParams &params, const std::string &round_tag = "ml.vision",
    std::chrono::milliseconds timeout = kDefaultVisionTimeout);

} // namespace zetta::ml
