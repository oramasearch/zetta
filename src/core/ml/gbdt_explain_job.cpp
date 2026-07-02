#include "zetta/ml/gbdt_explain_job.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/runtime.hpp"

#include "feature_columns.hpp"

// The distributed `explain` ComputeJob: exact per-entity TreeSHAP over a shard of a
// feature table, plus a globally-allreduced mean-|φ| driver ranking. The math is in
// tree_shap.cpp; this file is the job wrapper (mirrors gbdt_train_job.cpp). See
// docs/ml/tree-shap.md.
namespace zetta::ml {

namespace {

using compute::ComputeStatus;
using compute::JobRound;
using detail::read_column_as_double;
using Outcome = compute::JobCoordinator::Outcome;

// A deterministic, model-derived collective tag — the default round_tag. Same model
// feature ordering -> same tag on every node, so SPMD callers meet in the same round
// without agreeing one explicitly (mirrors gbdt_train_job / featurize_job).
std::string default_round_tag(const GbdtModel& model) {
    std::string tag = "ml.explain";
    for (const std::string& name : model.feature_names) {
        tag += ":";
        tag += name;
    }
    return tag;
}

}  // namespace

DistributedExplainResult run_explain_job(
    compute::JobCoordinator& coordinator, ComputeJob& job, const GbdtModel& model,
    const ddf::DataFrame& features, const std::string& entity_column,
    const std::string& round_tag, std::chrono::milliseconds timeout) {
    DistributedExplainResult out;
    out.feature_names = model.feature_names;
    out.base_value = model_base_value(model);
    const std::size_t feature_count = model.feature_count();
    out.mean_abs_shap.assign(feature_count, 0.0);
    const std::string tag =
        round_tag.empty() ? default_round_tag(model) : round_tag;

    out.status = coordinator.run(job, [&](JobRound& round) -> Outcome {
        out.is_root = round.is_root();
        if (feature_count == 0) {
            return {.ok = false, .detail = "explain: model has no features"};
        }

        // 1. Read this node's shard back from the device — the model's feature
        // columns (its fixed ordering) and, optionally, the entity-key column.
        std::vector<std::vector<double>> columns(feature_count);
        for (std::size_t index = 0; index < feature_count; ++index) {
            const ComputeStatus status = read_column_as_double(
                features, model.feature_names[index], columns[index]);
            if (!status.ok()) return {.ok = false, .detail = status.message};
        }
        const std::size_t local_rows = columns.empty() ? 0 : columns[0].size();
        std::vector<double> entities;
        if (!entity_column.empty()) {
            const ComputeStatus status =
                read_column_as_double(features, entity_column, entities);
            if (!status.ok()) return {.ok = false, .detail = status.message};
        }

        // 2. Per-entity exact TreeSHAP, fanned across the local rows (intra-node
        // parallelism over the shared runtime pool — each row writes its own slot,
        // so the accumulation is race-free, the trainer's pattern). Pure + thread-safe.
        out.explanations.assign(local_rows, EntityExplanation{});
        if (local_rows > 0) {
            runtime::parallel_for(
                local_rows, [&](std::size_t, std::size_t begin, std::size_t end) {
                    std::vector<double> row(feature_count);
                    for (std::size_t r = begin; r < end; ++r) {
                        for (std::size_t f = 0; f < feature_count; ++f) {
                            row[f] = columns[f][r];
                        }
                        const ShapExplanation shap = tree_shap(model, row);
                        const ShapDriver driver = dominant_driver(model, shap);
                        EntityExplanation& entry = out.explanations[r];
                        entry.entity =
                            r < entities.size()
                                ? static_cast<std::int64_t>(entities[r])
                                : static_cast<std::int64_t>(r);
                        entry.predicted = shap.predicted;
                        entry.values = shap.values;
                        entry.dominant_feature = driver.feature;
                        entry.dominant_index = driver.feature_index;
                        entry.dominant_value = driver.value;
                    }
                });
        }

        // 3. Global mean |φ| per feature: each node sums |φ| over its rows; one
        // allreduce(Sum) combines those + the row counts, so every node finalizes the
        // identical population-level driver ranking. The partial rides the
        // schema-validated CollectiveMessage (no new wire format).
        std::vector<double> partial(feature_count + 1, 0.0);
        for (const EntityExplanation& entry : out.explanations) {
            for (std::size_t f = 0; f < feature_count && f < entry.values.size();
                 ++f) {
                partial[f] += std::abs(entry.values[f]);
            }
        }
        partial[feature_count] = static_cast<double>(local_rows);
        const std::optional<std::vector<double>> reduced =
            round.allreduce(tag + ".driver_sum", partial, CollectiveOp::Sum, timeout);
        if (!reduced || reduced->size() != feature_count + 1) {
            return {.ok = false, .detail = "explain: driver allreduce failed"};
        }
        const double global_count = (*reduced)[feature_count];
        out.global_entity_count = static_cast<std::size_t>(global_count);
        double best = -1.0;
        for (std::size_t f = 0; f < feature_count; ++f) {
            out.mean_abs_shap[f] =
                global_count > 0.0 ? (*reduced)[f] / global_count : 0.0;
            if (out.mean_abs_shap[f] > best) {  // ties -> lower index
                best = out.mean_abs_shap[f];
                out.top_driver = model.feature_names[f];
            }
        }

        return {.ok = true,
                .detail = "explain over " +
                          std::to_string(round.participants().size()) + " nodes -> " +
                          std::to_string(out.global_entity_count) + " entities x " +
                          std::to_string(feature_count) + " features, top driver " +
                          out.top_driver};
    });

    return out;
}

}  // namespace zetta::ml
