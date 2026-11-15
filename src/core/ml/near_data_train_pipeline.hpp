// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/local_train_job.hpp"

// Shared driver for the train(+calibrate) → explain → rank phases of the near-data
// ML pipeline, over an **already-materialized** local feature frame. This is the
// common core of `run_near_data_train_job` (#533 — the frame loaded straight from
// the node's owned partitions) and `run_near_data_featurize_train_job` (#530 — the
// frame sliced from the featurized table), factored so the two runners cannot
// drift. Internal to zetta-core (impl detail, like feature_columns.hpp) — not a
// public API.
namespace zetta {
class Node;
}

namespace zetta::ml::detail {

// Drive train (+ calibrate) → explain → rank over `frame` through `coordinator`,
// filling every `NearDataTrainResult` field EXCEPT the shard-load instrumentation
// (`partitions_read` / `rows_read` / `local_bytes_read` stay with the caller that
// materialized the frame); `out.wire_bytes_moved` covers the three phases driven
// here. `job` is the headline train ComputeJob; the explain + score jobs are
// created on `node` and their ids returned in `out`. `base_tag` names the
// collective rounds and must be identical on every participating node — the
// ":train" / ":explain" / ":score" suffixes keep the phases from colliding.
void run_train_explain_rank_phases(
    compute::JobCoordinator& coordinator, Node& node, zetta::compute::ComputeJob& job,
    const ddf::DataFrame& frame, const std::vector<std::string>& feature_columns,
    const std::string& label_column, const GbdtParams& params,
    const std::string& entity_column, CalibrationConfig calibration,
    const std::string& model_name, std::size_t top_k, const std::string& base_tag,
    std::chrono::milliseconds timeout, NearDataTrainResult& out);

}  // namespace zetta::ml::detail
