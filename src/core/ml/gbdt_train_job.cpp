#include "zetta/ml/gbdt_train_job.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

#include "feature_columns.hpp"

namespace zetta::ml {

namespace {

using compute::ComputeStatus;
using compute::JobRound;
using detail::read_column_as_double;
using Outcome = compute::JobCoordinator::Outcome;

struct RegressionMetrics {
    double rmse = 0.0;
    double mae = 0.0;
};

RegressionMetrics evaluate_regression(
    const std::vector<double>& predictions,
    const std::vector<double>& targets) {
    if (predictions.empty() || predictions.size() != targets.size()) {
        return {};
    }
    double squared_error = 0.0;
    double absolute_error = 0.0;
    for (std::size_t index = 0; index < predictions.size(); ++index) {
        const double residual = predictions[index] - targets[index];
        squared_error += residual * residual;
        absolute_error += std::abs(residual);
    }
    const double count = static_cast<double>(predictions.size());
    return {std::sqrt(squared_error / count), absolute_error / count};
}

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
zetta::cluster::CollectiveOp to_collective_op(GbdtReduce op) {
    switch (op) {
        case GbdtReduce::Sum: return zetta::cluster::CollectiveOp::Sum;
        case GbdtReduce::Min: return zetta::cluster::CollectiveOp::Minimum;
        case GbdtReduce::Max: return zetta::cluster::CollectiveOp::Maximum;
    }
    return zetta::cluster::CollectiveOp::Sum;
}

}  // namespace

DistributedTrainResult run_train_job(
    compute::JobCoordinator& coordinator, zetta::compute::ComputeJob& job,
    const ddf::DataFrame& features,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const GbdtParams& params, Node* node,
    const std::string& model_name, const std::string& round_tag,
    CalibrationConfig calibration, std::chrono::milliseconds timeout,
    TrainCheckpointConfig checkpoint, const std::string& run_id,
    TrainProgressCallback progress,
    const std::vector<double>& validation_times) {
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
        // Rows held out to MEASURE the model (#953), kept disjoint from the
        // calibration holdout: a row that fit the calibration has influenced the
        // model's probabilities and is therefore not an unseen row.
        std::vector<std::vector<double>> eval_columns;
        std::vector<double> eval_labels;
        const bool wants_eval_holdout = params.holdout_fraction > 0.0;
        const bool splitting = calibration.enabled || wants_eval_holdout;
        std::vector<bool> temporal_eval(shard_rows, false);
        bool temporal_split = false;
        if (wants_eval_holdout) {
            const double local_has_times =
                validation_times.size() == shard_rows &&
                        (shard_rows == 0 || !validation_times.empty())
                    ? 1.0
                    : 0.0;
            const auto agreed = round.allreduce(
                tag + ".validation_times.enabled", {local_has_times},
                zetta::cluster::CollectiveOp::Minimum, timeout);
            if (!agreed || agreed->size() != 1) {
                return {.ok = false,
                        .detail = "gbdt validation times: agreement failed"};
            }
            temporal_split = agreed->front() == 1.0;
        }
        if (temporal_split) {
            const auto gathered = round.gather(
                tag + ".validation_times.gather", validation_times, timeout);
            if (!gathered) {
                return {.ok = false,
                        .detail = "gbdt validation times: gather failed"};
            }
            std::vector<double> cutoff_wire;
            if (round.is_root()) {
                std::vector<double> global_times;
                for (const std::vector<double>& member : *gathered) {
                    global_times.insert(global_times.end(), member.begin(),
                                        member.end());
                }
                if (global_times.size() > 1) {
                    std::ranges::stable_sort(global_times);
                    const std::size_t held_out = std::clamp<std::size_t>(
                        static_cast<std::size_t>(std::ceil(
                            static_cast<double>(global_times.size()) *
                            params.holdout_fraction)),
                        1, global_times.size() - 1);
                    cutoff_wire.push_back(
                        global_times[global_times.size() - held_out]);
                }
            }
            const auto cutoff = round.broadcast(
                tag + ".validation_times.cutoff", cutoff_wire, timeout);
            if (!cutoff) {
                return {.ok = false,
                        .detail = "gbdt validation times: broadcast failed"};
            }
            temporal_split = cutoff->size() == 1;
            if (temporal_split) {
                for (std::size_t row = 0; row < shard_rows; ++row) {
                    temporal_eval[row] = validation_times[row] >= cutoff->front();
                }
            }
        }
        if (splitting) {
            train_columns.assign(feature_columns.size(), {});
            holdout_columns.assign(feature_columns.size(), {});
            eval_columns.assign(feature_columns.size(), {});
            for (std::size_t r = 0; r < shard_rows; ++r) {
                // Evaluation is decided first and wins, so the measured set is the
                // same rows whether or not calibration is on.
                const bool evaluate =
                    wants_eval_holdout &&
                    (temporal_split
                         ? temporal_eval[r]
                         : row_is_holdout(columns, r, labels[r],
                                          params.holdout_fraction));
                // The hash input is unchanged from what calibration has always
                // used. Disjointness comes from `!evaluate`; perturbing the input
                // would change WHICH rows calibrate, and so the calibrated
                // probabilities, for every run that never asked for an evaluation
                // holdout.
                const bool calibrate =
                    !evaluate && calibration.enabled &&
                    row_is_holdout(columns, r, labels[r], calibration.holdout_fraction);
                std::vector<std::vector<double>>& into =
                    evaluate ? eval_columns
                             : (calibrate ? holdout_columns : train_columns);
                for (std::size_t f = 0; f < feature_columns.size(); ++f) {
                    into[f].push_back(columns[f][r]);
                }
                (evaluate ? eval_labels
                          : (calibrate ? holdout_labels : train_labels))
                    .push_back(labels[r]);
            }
        }
        const std::vector<std::vector<double>>& fit_columns =
            splitting ? train_columns : columns;
        const std::vector<double>& fit_labels = splitting ? train_labels : labels;

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
        // Why the metrics hook stopped training, when it did: train_gbdt's
        // status names only the tree, the hook knows the reason.
        std::string metrics_stop;
        // The root owns the Run; peers still participate in metric reductions.
        const auto metrics_enabled = round.broadcast(
            tag + ".metrics.enabled",
            {node != nullptr && (!run_id.empty() || progress) ? 1.0 : 0.0},
            timeout);
        if (!metrics_enabled || metrics_enabled->size() != 1) {
            return {.ok = false,
                    .detail = "gbdt run metrics: participant agreement failed"};
        }
        if (metrics_enabled->front() != 0.0) {
            hooks.on_metrics = [](int, const std::map<std::string, double>&) {
                return true;
            };
        }
        if (round.is_root() && node != nullptr && !run_id.empty()) {
            nlohmann::json run_params = params.to_json();
            run_params["trainer"] = "gbdt";
            run_params["features"] = feature_columns.size();
            bool params_logged = false;
            node->run_on_owner([&] {
                const std::shared_ptr<pipeline::Run> run =
                    node->find_run(run_id);
                if (run && !run->value().metrics.at("params").empty()) {
                    params_logged = true;
                } else {
                    params_logged = node->log_run_params(run_id, run_params);
                }
            });
            if (!params_logged) {
                return {.ok = false,
                        .detail = "gbdt run metrics: parameters could not be logged"};
            }
            hooks.on_metrics = [node, &run_id, &progress, &metrics_stop](
                                   int step,
                                   const std::map<std::string, double>& values) {
                if (!run_id.empty()) {
                    std::string error;
                    bool logged = false;
                    node->run_on_owner([&] {
                        // One batch per tree: coalesce the Run's
                        // replicated capture (#1217).
                        logged = node->log_run_metrics(
                            run_id, static_cast<std::uint64_t>(step), values,
                            {}, &error, /*coalesce=*/true);
                    });
                    if (!logged) {
                        metrics_stop = "run metric logging failed at tree " +
                                       std::to_string(step) +
                                       (error.empty() ? "" : ": " + error);
                        return false;
                    }
                }
                if (progress && !progress(step, values)) {
                    metrics_stop = "training stopped by its caller after tree " +
                                   std::to_string(step);
                    return false;
                }
                return true;
            };
        }
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
                                zetta::cluster::CollectiveOp::Minimum, timeout);
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
                        tag, zetta::compute::JobKind::Train,
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
            return {.ok = false,
                    .detail = "gbdt train: " + (metrics_stop.empty()
                                                    ? trained.status.message
                                                    : metrics_stop)};
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

