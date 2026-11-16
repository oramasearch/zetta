// SPDX-License-Identifier: MIT

#include "zetta/ml/local_featurize_train_job.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "zetta/node.hpp"
#include "zetta/compute/pipeline_instrumentation.hpp"
#include "zetta/ddf/featurize_job.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/util/timestamp.hpp"
#include "near_data_train_pipeline.hpp"

namespace zetta::ml {

namespace {

// Read the shard's integer entity-key column to host as exact int64 (no double
// round-trip) — the same dtype set the featurization's own entity read accepts
// (ddf/featurize.cpp).
template <class Native>
compute::ComputeStatus read_entities_as_int64(const ddf::DataFrame& frame,
                                              const std::string& name,
                                              std::vector<std::int64_t>& out) {
    std::vector<Native> native;
    const compute::ComputeStatus status = frame.to_host<Native>(name, native);
    if (!status.ok()) return status;
    out.clear();
    out.reserve(native.size());
    for (const Native value : native) out.push_back(static_cast<std::int64_t>(value));
    return compute::ComputeStatus::success();
}

compute::ComputeStatus read_entity_column(const ddf::DataFrame& frame,
                                          const std::string& name,
                                          std::vector<std::int64_t>& out) {
    using compute::ComputeStatus;
    using compute::DataType;
    const ddf::Column* column = frame.column(name);
    if (column == nullptr) {
        return ComputeStatus::invalid_argument(
            "featurize-train: no entity column named '" + name + "'");
    }
    switch (column->dtype()) {
        case DataType::Int32:  return read_entities_as_int64<std::int32_t>(frame, name, out);
        case DataType::Int64:  return read_entities_as_int64<std::int64_t>(frame, name, out);
        case DataType::UInt32: return read_entities_as_int64<std::uint32_t>(frame, name, out);
        case DataType::UInt64: return read_entities_as_int64<std::uint64_t>(frame, name, out);
        default:
            return ComputeStatus::invalid_argument("featurize-train: entity column '" +
                                                   name + "' must be an integer dtype");
    }
}

// This node's slice of the global feature table: the rows whose entity came from its
// own owned partitions. The global table has one row per entity in ascending order,
// so the slice stays sorted and the slices across nodes union back to the exact
// global table (disjoint by the partitioned write path — a row lives in exactly one
// partition).
ddf::FeatureTable slice_feature_table(const ddf::FeatureTable& global,
                                      const std::unordered_set<std::int64_t>& keep) {
    ddf::FeatureTable slice;
    slice.feature_names = global.feature_names;
    slice.label_name = global.label_name;
    slice.features.resize(global.features.size());
    for (std::size_t row = 0; row < global.row_count(); ++row) {
        if (!keep.contains(global.entities[row])) continue;
        slice.entities.push_back(global.entities[row]);
        for (std::size_t column = 0; column < global.features.size(); ++column) {
            slice.features[column].push_back(global.features[column][row]);
        }
        if (global.has_label()) slice.labels.push_back(global.labels[row]);
    }
    return slice;
}

}  // namespace

NearDataFeaturizeTrainResult run_near_data_featurize_train_job(
    compute::JobCoordinator& coordinator, Node& node, zetta::compute::ComputeJob& job,
    const std::string& table, const ddf::DataFrameSchema& schema,
    const ddf::FeaturizeSpec& spec, const GbdtParams& params,
    compute::ComputeDevice& device, CalibrationConfig calibration,
    const std::string& model_name, std::size_t top_k, const std::string& round_tag,
    std::chrono::milliseconds timeout) {
    NearDataFeaturizeTrainResult out;

    // One trace per distributed job (#640): stamp the replicated job record up
    // front so the derived featurize/explain/score jobs and every peer's spans
    // all join the same trace.
    if (job.value().trace_id.empty()) {
        job.set_trace_id(zetta::observability::make_root_trace().trace_id);
    }

    const std::uint64_t wire_start = coordinator.collectives().wire_bytes();
    std::uint64_t featurize_wire = 0;
    zetta::compute::JobStatus featurize_status = zetta::compute::JobStatus::Failed;

    // Record the pipeline's near-data instrumentation as TWO system.pipeline_jobs
    // rows (#589): the featurize row carries the one shard read + the featurization
    // partials; the train row carries the feature rows this node trained on + the
    // train/explain/score partials — so summing the view never double-counts the read.
    auto record_instrumentation = [&] {
        if (!out.featurize_job_id.empty()) {
            node.record_pipeline_job(
                {.job_id = out.featurize_job_id,
                 .kind = zetta::compute::NearDataJobKind::Featurize,
                 .node = node.node_id(),
                 .partitions_read = out.partitions_read,
                 .rows_read = out.rows_read,
                 .local_bytes_read = out.local_bytes_read,
                 .wire_bytes_moved = featurize_wire,
                 .status = featurize_status,
                 .recorded_at = zetta::util::now_iso8601()});
        }
        node.record_pipeline_job({.job_id = job.id(),
                                  .kind = zetta::compute::NearDataJobKind::Train,
                                  .node = node.node_id(),
                                  .partitions_read = 0,
                                  .rows_read = out.feature_rows,
                                  .local_bytes_read = 0,
                                  .wire_bytes_moved = out.train.wire_bytes_moved,
                                  .status = out.train.status,
                                  .recorded_at = zetta::util::now_iso8601()});
    };

    // Fail the headline train job cleanly (so it is still recorded, with the reason in
    // its detail) rather than entering a collective with a broken pipeline.
    auto fail_job = [&](const std::string& detail) {
        using Outcome = compute::JobCoordinator::Outcome;
        coordinator.run(job, [&](compute::JobRound&) -> Outcome {
            return {.ok = false, .detail = detail};
        });
        out.status = job.status();
        out.train.status = job.status();
        out.wire_bytes_moved = coordinator.collectives().wire_bytes() - wire_start;
        record_instrumentation();
    };

    // The GBDT needs a target: a spec without a label column can featurize, but it
    // cannot train — reject it up front, before any collective work.
    if (spec.label_column.empty()) {
        fail_job("near-data featurize-train: the spec needs a label_column "
                 "(the GBDT target)");
        return out;
    }

    // (1) Compute near data: materialize this node's shard ONCE from its OWNED
    // partitions of `table` — only the bodies it physically holds, read from local
    // disk (no full-dataset pull; the dropped embeddings are never even on disk here).
    ddf::LocalShardResult shard = ddf::load_local_partitions(node, table, schema, device);
    out.partitions_read = shard.partitions_read;
    out.rows_read = shard.rows_read;
    out.local_bytes_read = shard.bytes_read;

    if (!shard.ok()) {
        fail_job("near-data featurize-train: local shard load failed: " +
                 shard.status.message);
        return out;
    }

    // Distinct, cross-node-agreed per-phase collective tags — the base is derived from
    // the table id (identical on every node), the suffixes keep the four phases from
    // colliding even though they run sequentially.
    const std::string base =
        round_tag.empty() ? ("near-data-featurize-train:" + table) : round_tag;

    // (2) Featurize: the recorded `featurize` ComputeJob over the local shard (used as
    // both the events and accounts frame — the static engineered-columns case reads
    // only the accounts side; a set time_column point-in-time filters to time <= as_of
    // first). The merged feature table is broadcast: identical on every node, equal to
    // a single-node featurize over the whole dataset.
    std::shared_ptr<zetta::compute::ComputeJob> featurize_job =
        node.create_job(zetta::compute::JobKind::Featurize, job.project(), job.graph());
    // The derived job belongs to the same distributed run: ONE trace (#640).
    featurize_job->set_trace_id(job.value().trace_id);
    out.featurize_job_id = featurize_job->id();
    const std::uint64_t wire_before_featurize = coordinator.collectives().wire_bytes();
    ddf::DistributedFeaturizeResult featurized = ddf::run_featurize_job(
        coordinator, *featurize_job, shard.frame, shard.frame, spec,
        base + ":featurize", timeout);
    featurize_wire = coordinator.collectives().wire_bytes() - wire_before_featurize;
    featurize_status = featurized.status;
    out.feature_table = std::move(featurized.table);
    out.is_root = featurized.is_root;

    if (!featurized.ok()) {
        fail_job("near-data featurize-train: the featurize phase failed (job " +
                 out.featurize_job_id + ": " + featurize_job->detail() + ")");
        return out;
    }

    // (3) Slice the GLOBAL feature table down to this node's rows — the entities that
    // came from its own owned partitions. A local row past the point-in-time cutoff is
    // absent from the global table, so it is excluded here too; across nodes the
    // slices are disjoint and union back to exactly the global table, so the GBDT's
    // training universe IS the feature table, each row contributed once.
    std::vector<std::int64_t> local_entities;
    if (shard.rows_read > 0) {
        const compute::ComputeStatus read =
            read_entity_column(shard.frame, spec.entity_column, local_entities);
        if (!read.ok()) {
            fail_job("near-data featurize-train: reading the entity column failed: " +
                     read.message);
            return out;
        }
    }
    const std::unordered_set<std::int64_t> local_keys(local_entities.begin(),
                                                      local_entities.end());
    const ddf::FeatureTable slice = slice_feature_table(out.feature_table, local_keys);
    out.feature_rows = slice.row_count();

    // Materialize the slice as this node's device-backed training frame. Its entity
    // column is named "entity" (the FeatureTable::to_data_frame convention).
    ddf::DataFrameResult feature_frame = slice.to_data_frame(device);
    if (!feature_frame.ok()) {
        fail_job("near-data featurize-train: materializing the feature slice failed: " +
                 feature_frame.status.message);
        return out;
    }

    // (4) Train (+ calibrate) → explain → rank over the feature slice — the shared
    // #533 phase driver (near_data_train_pipeline.hpp). The model's features are the
    // feature table's columns, its target the spec's label.
    detail::run_train_explain_rank_phases(
        coordinator, node, job, feature_frame.frame, out.feature_table.feature_names,
        out.feature_table.label_name, params, "entity", calibration, model_name,
        top_k, base, timeout, out.train);

    out.wire_bytes_moved = coordinator.collectives().wire_bytes() - wire_start;
    out.is_root = out.train.is_root;
    // The pipeline succeeds only when the featurize job AND the whole train → explain
    // → rank pipeline did.
    out.status = out.train.status;
    record_instrumentation();
    return out;
}

}  // namespace zetta::ml
