// SPDX-License-Identifier: MIT

#include "zetta/ml/language_train_job.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {

DistributedClassifierResult
run_language_classifier_job(compute::JobCoordinator& coordinator, compute::ComputeJob& job,
                            const ClassifierPartials& local, double regularization,
                            std::string embedder_version, const std::string& round_tag,
                            std::chrono::milliseconds timeout) {
    DistributedClassifierResult result;
    result.status =
        coordinator.run(job, [&](compute::JobRound& round) -> compute::JobCoordinator::Outcome {
            result.is_root = round.is_root();
            if (local.feature_label_products.empty() ||
                local.feature_label_products.size() != local.feature_squares.size()) {
                return {.ok = false,
                        .detail = "language classifier requires equal non-empty "
                                  "feature partials"};
            }
            std::vector<double> packed;
            packed.reserve(local.feature_label_products.size() * 2 + 2);
            packed.insert(packed.end(), local.feature_label_products.begin(),
                          local.feature_label_products.end());
            packed.insert(packed.end(), local.feature_squares.begin(), local.feature_squares.end());
            packed.push_back(local.label_sum);
            packed.push_back(local.row_count);

            const std::optional<std::vector<double>> reduced =
                round.allreduce(round_tag, packed, cluster::CollectiveOp::Sum, timeout);
            if (!reduced.has_value() || reduced->size() != packed.size()) {
                return {.ok = false, .detail = "language classifier allreduce failed"};
            }
            const std::size_t dimensions = local.feature_label_products.size();
            ClassifierPartials global;
            global.feature_label_products.assign(reduced->begin(), reduced->begin() + dimensions);
            global.feature_squares.assign(reduced->begin() + dimensions,
                                          reduced->begin() + dimensions * 2);
            global.label_sum = (*reduced)[dimensions * 2];
            global.row_count = (*reduced)[dimensions * 2 + 1];
            result.rows = static_cast<std::uint64_t>(global.row_count);
            result.head =
                finish_classifier_head(global, regularization, std::move(embedder_version));
            return {.ok = true,
                    .detail = "language classifier rows=" + std::to_string(result.rows)};
        });
    return result;
}

} // namespace zetta::ml