        // 3. Global metrics: each node scores the rows it fit — plus any it held
        // out — the (prediction, label) pairs are gathered to the root, evaluated
        // once over the union, and the scalars broadcast back, so every node ends
        // with identical objective-appropriate metrics.
        //
        // Both sets of pairs ride THIS ONE gather, length-prefixed (#953). The
        // obvious shape — a second gather/broadcast guarded by
        // `wants_eval_holdout` — is a hang waiting to happen: a node that
        // disagreed about whether a holdout was asked for would sit in a
        // collective its peers never enter, and the run would block until the
        // timeout with no error naming the cause. One unconditional collective
        // cannot desync, whatever any node believes about the parameter.
        const auto score_rows_of =
            [&](const std::vector<std::vector<double>>& cols,
                const std::vector<double>& target, std::vector<double>& into) {
                std::vector<double> row(feature_columns.size());
                for (std::size_t r = 0; r < target.size(); ++r) {
                    for (std::size_t f = 0; f < feature_columns.size(); ++f) {
                        row[f] = cols[f][r];
                    }
                    into.push_back(out.model.predict_proba(row));
                    into.push_back(target[r]);
                }
            };

        // In-sample is scored over the rows the trees were FIT on — not over every
        // local row. With no holdout of either kind those are the same set and the
        // numbers are unchanged; once rows are held back, scoring them under the
        // name "in sample" would make the label false and quietly mix the two
        // populations the whole distinction exists to keep apart.
        //
        // A consequence worth knowing: with calibration on, train_rows +
        // held_out_rows is LESS than the row count, because the calibration
        // holdout fit neither the trees nor the evaluation.
        std::vector<double> pairs;
        pairs.reserve(fit_labels.size() * 2 + 1);
        pairs.push_back(0.0);  // placeholder: count of in-sample doubles
        score_rows_of(fit_columns, fit_labels, pairs);
        pairs[0] = static_cast<double>(pairs.size() - 1);
        score_rows_of(eval_columns, eval_labels, pairs);

