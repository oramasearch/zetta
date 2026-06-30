#include "zetta/ml/local_train_job.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "zetta/node.hpp"
#include "zetta/pipeline_instrumentation.hpp"
#include "zetta/timestamp.hpp"

namespace zetta::ml {

NearDataTrainResult run_near_data_train_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const ddf::DataFrameSchema& schema,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const GbdtParams& params,
    compute::ComputeDevice& device, const std::string& entity_column,
    CalibrationConfig calibration, const std::string& model_name,
    std::size_t top_k, const std::string& round_tag,
    std::chrono::milliseconds timeout) {
    NearDataTrainResult out;

    // Record this pipeline's near-data instrumentation into the node before
    // returning (it spans the train + explain + score phases), so the counters
    // survive the call and surface in system.pipeline_jobs (#589).
    auto record_instrumentation = [&] {
        node.record_pipeline_job({.job_id = job.id(),
                                  .kind = NearDataJobKind::Train,
                                  .node = node.node_id(),
                                  .partitions_read = out.partitions_read,
                                  .rows_read = out.rows_read,
                                  .local_bytes_read = out.local_bytes_read,
                                  .wire_bytes_moved = out.wire_bytes_moved,
                                  .status = out.status,
                                  .recorded_at = now_iso8601()});
    };

    // (1) Compute near data: materialize this node's shard from its OWNED partitions
    // of `table` — only the bodies it physically holds, read from local disk (no
    // full-dataset pull; the dropped embeddings are never even on disk here).
    ddf::LocalShardResult shard = ddf::load_local_partitions(node, table, schema, device);
    out.partitions_read = shard.partitions_read;
    out.rows_read = shard.rows_read;
    out.local_bytes_read = shard.bytes_read;

    if (!shard.ok()) {
        // The shard could not be built — fail this node's train job cleanly (so it is
        // still recorded) rather than entering the collective with a broken frame.
        using Outcome = compute::JobCoordinator::Outcome;
        coordinator.run(job, [&](compute::JobRound&) -> Outcome {
            return {.ok = false,
                    .detail = "near-data train: local shard load failed: " +
                              shard.status.message};
        });
        out.status = job.status();
        record_instrumentation();
        return out;
    }

    // Distinct, cross-node-agreed per-phase collective tags. The base is derived from
    // the table id (identical on every node), so the default is consistent without the
    // caller having to coordinate a tag; the suffixes keep the three phases from
    // colliding even though they run sequentially.
    const std::string base =
        round_tag.empty() ? ("near-data-train:" + table) : round_tag;

    // Measure the collective-plane bytes this node moves across all three phases (the
    // training histograms / metrics, the SHAP partials, the score pairs — not the rows).
    const std::uint64_t wire_before = coordinator.collectives().wire_bytes();

    // (2) Train (+ calibrate) over the local shard. Drives the headline `train` job and,
    // on the root, registers the calibrated Model (metrics = global AUC/log-loss/
    // accuracy, feature_schema.drivers = per-feature gains).
    DistributedTrainResult trained = run_train_job(
        coordinator, job, shard.frame, feature_columns, label_column, params, &node,
        model_name, base + ":train", calibration, timeout);
    out.model = trained.model;
    out.metrics = trained.metrics;
    out.feature_importances = trained.feature_importances;
    out.calibrated = trained.calibrated;
    out.model_id = trained.model_id;
    out.status = trained.status;
    out.is_root = trained.is_root;

    if (!trained.ok()) {
        // Training failed — surface it without attempting explain/rank over an unbuilt
        // model. The instrumentation still reflects what this node moved.
        out.wire_bytes_moved = coordinator.collectives().wire_bytes() - wire_before;
        record_instrumentation();
        return out;
    }

    // (3) Per-row TreeSHAP drivers over the same shard, with the trained model. A
    // self-created `explain` ComputeJob (so it appears in system.zetta_jobs), under the
    // same project/graph as the train job.
    std::shared_ptr<ComputeJob> explain_job =
        node.create_job(JobKind::Explain, job.project(), job.graph());
    out.explain_job_id = explain_job->id();
    DistributedExplainResult explained = run_explain_job(
        coordinator, *explain_job, trained.model, shard.frame, entity_column,
        base + ":explain", timeout);
    out.explanations = std::move(explained.explanations);
    out.feature_names = std::move(explained.feature_names);
    out.shap_base_value = explained.base_value;
    out.mean_abs_shap = std::move(explained.mean_abs_shap);
    out.top_driver = std::move(explained.top_driver);

    // (4) The global ranked at-risk list over the same shard. A self-created `score`
    // ComputeJob; the gather + device sort_by_key + broadcast make the ranking
    // identical on every node.
    std::shared_ptr<ComputeJob> score_job =
        node.create_job(JobKind::Score, job.project(), job.graph());
    out.score_job_id = score_job->id();
    DistributedScoreResult scored =
        run_score_job(coordinator, *score_job, shard.frame, trained.model,
                      entity_column, top_k, base + ":score", timeout);
    out.ranked = std::move(scored.ranked);

    out.wire_bytes_moved = coordinator.collectives().wire_bytes() - wire_before;

    // The pipeline succeeds only when all three phases did — the per-row drivers + rank
    // are part of the deliverable, not optional decoration.
    out.status = (explained.status == JobStatus::Succeeded &&
                  scored.status == JobStatus::Succeeded)
                     ? JobStatus::Succeeded
                     : JobStatus::Failed;
    record_instrumentation();
    return out;
}

}  // namespace zetta::ml
