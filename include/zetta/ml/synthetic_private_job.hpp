#pragma once

#include <chrono>
#include <string>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/synthetic_private.hpp"

namespace zetta {
class Node;
}

namespace zetta::compute {
class CoordinatedLaunchListener;
}

// Compute-near-data runner for M-20 (#1059). Each participant reads only the
// partition bodies it owns, then exchanges the aggregate copula statistics,
// evaluation scores, and DP gradient histograms through JobRound collectives.
//
// See docs/ml/synthetic_private.md.
namespace zetta::ml {

struct DistributedSyntheticPrivateResult {
    SyntheticPrivateResult learned;
    compute::JobStatus status = compute::JobStatus::Failed;
    bool is_root = false;
    std::size_t partitions_read = 0;
    std::uint64_t rows_read = 0;
    std::uint64_t local_bytes_read = 0;
    std::uint64_t wire_bytes_moved = 0;

    bool ok() const noexcept {
        return status == compute::JobStatus::Succeeded && learned.ok();
    }
};

DistributedSyntheticPrivateResult run_synthetic_private_job(
    compute::JobCoordinator& coordinator, Node& node,
    compute::ComputeJob& job, const std::string& table_id,
    const data::DatasetContract& contract,
    const SyntheticPrivateParams& params,
    const std::string& round_tag = "synthetic_private",
    std::chrono::milliseconds timeout =
        cluster::Collectives::kDefaultTimeout);

void install_synthetic_private_launch_runner(
    compute::CoordinatedLaunchListener& listener, Node& node);

}  // namespace zetta::ml