        const std::optional<std::vector<std::vector<double>>> gathered =
            round.gather(tag + ".metrics.gather", pairs, timeout);
        if (!gathered) {
            return {.ok = false, .detail = "gbdt metrics: gather failed"};
        }
        std::vector<double> metric_wire;
        if (!gathered->empty()) {  // the root holds every node's pairs
            std::vector<double> probabilities, targets;
            std::vector<double> held_probabilities, held_targets;
            for (const std::vector<double>& node_pairs : *gathered) {
                if (node_pairs.empty()) continue;
                // Clamp rather than trust: the split index came off the wire, and
                // a bad one would otherwise index past the end.
                const std::size_t split = std::min<std::size_t>(
                    static_cast<std::size_t>(std::max(0.0, node_pairs[0])),
                    node_pairs.size() - 1);
                for (std::size_t i = 1; i + 1 < node_pairs.size(); i += 2) {
                    const bool held = i > split;
                    (held ? held_probabilities : probabilities)
                        .push_back(node_pairs[i]);
                    (held ? held_targets : targets).push_back(node_pairs[i + 1]);
                }
            }
            const bool measured = !held_targets.empty();
            if (params.objective == "squared_error") {
                const RegressionMetrics metrics =
                    evaluate_regression(probabilities, targets);
                const RegressionMetrics held =
                    measured ? evaluate_regression(held_probabilities,
                                                   held_targets)
                             : RegressionMetrics{};
                metric_wire = {metrics.rmse,
                               metrics.mae,
                               0.0,
                               measured ? 1.0 : 0.0,
                               held.rmse,
                               held.mae,
                               0.0,
                               static_cast<double>(targets.size()),
                               static_cast<double>(held_targets.size())};
            } else {
                const GbdtMetrics metrics =
                    evaluate_binary(probabilities, targets);
                // An empty holdout is reported as absent, never as zeros — a
                // zeroed AUC reads as a model that ranks backwards.
                const GbdtMetrics held =
                    measured
                        ? evaluate_binary(held_probabilities, held_targets)
                        : GbdtMetrics{};
                metric_wire = {metrics.auc,
                               metrics.log_loss,
                               metrics.accuracy,
                               measured ? 1.0 : 0.0,
                               held.auc,
                               held.log_loss,
                               held.accuracy,
                               static_cast<double>(targets.size()),
                               static_cast<double>(held_targets.size())};
            }
        }
        const std::optional<std::vector<double>> result_wire =
            round.broadcast(tag + ".metrics.result", metric_wire, timeout);
        if (!result_wire || result_wire->size() != 9) {
            return {.ok = false, .detail = "gbdt metrics: broadcast failed"};
        }
        out.regression = params.objective == "squared_error";
        if (out.regression) {
            out.rmse = (*result_wire)[0];
            out.mae = (*result_wire)[1];
            out.held_out_rmse = (*result_wire)[4];
            out.held_out_mae = (*result_wire)[5];
        } else {
            out.metrics.auc = (*result_wire)[0];
            out.metrics.log_loss = (*result_wire)[1];
            out.metrics.accuracy = (*result_wire)[2];
            out.held_out.auc = (*result_wire)[4];
            out.held_out.log_loss = (*result_wire)[5];
            out.held_out.accuracy = (*result_wire)[6];
        }
        out.has_held_out = (*result_wire)[3] != 0.0;
        out.train_rows = static_cast<std::uint64_t>((*result_wire)[7]);
        out.held_out_rows = static_cast<std::uint64_t>((*result_wire)[8]);

