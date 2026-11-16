// SPDX-License-Identifier: MIT

#include "zetta/ml/rank.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"

namespace zetta::ml {
namespace {

using compute::JobRound;
using Outcome = compute::JobCoordinator::Outcome;

cluster::CollectiveOp collective_op(GbdtReduce operation) {
    switch (operation) {
    case GbdtReduce::Sum:
        return cluster::CollectiveOp::Sum;
    case GbdtReduce::Min:
        return cluster::CollectiveOp::Minimum;
    case GbdtReduce::Max:
        return cluster::CollectiveOp::Maximum;
    }
    return cluster::CollectiveOp::Sum;
}

RankDataset select_rows(const RankDataset& dataset, const std::vector<std::size_t>& rows) {
    RankDataset selected;
    selected.feature_names = dataset.feature_names;
    selected.label_name = dataset.label_name;
    selected.entity_name = dataset.entity_name;
    selected.features.assign(dataset.features.size(), {});
    for (const std::size_t row : rows) {
        for (std::size_t feature = 0; feature < dataset.features.size(); ++feature) {
            selected.features[feature].push_back(dataset.features[feature][row]);
        }
        selected.labels.push_back(dataset.labels[row]);
        selected.groups.push_back(dataset.groups[row]);
        selected.entities.push_back(dataset.entities[row]);
    }
    return selected;
}

std::vector<double> score_rows(const GbdtModel& model, const RankDataset& dataset) {
    std::vector<double> scores;
    scores.reserve(dataset.row_count());
    std::vector<double> row(dataset.features.size(), 0.0);
    for (std::size_t index = 0; index < dataset.row_count(); ++index) {
        for (std::size_t feature = 0; feature < dataset.features.size(); ++feature) {
            row[feature] = dataset.features[feature][index];
        }
        scores.push_back(model.predict_raw(row));
    }
    return scores;
}

std::vector<double> local_groups_wire(const RankDataset& dataset) {
    std::set<std::int64_t> groups(dataset.groups.begin(), dataset.groups.end());
    std::vector<double> wire;
    wire.reserve(groups.size());
    for (const std::int64_t group : groups) {
        wire.push_back(static_cast<double>(group));
    }
    return wire;
}

std::vector<double> ranked_wire(const std::vector<std::vector<double>>& gathered) {
    struct ScoredRow {
        std::int64_t group;
        std::int64_t entity;
        double score;
        double relevance;
    };
    std::map<std::int64_t, std::vector<ScoredRow>> groups;
    for (const std::vector<double>& shard : gathered) {
        for (std::size_t index = 0; index + 3 < shard.size(); index += 4) {
            const std::int64_t group = static_cast<std::int64_t>(shard[index]);
            groups[group].push_back({group, static_cast<std::int64_t>(shard[index + 1]),
                                     shard[index + 2], shard[index + 3]});
        }
    }
    std::vector<double> wire;
    for (auto& [_, rows] : groups) {
        std::stable_sort(rows.begin(), rows.end(),
                         [](const ScoredRow& left, const ScoredRow& right) {
                             if (left.score != right.score) {
                                 return left.score > right.score;
                             }
                             return left.entity < right.entity;
                         });
        for (std::size_t position = 0; position < rows.size(); ++position) {
            wire.push_back(static_cast<double>(rows[position].group));
            wire.push_back(static_cast<double>(rows[position].entity));
            wire.push_back(rows[position].score);
            wire.push_back(rows[position].relevance);
            wire.push_back(static_cast<double>(position + 1));
        }
    }
    return wire;
}

} // namespace

DistributedRankResult
run_lambda_mart_job(compute::JobCoordinator& coordinator, compute::ComputeJob& job,
                    const RankDataset& local_dataset, const RankParams& params, Node* node,
                    const std::string& model_name, const std::string& round_tag,
                    std::chrono::milliseconds timeout) {
    DistributedRankResult output;
    output.status = coordinator.run(job, [&](JobRound& round) -> Outcome {
        output.is_root = round.is_root();
        if (params.group_column.empty()) {
            return {.ok = false, .detail = "rank: group column is required"};
        }

        const std::optional<std::vector<std::vector<double>>> gathered_groups =
            round.gather(round_tag + ".groups", local_groups_wire(local_dataset), timeout);
        if (!gathered_groups) {
            return {.ok = false, .detail = "rank: group ownership gather failed"};
        }
        std::vector<double> ownership_wire;
        if (!gathered_groups->empty()) {
            std::set<std::int64_t> seen;
            bool unique = true;
            for (const std::vector<double>& shard : *gathered_groups) {
                for (const double value : shard) {
                    if (!seen.insert(static_cast<std::int64_t>(value)).second) {
                        unique = false;
                    }
                }
            }
            ownership_wire.push_back(unique ? 1.0 : 0.0);
        }
        const std::optional<std::vector<double>> ownership =
            round.broadcast(round_tag + ".group_ownership", ownership_wire, timeout);
        if (!ownership || ownership->size() != 1 || (*ownership)[0] == 0.0) {
            return {.ok = false,
                    .detail = "rank: a group spans participants; repartition by group"};
        }

        const RankGroupSplit split =
            split_rank_groups(local_dataset.groups, params.holdout_fraction, params.seed);
        const RankDataset training = select_rows(local_dataset, split.training_rows);
        const RankDataset validation = select_rows(local_dataset, split.validation_rows);
        const std::optional<std::vector<double>> counts =
            round.allreduce(round_tag + ".counts",
                            {static_cast<double>(training.row_count()),
                             static_cast<double>(split.training_groups.size()),
                             static_cast<double>(validation.row_count())},
                            cluster::CollectiveOp::Sum, timeout);
        if (!counts || counts->size() != 3 || (*counts)[0] <= 0.0 || (*counts)[2] <= 0.0) {
            return {.ok = false, .detail = "rank: group split needs training and validation rows"};
        }
        output.train_rows = static_cast<std::uint64_t>((*counts)[0]);
        output.train_groups = static_cast<std::uint64_t>((*counts)[1]);

        const AllReduce allreduce = [&](const std::string& name, const std::vector<double>& local,
                                        GbdtReduce operation) {
            const std::optional<std::vector<double>> reduced =
                round.allreduce(name, local, collective_op(operation), timeout);
            return reduced ? *reduced : std::vector<double>{};
        };
        LambdaMartTrainResult trained =
            train_lambda_mart(training, params, allreduce, round_tag + ".trees");
        if (!trained.ok()) {
            return {.ok = false, .detail = "rank train: " + trained.status.message};
        }
        output.model = std::move(trained.model);
        output.feature_importances = std::move(trained.feature_importances);

        const std::vector<double> validation_scores = score_rows(output.model, validation);
        std::vector<double> validation_wire;
        validation_wire.reserve(validation.row_count() * 3);
        for (std::size_t row = 0; row < validation.row_count(); ++row) {
            validation_wire.push_back(static_cast<double>(validation.groups[row]));
            validation_wire.push_back(validation.labels[row]);
            validation_wire.push_back(validation_scores[row]);
        }
        const std::optional<std::vector<std::vector<double>>> metric_gather =
            round.gather(round_tag + ".metrics", validation_wire, timeout);
        if (!metric_gather) {
            return {.ok = false, .detail = "rank: metric gather failed"};
        }
        std::vector<double> metric_wire;
        if (!metric_gather->empty()) {
            std::vector<std::int64_t> groups;
            std::vector<double> labels;
            std::vector<double> scores;
            for (const std::vector<double>& shard : *metric_gather) {
                for (std::size_t index = 0; index + 2 < shard.size(); index += 3) {
                    groups.push_back(static_cast<std::int64_t>(shard[index]));
                    labels.push_back(shard[index + 1]);
                    scores.push_back(shard[index + 2]);
                }
            }
            const RankMetrics metrics = evaluate_ranking(scores, labels, groups, params.cutoff);
            metric_wire = {metrics.ndcg_at_k, metrics.yield_at_k,
                           static_cast<double>(metrics.groups), static_cast<double>(metrics.rows)};
        }
        const std::optional<std::vector<double>> metrics =
            round.broadcast(round_tag + ".metric_result", metric_wire, timeout);
        if (!metrics || metrics->size() != 4) {
            return {.ok = false, .detail = "rank: metric broadcast failed"};
        }
        output.validation.ndcg_at_k = (*metrics)[0];
        output.validation.yield_at_k = (*metrics)[1];
        output.validation.groups = static_cast<std::uint64_t>((*metrics)[2]);
        output.validation.rows = static_cast<std::uint64_t>((*metrics)[3]);

        const std::vector<double> scores = score_rows(output.model, local_dataset);
        std::vector<double> score_wire;
        score_wire.reserve(local_dataset.row_count() * 4);
        for (std::size_t row = 0; row < local_dataset.row_count(); ++row) {
            score_wire.push_back(static_cast<double>(local_dataset.groups[row]));
            score_wire.push_back(static_cast<double>(local_dataset.entities[row]));
            score_wire.push_back(scores[row]);
            score_wire.push_back(local_dataset.labels[row]);
        }
        const std::optional<std::vector<std::vector<double>>> score_gather =
            round.gather(round_tag + ".scores", score_wire, timeout);
        if (!score_gather) {
            return {.ok = false, .detail = "rank: score gather failed"};
        }
        std::vector<double> global_ranking;
        if (!score_gather->empty()) {
            global_ranking = ranked_wire(*score_gather);
        }
        const std::optional<std::vector<double>> rank_result =
            round.broadcast(round_tag + ".ranked", global_ranking, timeout);
        if (!rank_result || rank_result->size() % 5 != 0) {
            return {.ok = false, .detail = "rank: ranking broadcast failed"};
        }
        for (std::size_t index = 0; index < rank_result->size(); index += 5) {
            output.ranked.push_back({static_cast<std::int64_t>((*rank_result)[index]),
                                     static_cast<std::int64_t>((*rank_result)[index + 1]),
                                     (*rank_result)[index + 2], (*rank_result)[index + 3],
                                     static_cast<std::size_t>((*rank_result)[index + 4])});
        }

        return {.ok = true,
                .detail = "LambdaMART ranked " + std::to_string(output.ranked.size()) +
                          " rows in " + std::to_string(output.validation.groups) +
                          " validation groups"};
    });

    if (output.ok() && output.is_root && node != nullptr) {
        ModelValue value;
        value.name = model_name.empty() ? "lambdamart-rank" : model_name;
        value.kind = "rank";
        value.project = job.project();
        value.run = job.run();
        nlohmann::json drivers = nlohmann::json::array();
        for (std::size_t index = 0; index < local_dataset.feature_names.size() &&
                                    index < output.feature_importances.size();
             ++index) {
            drivers.push_back({{"feature", local_dataset.feature_names[index]},
                               {"importance", output.feature_importances[index]}});
        }
        value.feature_schema = {{"objective", "lambdamart"},
                                {"features", local_dataset.feature_names},
                                {"group_column", params.group_column},
                                {"cutoff", params.cutoff},
                                {"drivers", std::move(drivers)}};
        nlohmann::json excluded = nlohmann::json::array();
        excluded.push_back(params.group_column);
        if (!local_dataset.entity_name.empty()) {
            excluded.push_back(local_dataset.entity_name);
            value.feature_schema["entity_column"] = local_dataset.entity_name;
        }
        if (!local_dataset.label_name.empty()) {
            excluded.push_back(local_dataset.label_name);
            value.feature_schema["label_column"] = local_dataset.label_name;
        }
        value.feature_schema["excluded_from_features"] = std::move(excluded);
        value.metrics = {{"ndcg_at_k", output.validation.ndcg_at_k},
                         {"yield_at_k", output.validation.yield_at_k},
                         {"cutoff", params.cutoff},
                         {"held_out_groups", output.validation.groups},
                         {"held_out_rows", output.validation.rows},
                         {"train_groups", output.train_groups},
                         {"train_rows", output.train_rows}};
        std::shared_ptr<Model> model;
        node->run_on_owner([&] {
            model = node->create_model(std::move(value));
            if (model) {
                model->set_weights(output.model.to_json().dump());
            }
        });
        if (model) {
            output.model_id = model->id();
        }
    }
    return output;
}

} // namespace zetta::ml
