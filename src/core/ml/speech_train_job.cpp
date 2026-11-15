// SPDX-License-Identifier: MIT

#include "zetta/ml/speech_train_job.hpp"

#include <string>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {

SpeechStatistics run_speech_statistics_job(compute::JobCoordinator &coordinator,
                                           compute::ComputeJob &job,
                                           const SpeechStatistics &local,
                                           const std::string &round_tag,
                                           std::chrono::milliseconds timeout,
                                           std::string *error) {
  SpeechStatistics result;
  const compute::JobStatus status = coordinator.run(
      job, [&](compute::JobRound &round) -> compute::JobCoordinator::Outcome {
        bool reduction_failed = false;
        const SpeechAllReduce reduce = [&](const std::string &tag,
                                           const std::vector<double> &values) {
          const auto reduced =
              round.allreduce(tag, values, cluster::CollectiveOp::Sum, timeout);
          if (!reduced) {
            reduction_failed = true;
            return std::vector<double>{};
          }
          return *reduced;
        };
        result = aggregate_speech_statistics(local, reduce, round_tag);
        if (reduction_failed) {
          return {.ok = false, .detail = "speech statistics allreduce failed"};
        }
        return {.ok = true, .detail = "speech statistics combined"};
      });
  if (status != compute::JobStatus::Succeeded && error != nullptr) {
    *error = job.detail();
  }
  return result;
}

} // namespace zetta::ml