        return {.ok = true,
                .detail = "gbdt over " +
                          std::to_string(round.participants().size()) +
                          " nodes -> " +
                          std::to_string(out.model.tree_count()) + " trees, " +
                          std::to_string(out.model.feature_count()) +
                          " features, " +
                          (out.regression
                               ? "rmse " + std::to_string(out.rmse)
                               : "auc " + std::to_string(out.metrics.auc)) +
                          (out.calibrated
                               ? ", calibrated (" +
                                     to_string(calibration.method) + ")"
                               : "")};
    });

    // Produce the Model: one logical record, registered by the root (it replicates
    // to peers). Each node still ran its own ComputeJob (recorded above).
    if (out.status == zetta::compute::JobStatus::Succeeded && out.is_root && node != nullptr) {
        ModelValue value;
        value.name = model_name.empty()
                         ? (out.regression ? "gbdt-squared-error"
                                           : "gbdt-binary-logistic")
                         : model_name;
        value.kind = out.regression ? "regressor" : "classifier";
        value.project = job.project();
        value.run = run_id.empty() ? job.run() : run_id;
        value.run_id = value.run;
        nlohmann::json drivers = nlohmann::json::array();
        for (std::size_t index = 0; index < feature_columns.size() &&
                                    index < out.feature_importances.size();
             ++index) {
            drivers.push_back({{"feature", feature_columns[index]},
                               {"importance", out.feature_importances[index]}});
        }
        value.feature_schema = {{"objective", params.objective},
                                {"features", feature_columns},
                                {"drivers", std::move(drivers)}};
        // The Model's `metrics` is what `system.models`, `get_model`, the chat
        // agent and the GUI all call "model quality" — and until #953 the three
        // numbers in it were measured on the rows the model trained on, with
        // nothing saying so. The legacy top-level keys stay put (every existing
        // model record has them, and they always meant in-sample); alongside them
        // go prefixed keys that NAME what each figure was measured on.
        //
        // **Every value here must be a number.** `model_schema()` declares
        // `metrics` as `additionalProperties: {type: number}`, so a nested object
        // or a string flag is rejected — and rejected at the RECEIVING peer, which
        // means the root registers the model, replication carries it, and it then
        // fails to materialize anywhere else. The symptom is a model that "does
        // not resolve on every member", nowhere near the code that wrote it.
        //
        // Which figure to quote is therefore answered by PRESENCE: a held-out
        // objective metric takes precedence over its in-sample counterpart. When
        // it is absent no evaluation holdout was asked for.
        nlohmann::json metrics = nlohmann::json::object();
        if (out.regression) {
            metrics["rmse"] = out.rmse;
            metrics["mae"] = out.mae;
            metrics["in_sample_rmse"] = out.rmse;
            metrics["in_sample_mae"] = out.mae;
        } else {
            metrics = out.metrics.to_json();
            metrics["in_sample_auc"] = out.metrics.auc;
            metrics["in_sample_log_loss"] = out.metrics.log_loss;
            metrics["in_sample_accuracy"] = out.metrics.accuracy;
        }
        metrics["in_sample_rows"] = out.train_rows;
        if (out.has_held_out) {
            if (out.regression) {
                metrics["held_out_rmse"] = out.held_out_rmse;
                metrics["held_out_mae"] = out.held_out_mae;
            } else {
                metrics["held_out_auc"] = out.held_out.auc;
                metrics["held_out_log_loss"] = out.held_out.log_loss;
                metrics["held_out_accuracy"] = out.held_out.accuracy;
            }
            metrics["held_out_rows"] = out.held_out_rows;
        }
        value.metrics = std::move(metrics);
        // Node-tree write: gated onto the owner thread (#624), so a train job
        // driven from a background-action worker registers the Model safely.
        std::shared_ptr<Model> model;
        node->run_on_owner([&] {
            model = node->create_model(std::move(value));
            if (model) model->set_weights(out.model.to_json().dump());
        });
        if (model) out.model_id = model->id();

        // The held-out scores are what decides between runs (#1239): log them on
        // the Run as well, one point at the last tree, so get_run_metrics'
        // summary and compare_runs carry them beside the training curves.
        // Before, only the Model and get_run_report had them. A run whose
        // metrics are suppressed for differential privacy gets none.
        if (out.has_held_out && !run_id.empty()) {
            std::map<std::string, double> held_out;
            if (out.regression) {
                held_out["held_out_rmse"] = out.held_out_rmse;
                held_out["held_out_mae"] = out.held_out_mae;
            } else {
                held_out["held_out_auc"] = out.held_out.auc;
                held_out["held_out_log_loss"] = out.held_out.log_loss;
                held_out["held_out_accuracy"] = out.held_out.accuracy;
            }
            held_out["held_out_rows"] = static_cast<double>(out.held_out_rows);
            std::erase_if(held_out, [](const auto& metric) {
                return !std::isfinite(metric.second);
            });
            node->run_on_owner([&] {
                const std::shared_ptr<pipeline::Run> run = node->find_run(run_id);
                if (!run || !run->value().metrics_suppressed.empty()) return;
                std::string error;
                const auto step = static_cast<std::uint64_t>(
                    std::max<std::size_t>(out.model.tree_count(), 1));
                // A failed write leaves the model and its report intact; the
                // run just lacks the summary entry, as before #1239.
                static_cast<void>(node->log_run_metrics(run_id, step, held_out,
                                                        {}, &error));
            });
        }
    }

    // Training fully completed — the model is durable as a Model entity, so the
    // resume checkpoints are no longer needed (each node clears its own store).
    if (out.status == zetta::compute::JobStatus::Succeeded && checkpoint.store != nullptr &&
        checkpoint.clear_on_success) {
        checkpoint.store->clear(tag);
    }
    return out;
}

}  // namespace zetta::ml
