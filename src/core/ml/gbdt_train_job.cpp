#include "zetta/ml/gbdt_train_job.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/collectives.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/model.hpp"
#include "zetta/node.hpp"

#include "feature_columns.hpp"

namespace zetta::ml {

namespace {

using compute::ComputeStatus;
using compute::JobRound;
using detail::read_column_as_double;
using Outcome = compute::JobCoordinator::Outcome;

// A deterministic, spec-derived collective tag — the default round_tag. Same
// label + feature ordering -> same tag on every node, so SPMD callers meet in the
// same rounds without agreeing one explicitly (mirrors group_by_job / featurize_job).
std::string default_round_tag(const std::vector<std::string>& feature_columns,
                              const std::string& label_column) {
    std::string tag = "ml.gbdt:" + label_column;
    for (const std::string& column : feature_columns) {
        tag += ":";
        tag += column;
    }
    return tag;
}

// Whether a row is held out for the calibration fit, decided by a **content hash**
// of the row (every feature value + the label). Because it depends only on the
// row's content — not on which node or local index holds it — the same physical
// rows are held out under any sharding, so single-node and multi-node grow the
// trees on the same train rows and fit the calibrator on the same held-out rows.
bool row_is_holdout(const std::vector<std::vector<double>>& columns,
                    std::size_t row, double label, double fraction) {
    if (fraction <= 0.0) return false;
    if (fraction >= 1.0) return true;
    std::uint64_t hash = 1469598103934665603ull;  // FNV-1a offset basis
    const auto mix = [&](double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (bits >> (byte * 8)) & 0xffull;
            hash *= 1099511628211ull;  // FNV-1a prime
        }
    };
    for (const std::vector<double>& column : columns) mix(column[row]);
    mix(label);
    // The top 53 bits as a uniform fraction in [0, 1).
    const double unit = static_cast<double>(hash >> 11) /
                        static_cast<double>(1ull << 53);
    return unit < fraction;
}

// The GBDT checkpoint payload carried in the opaque JobCheckpoint.state (#468): the
// partial model plus its accumulated per-feature gains. The model rides its own
// validated GbdtModel JSON boundary (re-validated on decode); the importances are a
// plain numeric array, bounds-checked. Encoding is total; decoding is non-throwing
// and returns nullopt on anything malformed, so a corrupt payload just starts fresh.
std::string encode_gbdt_checkpoint(const GbdtModel& model,
                                   const std::vector<double>& importances) {
    nlohmann::json payload;
    payload["model"] = model.to_json();
    payload["feature_importances"] = importances;
    return payload.dump();
}

struct GbdtCheckpointPayload {
    GbdtModel model;
    std::vector<double> feature_importances;
};

std::optional<GbdtCheckpointPayload> decode_gbdt_checkpoint(
    const std::string& bytes) {
    const nlohmann::json payload = nlohmann::json::parse(bytes, nullptr, false);
    if (payload.is_discarded() || !payload.is_object() ||
        !payload.contains("model")) {
        return std::nullopt;
    }
    std::optional<GbdtModel> model = GbdtModel::from_json(payload.at("model"));
    if (!model) return std::nullopt;
    GbdtCheckpointPayload out;
    out.model = std::move(*model);
    if (payload.contains("feature_importances") &&
        payload.at("feature_importances").is_array()) {
        for (const nlohmann::json& value : payload.at("feature_importances")) {
            if (!value.is_number()) return std::nullopt;
            out.feature_importances.push_back(value.get<double>());
        }
    }
    return out;
}

// Map a GBDT reduction onto the collective-plane op.
CollectiveOp to_collective_op(GbdtReduce op) {
    switch (op) {
        case GbdtReduce::Sum: return CollectiveOp::Sum;
        case GbdtReduce::Min: return CollectiveOp::Minimum;
        case GbdtReduce::Max: return CollectiveOp::Maximum;
    }
    return CollectiveOp::Sum;
}

}  // namespace

