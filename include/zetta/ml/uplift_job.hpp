#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/uplift.hpp"

// Distributed M-6 uplift training over a local, device-backed partition shard
// (#1045). Component GBDTs combine gradient histograms through JobRound
// allreduce. Row-level held-out predictions are gathered once, evaluated on the
// root, and broadcast as Qini points and the global targeting policy.
//
// See docs/ml/uplift.md.
namespace zetta::ml {

struct DistributedUpliftResult {
  UpliftResult uplift;
  compute::JobStatus status = compute::JobStatus::Failed;
  bool is_root = false;

  bool ok() const noexcept {
    return status == compute::JobStatus::Succeeded && uplift.ok();
  }
};

DistributedUpliftResult run_uplift_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const ddf::DataFrame &local_shard, const std::string &entity_column,
    const std::string &treatment_column, const std::string &outcome_column,
    const std::vector<std::string> &feature_columns, const UpliftParams &params,
    const std::string &round_tag = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

} // namespace zetta::ml
