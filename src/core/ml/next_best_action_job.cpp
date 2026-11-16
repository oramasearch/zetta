// SPDX-License-Identifier: MIT

#include "zetta/ml/next_best_action_job.hpp"

#include <optional>
#include <utility>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {
namespace {

zetta::cluster::CollectiveOp collective_op(GbdtReduce operation) {
  switch (operation) {
  case GbdtReduce::Sum:
    return zetta::cluster::CollectiveOp::Sum;
  case GbdtReduce::Min:
    return zetta::cluster::CollectiveOp::Minimum;
  case GbdtReduce::Max:
    return zetta::cluster::CollectiveOp::Maximum;
  }
  return zetta::cluster::CollectiveOp::Sum;
}

} // namespace

DistributedNextBestActionResult run_next_best_action_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<NextBestActionObservation> &local_observations,
    const std::vector<std::string> &feature_names,
    const NextBestActionParams &params, const std::string &round_tag,
    std::chrono::milliseconds timeout) {
  DistributedNextBestActionResult result;
  result.status = coordinator.run(
      job, [&](compute::JobRound &round) -> compute::JobCoordinator::Outcome {
        result.is_root = round.is_root();
        const NextBestActionAllReduce allreduce =
            [&](const std::string &name, const std::vector<double> &local,
                GbdtReduce operation) {
              const std::optional<std::vector<double>> reduced =
                  round.allreduce(round_tag + "." + name, local,
                                  collective_op(operation), timeout);
              return reduced ? *reduced : std::vector<double>{};
            };
        NextBestActionResult trained = train_next_best_action(
            local_observations, feature_names, params, allreduce);
        static_cast<NextBestActionResult &>(result) = std::move(trained);
        if (!result.error.empty()) {
          return {.ok = false, .detail = result.error};
        }
        return {.ok = true,
                .detail = "trained " +
                          std::to_string(result.propensity_models.size()) +
                          " action propensity models; ranked " +
                          std::to_string(result.recommendations.size()) +
                          " entity-action candidates"};
      });
  if (result.status != compute::JobStatus::Succeeded && result.error.empty()) {
    result.error = job.detail();
  }
  return result;
}

} // namespace zetta::ml
