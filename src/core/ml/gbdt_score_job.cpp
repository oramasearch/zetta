#include "zetta/ml/gbdt_score_job.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/device_buffer.hpp"

#include "feature_columns.hpp"

namespace zetta::ml {

namespace {

using compute::ComputeDevice;
using compute::ComputeStatus;
using compute::DeviceBuffer;
using compute::JobRound;
using compute::SortOrder;
using detail::read_column_as_double;
using Outcome = compute::JobCoordinator::Outcome;

// A deterministic, model-derived collective tag — the default round_tag. Same model
// feature ordering -> same tag on every node, so SPMD callers meet in the same
// rounds without agreeing one explicitly (mirrors gbdt_train_job's default).
std::string default_round_tag(const std::string& entity_column,
                              const GbdtModel& model) {
    std::string tag = "ml.score:" + entity_column;
    for (const std::string& name : model.feature_names) {
        tag += ":";
        tag += name;
    }
    return tag;
}

// Rank a flat union of (entity, risk) pairs by risk descending — on the device, so
// the ranking runs on the GPU when the frame lives there and falls back to the CPU
// otherwise (zetta's AI-compute direction). Uses the device `sort_by_key` operator
// (the op the device header documents as "sort entities by risk score, carrying
// their ids"): a first ascending sort by entity canonicalizes the order
// independent of how the rows were sharded, then a stable descending sort by risk
// gives risk-descending with an entity-ascending tie-break — so single-node and
// multi-node produce the **same** ranking. Writes the ranked pairs into `entities`
// / `risks` in place. Non-throwing.
ComputeStatus rank_on_device(ComputeDevice& device, std::vector<double>& entities,
                             std::vector<double>& risks) {
    const std::size_t count = entities.size();
    if (count == 0) return ComputeStatus::success();  // device sort rejects empty

    DeviceBuffer entity_buffer =
        device.upload<double>(std::span<const double>(entities));
    DeviceBuffer risk_buffer =
        device.upload<double>(std::span<const double>(risks));
    if (entity_buffer.empty() || risk_buffer.empty()) {
        return ComputeStatus::out_of_memory("score: rank buffer upload failed");
    }

    // 1. Canonicalize by entity ascending (sharding-independent order).
    if (const ComputeStatus status = device.sort_by_key(
            entity_buffer, risk_buffer, SortOrder::Ascending);
        !status.ok()) {
        return status;
    }
    // 2. Rank by risk descending; stable -> equal risks keep entity-ascending order.
    if (const ComputeStatus status =
            device.sort_by_key(risk_buffer, entity_buffer, SortOrder::Descending);
        !status.ok()) {
        return status;
    }
    if (const ComputeStatus status = device.synchronize(); !status.ok()) {
        return status;
    }

    if (const ComputeStatus status = device.download<double>(
            risk_buffer, std::span<double>(risks));
        !status.ok()) {
        return status;
    }
    return device.download<double>(entity_buffer, std::span<double>(entities));
}

}  // namespace

DistributedScoreResult run_score_job(compute::JobCoordinator& coordinator,
                                     ComputeJob& job,
                                     const ddf::DataFrame& features,
                                     const GbdtModel& model,
                                     const std::string& entity_column,
                                     std::size_t top_k,
                                     const std::string& round_tag,
                                     std::chrono::milliseconds timeout) {
    DistributedScoreResult out;
    const std::string tag =
        round_tag.empty() ? default_round_tag(entity_column, model) : round_tag;

    out.status = coordinator.run(job, [&](JobRound& round) -> Outcome {
        out.is_root = round.is_root();
        ComputeDevice* device = features.device();
        if (device == nullptr) {
            return {.ok = false, .detail = "score: feature frame has no device"};
        }

        // 1. Read this node's shard back from the device: the entity key column +
        // each column the model's feature ordering names (the device download seam).
        std::vector<double> entities;
        if (const ComputeStatus status =
                read_column_as_double(features, entity_column, entities);
            !status.ok()) {
            return {.ok = false, .detail = status.message};
        }
        std::vector<std::vector<double>> columns(model.feature_count());
        for (std::size_t index = 0; index < model.feature_count(); ++index) {
            if (const ComputeStatus status = read_column_as_double(
                    features, model.feature_names[index], columns[index]);
                !status.ok()) {
                return {.ok = false, .detail = status.message};
            }
        }

        // 2. Tree-predict the shard host-side: calibrated risk per local entity.
        std::vector<double> risks;
        if (const ComputeStatus status =
                predict_risk(model, columns, model.feature_names, risks);
            !status.ok()) {
            return {.ok = false, .detail = status.message};
        }
        if (entities.size() != risks.size()) {
            return {.ok = false, .detail = "score: entity / row count mismatch"};
        }

        // 3. Gather every node's (entity, risk) pairs to the root.
        std::vector<double> pairs;
        pairs.reserve(risks.size() * 2);
        for (std::size_t r = 0; r < risks.size(); ++r) {
            pairs.push_back(static_cast<double>(entities[r]));
            pairs.push_back(risks[r]);
        }
        const std::optional<std::vector<std::vector<double>>> gathered =
            round.gather(tag + ".gather", pairs, timeout);
        if (!gathered) {
            return {.ok = false, .detail = "score: gather failed"};
        }

        // 4. The root ranks the union (on the device), truncates to top_k, and
        // broadcasts the ranked pairs back so every node holds the same list.
        std::vector<double> ranked_wire;
        if (!gathered->empty()) {  // the root holds every node's pairs
            std::vector<double> union_entities;
            std::vector<double> union_risks;
            for (const std::vector<double>& node_pairs : *gathered) {
                for (std::size_t i = 0; i + 1 < node_pairs.size(); i += 2) {
                    union_entities.push_back(node_pairs[i]);
                    union_risks.push_back(node_pairs[i + 1]);
                }
            }
            if (const ComputeStatus status =
                    rank_on_device(*device, union_entities, union_risks);
                !status.ok()) {
                return {.ok = false, .detail = "score rank: " + status.message};
            }
            const std::size_t kept = (top_k > 0 && top_k < union_entities.size())
                                         ? top_k
                                         : union_entities.size();
            ranked_wire.reserve(kept * 2);
            for (std::size_t r = 0; r < kept; ++r) {
                ranked_wire.push_back(union_entities[r]);
                ranked_wire.push_back(union_risks[r]);
            }
        }
        const std::optional<std::vector<double>> result_wire =
            round.broadcast(tag + ".result", ranked_wire, timeout);
        if (!result_wire) {
            return {.ok = false, .detail = "score: broadcast failed"};
        }

        // 5. Every node decodes the global ranked at-risk list.
        out.ranked.clear();
        out.ranked.reserve(result_wire->size() / 2);
        for (std::size_t i = 0; i + 1 < result_wire->size(); i += 2) {
            out.ranked.push_back(
                EntityScore{.entity = static_cast<std::int64_t>((*result_wire)[i]),
                            .risk = (*result_wire)[i + 1]});
        }

        return {.ok = true,
                .detail = "score over " +
                          std::to_string(round.participants().size()) +
                          " nodes -> " + std::to_string(out.ranked.size()) +
                          " entities ranked"};
    });

    return out;
}

}  // namespace zetta::ml
