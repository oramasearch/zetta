// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/graph.hpp"

namespace zetta::ml {

inline constexpr std::chrono::milliseconds kDefaultGraphTimeout{10000};

struct DistributedGraphResult {
  GraphResult graph;
  compute::JobStatus status = compute::JobStatus::Failed;
  bool is_root = false;

  bool ok() const noexcept {
    return status == compute::JobStatus::Succeeded && graph.ok();
  }
};

// Run M-12 over this member's disjoint edge partition. Node ids and sparse edge
// keys are agreed once; partition-local counts and weights are combined with
// allreduce. PageRank then allreduces rank contributions on every iteration.
// Every participant receives the same features and report.
DistributedGraphResult
run_graph_job(compute::JobCoordinator &coordinator, compute::ComputeJob &job,
              const std::vector<GraphEdge> &local_edges,
              const GraphParams &params = {}, const std::string &round_tag = {},
              std::chrono::milliseconds timeout = kDefaultGraphTimeout);

} // namespace zetta::ml
