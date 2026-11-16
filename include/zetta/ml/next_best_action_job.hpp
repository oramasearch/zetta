// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/next_best_action.hpp"

namespace zetta::ml {

inline constexpr std::chrono::milliseconds kDefaultNextBestActionTimeout{10000};

struct DistributedNextBestActionResult : NextBestActionResult {
  zetta::compute::JobStatus status = zetta::compute::JobStatus::Queued;
  bool is_root = false;

  bool ok() const noexcept {
    return status == zetta::compute::JobStatus::Succeeded && error.empty();
  }
};

// Drive the native trainer through the recorded SPMD job lifecycle. Call on all
// participants with disjoint owned observations and the same tag/parameters.
DistributedNextBestActionResult run_next_best_action_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<NextBestActionObservation> &local_observations,
    const std::vector<std::string> &feature_names,
    const NextBestActionParams &params = {},
    const std::string &round_tag = "ml.next_best_action",
    std::chrono::milliseconds timeout = kDefaultNextBestActionTimeout);

} // namespace zetta::ml
