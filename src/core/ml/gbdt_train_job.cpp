#include "zetta/ml/gbdt_train_job.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/collectives.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/data_type.hpp"
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
    std::chrono::milliseconds timeout) {
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

        // 2. Train, combining per-node statistics across the cluster via the
        // round's allreduce (the same boosting loop the single-node path runs).
        const AllReduce allreduce = [&](const std::string& round_name,
                                        const std::vector<double>& local,
                                        GbdtReduce op) -> std::vector<double> {
            const std::optional<std::vector<double>> reduced = round.allreduce(
                round_name, local, to_collective_op(op), timeout);
            return reduced ? *reduced : std::vector<double>{};
        };
        GbdtTrainResult trained = train_gbdt(columns, feature_columns, labels,
                                             params, allreduce, tag);
        if (!trained.ok()) {
            return {.ok = false, .detail = "gbdt train: " + trained.status.message};
        }
        out.model = std::move(trained.model);
        out.feature_importances = std::move(trained.feature_importances);

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
                          " features, auc " + std::to_string(out.metrics.auc)};
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
    return out;
}

}  // namespace zetta::ml
