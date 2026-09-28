#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/forecast.hpp"
#include "zetta/pipeline/run.hpp"

// The table-backed native forecast runner (#1043). This is the phase-A entry
// point the methodology registry can adapt directly: resolve a dataset, pass
// its time/target/optional-series columns, and receive a registered `forecast`
// model, a forecast table, typed run outputs, and lineage.
//
// See docs/ml/forecast.md.
namespace zetta {
class Node;
}

namespace zetta::ml {

inline constexpr std::chrono::milliseconds kDefaultForecastTimeout{10000};

struct ForecastRunParams {
  ForecastParams forecast;
  std::string series_column;
  std::string time_column;
  std::string target_column;
  std::string output_database = "forecasts";
  std::string output_table = "forecast";
  std::string model_name = "forecast";
  std::string project_id;
  std::string run_id;
};

struct ForecastRunResult {
  ForecastResult result;
  std::string model_id;
  std::string forecast_table;
  std::vector<pipeline::RunOutput> outputs;
  pipeline::RunLineage lineage;
  compute::JobStatus status = compute::JobStatus::Failed;
  std::string error;
  bool is_root = false;
  std::uint64_t rows_read = 0;
  std::size_t partitions_read = 0;
  std::uint64_t bytes_read = 0;

  bool ok() const noexcept {
    return status == compute::JobStatus::Succeeded && error.empty();
  }
};

// Distributed SPMD runner over already-local complete series. Each member calls
// this concurrently with its owned series. The root writes the model and output
// table when `node` is provided; all members receive identical metrics and
// model.
ForecastRunResult
run_forecast_job(compute::JobCoordinator &coordinator, compute::ComputeJob &job,
                 const std::vector<ForecastSeries> &local_series,
                 const ForecastRunParams &params, Node *node = nullptr,
                 const std::string &source_name = {},
                 std::chrono::milliseconds timeout = kDefaultForecastTimeout);

// Resolve the runner's columns from explicit params first, then the resolved
// dataset contract; load this member's owned partition bodies near data; and
// run the distributed job above. Series keys are numeric in phase A because DDF
// is a numeric frame. An empty series column means one series.
ForecastRunResult
run_forecast_table(Node &node, compute::ComputeDevice &device,
                   const data::ResolvedDataset &dataset,
                   const ForecastRunParams &params,
                   std::chrono::milliseconds timeout = kDefaultForecastTimeout);

} // namespace zetta::ml