DistributedTrainResult run_train_job(
    compute::JobCoordinator& coordinator, ComputeJob& job,
    const ddf::DataFrame& features,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const GbdtParams& params, Node* node,
    const std::string& model_name, const std::string& round_tag,
    CalibrationConfig calibration, std::chrono::milliseconds timeout,
    TrainCheckpointConfig checkpoint) {
    DistributedTrainResult out;
    const std::string tag =
        round_tag.empty() ? default_round_tag(feature_columns, label_column)
                          : round_tag;

    out.status = coordinator.run(job, [&](JobRound& round) -> Outcome {
        out.is_root = round.is_root();
        if (feature_columns.empty()) {
            return {.ok = false, .detail = "gbdt: no feature columns"};
        }

        // 1. Read this node's shard back from the device.
        std::vector<std::vector<double>> columns(feature_columns.size());
        for (std::size_t index = 0; index < feature_columns.size(); ++index) {
            const ComputeStatus status = read_column_as_double(
                features, feature_columns[index], columns[index]);
            if (!status.ok()) return {.ok = false, .detail = status.message};
        }
        std::vector<double> labels;
        if (const ComputeStatus status =
                read_column_as_double(features, label_column, labels);
            !status.ok()) {
            return {.ok = false, .detail = status.message};
        }

        // 1b. Optional held-out split for probability calibration. The split is a
        // content hash of the row (sharding-invariant), so the train rows — and the
        // held-out rows — are the same set regardless of how the data is sharded.
        const std::size_t shard_rows = labels.size();
        std::vector<std::vector<double>> train_columns;
        std::vector<double> train_labels;
        std::vector<std::vector<double>> holdout_columns;
        std::vector<double> holdout_labels;
        if (calibration.enabled) {
            train_columns.assign(feature_columns.size(), {});
            holdout_columns.assign(feature_columns.size(), {});
            for (std::size_t r = 0; r < shard_rows; ++r) {
                const bool hold = row_is_holdout(columns, r, labels[r],
                                                 calibration.holdout_fraction);
                for (std::size_t f = 0; f < feature_columns.size(); ++f) {
                    (hold ? holdout_columns : train_columns)[f].push_back(
                        columns[f][r]);
                }
                (hold ? holdout_labels : train_labels).push_back(labels[r]);
            }
        }
        const std::vector<std::vector<double>>& fit_columns =
            calibration.enabled ? train_columns : columns;
        const std::vector<double>& fit_labels =
            calibration.enabled ? train_labels : labels;

        // 2. Train, combining per-node statistics across the cluster via the
        // round's allreduce (the same boosting loop the single-node path runs).
        const AllReduce allreduce = [&](const std::string& round_name,
                                        const std::vector<double>& local,
                                        GbdtReduce op) -> std::vector<double> {
            const std::optional<std::vector<double>> reduced = round.allreduce(
                round_name, local, to_collective_op(op), timeout);
            return reduced ? *reduced : std::vector<double>{};
        };
        // 1c. Checkpoint / restart (RULE-5 / #468). When a store is configured,
        // resume from the latest checkpoint and checkpoint after each round. Across
        // the cluster the resume point must be AGREED — the histogram allreduce is
        // lock-step — so each node loads its local checkpoint, the minimum committed
        // round is allreduced, and every node resumes from that shared round (a node
        // lacking a checkpoint forces a fresh start rather than desyncing the rounds;
        // the trees up to a given round are identical on every node, so truncating to
        // the agreed round is safe).
        TrainHooks hooks;
        if (checkpoint.store != nullptr) {
            std::optional<GbdtCheckpointPayload> resumed;
            std::uint64_t local_round = 0;
            if (std::optional<compute::JobCheckpoint> cp =
                    checkpoint.store->latest(tag)) {
                if (std::optional<GbdtCheckpointPayload> payload =
                        decode_gbdt_checkpoint(cp->state)) {
                    local_round = payload->model.trees.size();
                    resumed = std::move(payload);
                }
            }
            const std::optional<std::vector<double>> agreed =
                round.allreduce(tag + ".resume",
                                {static_cast<double>(local_round)},
                                CollectiveOp::Minimum, timeout);
            if (!agreed || agreed->empty()) {
                return {.ok = false, .detail = "gbdt resume: agreement failed"};
            }
            const std::uint64_t agreed_round = (*agreed)[0] <= 0.0
                                                   ? 0
                                                   : static_cast<std::uint64_t>(
                                                         (*agreed)[0]);
            if (resumed && agreed_round > 0 &&
                agreed_round <= resumed->model.trees.size()) {
                const bool exact = agreed_round == resumed->model.trees.size();
                GbdtModel seed = std::move(resumed->model);
                seed.trees.resize(agreed_round);  // identical trees on every node
                hooks.resume_model = std::move(seed);
                // Resumed gains are exact only when not truncated; otherwise let them
                // re-accumulate from the resume point (a minor drivers-ranking
                // caveat on the rare mismatched-checkpoint path).
                if (exact) {
                    hooks.resume_feature_importances =
                        std::move(resumed->feature_importances);
                }
                out.resumed_from_round = agreed_round;
            }
            const std::uint32_t every = checkpoint.checkpoint_every == 0
                                            ? 1u
                                            : checkpoint.checkpoint_every;
            const int total_trees = params.num_trees;
            hooks.on_round = [&checkpoint, &tag, every, total_trees](
                                 int trees_built, const GbdtModel& partial,
                                 const std::vector<double>& importances) -> bool {
                const bool last = trees_built >= total_trees;
                if (last || (trees_built % static_cast<int>(every) == 0)) {
                    checkpoint.store->save(compute::JobCheckpoint::make(
                        tag, JobKind::Train,
                        static_cast<std::uint64_t>(trees_built),
                        static_cast<std::uint64_t>(total_trees),
                        encode_gbdt_checkpoint(partial, importances)));
                }
                return true;  // lock-step: never drain a single node unilaterally
            };
        }

        GbdtTrainResult trained = train_gbdt(fit_columns, feature_columns,
                                             fit_labels, params, allreduce, tag,
                                             hooks);
        if (!trained.ok()) {
            return {.ok = false, .detail = "gbdt train: " + trained.status.message};
        }
        out.model = std::move(trained.model);
        out.feature_importances = std::move(trained.feature_importances);

        // 2b. Fit + attach the calibrator: each node scores its held-out rows, the
        // (raw score, label) pairs are gathered to the root, the calibrator is fit
        // there and broadcast back — so every node attaches an identical map. A
        // failed fit (e.g. a single-class held-out set) broadcasts an empty payload
        // and the model is left uncalibrated (calibration is best-effort).
        if (calibration.enabled) {
            std::vector<double> calib_pairs;
            calib_pairs.reserve(holdout_labels.size() * 2);
            std::vector<double> hrow(feature_columns.size());
            for (std::size_t r = 0; r < holdout_labels.size(); ++r) {
                for (std::size_t f = 0; f < feature_columns.size(); ++f) {
                    hrow[f] = holdout_columns[f][r];
                }
                calib_pairs.push_back(out.model.predict_raw(hrow));
                calib_pairs.push_back(holdout_labels[r]);
            }
            const std::optional<std::vector<std::vector<double>>> calib_gathered =
                round.gather(tag + ".calib.gather", calib_pairs, timeout);
            if (!calib_gathered) {
                return {.ok = false, .detail = "gbdt calibration: gather failed"};
            }
            std::vector<double> calib_wire;
            if (!calib_gathered->empty()) {  // the root holds every node's pairs
                std::vector<double> raw_scores;
                std::vector<double> targets;
                for (const std::vector<double>& node_pairs : *calib_gathered) {
                    for (std::size_t i = 0; i + 1 < node_pairs.size(); i += 2) {
                        raw_scores.push_back(node_pairs[i]);
                        targets.push_back(node_pairs[i + 1]);
                    }
                }
                const CalibrationResult fit =
                    fit_calibrator(raw_scores, targets, calibration.method);
                if (fit.ok()) calib_wire = fit.calibrator.to_wire();
            }
            const std::optional<std::vector<double>> calib_result =
                round.broadcast(tag + ".calib.result", calib_wire, timeout);
            if (!calib_result) {
                return {.ok = false, .detail = "gbdt calibration: broadcast failed"};
            }
            if (std::optional<Calibrator> calibrator =
                    Calibrator::from_wire(*calib_result)) {
                out.model.calibration = std::move(*calibrator);
                out.calibrated = true;
            }
        }

        // 3. Global metrics: each node scores its local rows, the (prob, label)
        // pairs are gathered to the root, evaluated once over the union, and the
        // three scalars broadcast back — so every node ends with identical metrics.
        const std::size_t local_rows = labels.size();
        std::vector<double> pairs;
        pairs.reserve(local_rows * 2);
        std::vector<double> row(feature_columns.size());
        for (std::size_t r = 0; r < local_rows; ++r) {
            for (std::size_t f = 0; f < feature_columns.size(); ++f) {
                row[f] = columns[f][r];
            }
            pairs.push_back(out.model.predict_proba(row));
            pairs.push_back(labels[r]);
        }
        const std::optional<std::vector<std::vector<double>>> gathered =
            round.gather(tag + ".metrics.gather", pairs, timeout);
        if (!gathered) {
            return {.ok = false, .detail = "gbdt metrics: gather failed"};
        }
        std::vector<double> metric_wire;
        if (!gathered->empty()) {  // the root holds every node's pairs
            std::vector<double> probabilities;
            std::vector<double> targets;
            for (const std::vector<double>& node_pairs : *gathered) {
                for (std::size_t i = 0; i + 1 < node_pairs.size(); i += 2) {
                    probabilities.push_back(node_pairs[i]);
                    targets.push_back(node_pairs[i + 1]);
                }
            }
            const GbdtMetrics metrics =
                evaluate_binary(probabilities, targets);
            metric_wire = {metrics.auc, metrics.log_loss, metrics.accuracy};
        }
        const std::optional<std::vector<double>> result_wire =
            round.broadcast(tag + ".metrics.result", metric_wire, timeout);
        if (!result_wire || result_wire->size() != 3) {
            return {.ok = false, .detail = "gbdt metrics: broadcast failed"};
        }
        out.metrics.auc = (*result_wire)[0];
        out.metrics.log_loss = (*result_wire)[1];
        out.metrics.accuracy = (*result_wire)[2];

        return {.ok = true,
                .detail = "gbdt over " +
                          std::to_string(round.participants().size()) +
                          " nodes -> " +
                          std::to_string(out.model.tree_count()) + " trees, " +
                          std::to_string(out.model.feature_count()) +
                          " features, auc " + std::to_string(out.metrics.auc) +
                          (out.calibrated
                               ? ", calibrated (" +
                                     to_string(calibration.method) + ")"
                               : "")};
    });

    // Produce the Model: one logical record, registered by the root (it replicates
    // to peers). Each node still ran its own ComputeJob (recorded above).
    if (out.status == JobStatus::Succeeded && out.is_root && node != nullptr) {
        ModelValue value;
        value.name = model_name.empty() ? "gbdt-binary-logistic" : model_name;
        value.kind = "classifier";
        value.project = job.project();
        value.run = job.run();
        nlohmann::json drivers = nlohmann::json::array();
        for (std::size_t index = 0; index < feature_columns.size() &&
                                    index < out.feature_importances.size();
             ++index) {
            drivers.push_back({{"feature", feature_columns[index]},
                               {"importance", out.feature_importances[index]}});
        }
        value.feature_schema = {{"objective", "binary_logistic"},
                                {"features", feature_columns},
                                {"drivers", std::move(drivers)}};
        value.metrics = out.metrics.to_json();
        std::shared_ptr<Model> model = node->create_model(std::move(value));
        if (model) {
            model->set_weights(out.model.to_json().dump());
            out.model_id = model->id();
        }
    }

    // Training fully completed — the model is durable as a Model entity, so the
    // resume checkpoints are no longer needed (each node clears its own store).
    if (out.status == JobStatus::Succeeded && checkpoint.store != nullptr &&
        checkpoint.clear_on_success) {
        checkpoint.store->clear(tag);
    }
    return out;
}

}  // namespace zetta::ml
