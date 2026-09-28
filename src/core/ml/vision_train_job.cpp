#include "zetta/ml/vision_train_job.hpp"

#include <string>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {

VisionTrainResult run_vision_train_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<VisionExample> &local_examples,
    const VisionTrainParams &params, const std::string &round_tag,
    std::chrono::milliseconds timeout) {
  VisionTrainResult result;
  std::string collective_error;
  const compute::JobStatus status = coordinator.run(
      job, [&](compute::JobRound &round) -> compute::JobCoordinator::Outcome {
        const VisionAllReduce allreduce =
            [&](const std::string &tag,
                const std::vector<double> &local) -> std::vector<double> {
          const std::optional<std::vector<double>> reduced = round.allreduce(
              tag, local, zetta::cluster::CollectiveOp::Sum, timeout);
          if (!reduced) {
            collective_error = "vision train: collective '" + tag + "' failed";
            return {};
          }
          return *reduced;
        };
        result =
            train_vision_head(local_examples, params, allreduce, round_tag);
        if (!collective_error.empty()) {
          result.error = collective_error;
        }
        if (!result.ok()) {
          return {.ok = false, .detail = result.error};
        }
        return {.ok = true,
                .detail = "vision head trained on " +
                          std::to_string(result.metrics.train_rows) +
                          " rows; held out " +
                          std::to_string(result.metrics.held_out_rows)};
      });
  if (status != compute::JobStatus::Succeeded && result.error.empty()) {
    result.error =
        job.detail().empty() ? "vision train job failed" : job.detail();
  }
  return result;
}

} // namespace zetta::ml
