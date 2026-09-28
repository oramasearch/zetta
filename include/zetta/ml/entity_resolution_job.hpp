#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/entity_resolution.hpp"

namespace zetta {
class Node;
namespace cluster {
class Cluster;
}
namespace compute {
class CoordinatedLaunchListener;
}
} // namespace zetta

// Distributed execution for native entity resolution M-2 (#1072).
//
// Every participant receives the same deterministic record directory but keeps
// only the candidate pairs assigned to its participant index. EM and supervised
// scoring combine numeric partials through JobRound collectives. Clustering
// exchanges conflict-checked component memberships, not candidate pairs.
//
// See docs/ml/entity_resolution.md.
namespace zetta::ml {

inline constexpr std::chrono::milliseconds
    kDefaultEntityResolutionTimeout{10000};

enum class EntityResolutionReduce {
  Sum,
  Minimum,
  Maximum,
};

struct EntityResolutionCollectives {
  std::function<std::optional<std::vector<double>>(
      const std::string &, const std::vector<double> &,
      EntityResolutionReduce)>
      allreduce;
  std::function<std::optional<std::vector<std::vector<double>>>(
      const std::string &, const std::vector<double> &)>
      gather;
  std::function<std::optional<std::vector<double>>(
      const std::string &, const std::vector<double> &)>
      broadcast;
};

struct DistributedEntityResolutionResult {
  EntityResolutionResult resolution;
  std::vector<EntityMatchPair> owned_pairs;
  compute::JobStatus status = compute::JobStatus::Failed;
  bool is_root = false;
  std::size_t participant_index = 0;
  std::size_t participant_count = 0;

  bool ok() const noexcept {
    return status == compute::JobStatus::Succeeded && resolution.ok();
  }
};

struct EntityResolutionJobOptions {
  bool stream_pairs = false;
  bool is_output_member = false;
  std::size_t pair_page_size = 256;
  std::function<bool(const std::vector<EntityMatchPair> &)> pair_sink;
};

// Pure SPMD body used by the JobRound adapter. `records` is the deterministic
// record directory available to every participant. The returned `pairs` vector
// contains only this participant's owned candidates; entities and metrics are
// global and identical on every participant.
EntityResolutionResult resolve_entity_partition(
    const std::vector<EntityRecord> &records, std::size_t participant_index,
    std::size_t participant_count, const EntityResolutionParams &params,
    const EntityResolutionCollectives &collectives,
    const std::string &round_tag = "ml.entity_resolution");

// Run one M-2 SPMD participant through the compute job coordinator. Call
// concurrently on every member in the collective roster with the same records,
// parameters, and round tag.
DistributedEntityResolutionResult run_entity_resolution_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<EntityRecord> &records,
    const EntityResolutionParams &params = {},
    const std::string &round_tag = {},
    std::chrono::milliseconds timeout = kDefaultEntityResolutionTimeout,
    const EntityResolutionJobOptions &options = {});

const nlohmann::json &entity_resolution_launch_schema();

void install_entity_resolution_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node,
    cluster::Cluster &cluster);

} // namespace zetta::ml
