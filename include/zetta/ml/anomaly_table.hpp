// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/anomaly_job.hpp"

namespace zetta {
class Node;
}
namespace zetta::pipeline {
class Run;
}

// The resolved-dataset runner for anomaly detection (#1046). This is the public
// phase-A entry that the methodology table can call in phase B: it reads
// primary owned partitions near data, runs the native trainer, registers an
// `anomaly` Model, writes queryable anomaly/report tables, and attaches output
// references plus source lineage to an optional Run.
namespace zetta::ml {

struct AnomalyTableResult {
  std::string model_id;
  std::string anomaly_table;
  std::string report_table;
  std::vector<AnomalyRecord> records;
  AnomalyReport report;
  compute::JobStatus status = compute::JobStatus::Failed;
  std::string error;

  bool ok() const noexcept {
    return status == compute::JobStatus::Succeeded && error.empty();
  }
};

struct AnomalyTableOptions {
  std::string round_tag;
  std::string model_name;
  bool write_outputs = true;
  bool materialize_on_caller = false;
};

// Run concurrently on each cluster participant for tabular mode. The primary
// owner of each catalog partition supplies that partition's tree subsamples;
// only the deterministic collective root materializes the replicated outputs.
// A single-member call is the complete KPI path in phase A.
AnomalyTableResult
run_anomaly_on_table(Node &node, const data::ResolvedDataset &dataset,
                     const AnomalyParams &params, pipeline::Run *run = nullptr,
                     std::chrono::milliseconds timeout = kDefaultAnomalyTimeout,
                     const AnomalyTableOptions &options = {});

} // namespace zetta::ml
