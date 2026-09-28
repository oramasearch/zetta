#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/anomaly.hpp"

// The distributed ComputeJob adapter for native anomaly detection (#1046).
// Tabular tree and robust-statistic partials are combined through JobRound
// collectives. Only compact scored rows are gathered to apply the exact global
// alert budget; the trained forest is identical on every participant.
namespace zetta::ml {

inline constexpr std::chrono::milliseconds kDefaultAnomalyTimeout{10000};

struct DistributedAnomalyResult {
  AnomalyModel model;
  std::vector<AnomalyRecord> records;
  AnomalyReport report;
  compute::JobStatus status = compute::JobStatus::Failed;
  bool is_root = false;

  bool ok() const noexcept { return status == compute::JobStatus::Succeeded; }
};

// Run concurrently on every participant. global_partition_ids and feature_names
// must be identical on every member. local_partitions contains only partitions
// for which this member is the catalog's primary owner.
//
// KPI mode is deliberately single-member in phase A. Its causal ordered history
// crosses partition boundaries; a distributed prefix-state implementation
// belongs in a later slice. The function refuses a multi-member KPI run rather
// than producing alerts from incomplete history.
DistributedAnomalyResult
run_anomaly_job(compute::JobCoordinator &coordinator, compute::ComputeJob &job,
                const std::vector<AnomalyPartition> &local_partitions,
                const std::vector<std::string> &global_partition_ids,
                const std::vector<std::string> &feature_names,
                const AnomalyParams &params,
                const std::string &round_tag = "ml.anomaly",
                std::chrono::milliseconds timeout = kDefaultAnomalyTimeout);

} // namespace zetta::ml
