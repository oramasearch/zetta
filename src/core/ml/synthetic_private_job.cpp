// SPDX-License-Identifier: MIT

#include "zetta/ml/synthetic_private_job.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kLaunchKind = "synthetic_private";

struct LocalRows {
    std::vector<nlohmann::json> rows;
    std::size_t partitions = 0;
    std::uint64_t bytes = 0;
    std::string error;
};

std::optional<std::string> read_blob(Node& node, const std::string& reference) {
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<data::Database> database =
            node.find_database(name);
        if (!database) {
            continue;
        }
        if (std::optional<std::string> body =
                database->large_objects().read(reference)) {
            return body;
        }
    }
    return std::nullopt;
}

LocalRows load_owned_rows(Node& node, const std::string& table_id) {
    LocalRows result;
    std::vector<std::shared_ptr<data::DatasetPartition>> owned;
    for (const std::shared_ptr<data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) {
            continue;
        }
        const std::vector<std::string>& owners = partition->owner_nodes();
        if (std::find(owners.begin(), owners.end(), node.node_id()) !=
            owners.end()) {
            owned.push_back(partition);
        }
    }
    std::sort(owned.begin(), owned.end(),
              [](const auto& left, const auto& right) {
                  return left->row_range().start < right->row_range().start;
              });
    for (const std::shared_ptr<data::DatasetPartition>& partition : owned) {
        if (partition->blob_ref().empty()) {
            continue;
        }
        const std::optional<std::string> body =
            read_blob(node, partition->blob_ref());
        if (!body) {
            result.error = "owned partition '" + partition->id() +
                           "' has no local body";
            return result;
        }
        data::PartitionBodyReadResult decoded =
            data::read_partition_body(*body, partition->id());
        if (!decoded.ok()) {
            result.error = decoded.error;
            return result;
        }
        result.bytes += body->size();
        ++result.partitions;
        for (data::PartitionRow& row : decoded.rows) {
            result.rows.push_back(std::move(row.doc));
        }
    }
    return result;
}

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

std::optional<SyntheticPrivateParams> parse_params(const nlohmann::json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    SyntheticPrivateParams params;
    params.epsilon = value.value("epsilon", params.epsilon);
    params.delta = value.value("delta", params.delta);
    params.maximum_epsilon =
        value.value("maximum_epsilon", params.maximum_epsilon);
    params.maximum_delta = value.value("maximum_delta", params.maximum_delta);
    const std::string noise = value.value("noise", std::string("gaussian"));
    if (noise != "gaussian" && noise != "laplace") {
        return std::nullopt;
    }
    params.noise = noise == "laplace" ? PrivacyNoise::Laplace
                                       : PrivacyNoise::Gaussian;
    params.clip_min = value.value("clip_min", params.clip_min);
    params.clip_max = value.value("clip_max", params.clip_max);
    params.synthetic_rows = value.value("synthetic_rows", params.synthetic_rows);
    params.seed = value.value("seed", params.seed);
    params.minimum_dcr = value.value("minimum_dcr", params.minimum_dcr);
    params.maximum_membership_auc =
        value.value("maximum_membership_auc", params.maximum_membership_auc);
    if (const auto gbdt = value.find("gbdt"); gbdt != value.end()) {
        const std::optional<GbdtParams> parsed = GbdtParams::from_json(*gbdt);
        if (!parsed) {
            return std::nullopt;
        }
        params.gbdt = *parsed;
    }
    return params;
}

compute::CoordinatedLaunchListener::LaunchOutcome run_launch(
    Node& node, const compute::CoordinatedLaunchRequest& request) {
    if (!request.payload.is_object() ||
        !request.payload.contains("table_id") ||
        !request.payload.contains("contract") ||
        !request.payload.contains("params") ||
        !request.payload.contains("round_tag")) {
        return {.ok = false, .detail = "invalid M-20 launch payload"};
    }
    std::string contract_error;
    const std::optional<data::DatasetContract> contract =
        data::dataset_contract_from_json(request.payload.at("contract"),
                                         &contract_error);
    const std::optional<SyntheticPrivateParams> params =
        parse_params(request.payload.at("params"));
    if (!contract || !params) {
        return {.ok = false,
                .detail = contract_error.empty()
                              ? "invalid M-20 launch parameters"
                              : contract_error};
    }
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        return {.ok = false, .detail = "M-20 launch has no compute coordinator"};
    }
    const std::shared_ptr<compute::ComputeJob> job =
        node.create_job(compute::JobKind::Train);
    if (!job) {
        return {.ok = false, .detail = "M-20 launch could not create a job"};
    }
    const DistributedSyntheticPrivateResult result = run_synthetic_private_job(
        *coordinator, node, *job,
        request.payload.at("table_id").get<std::string>(), *contract, *params,
        request.payload.at("round_tag").get<std::string>());
    return {.ok = result.ok(),
            .detail = result.ok() ? "M-20 participant completed"
                                  : result.learned.error};
}

}  // namespace

DistributedSyntheticPrivateResult run_synthetic_private_job(
    compute::JobCoordinator& coordinator, Node& node,
    compute::ComputeJob& job, const std::string& table_id,
    const data::DatasetContract& contract,
    const SyntheticPrivateParams& params, const std::string& round_tag,
    std::chrono::milliseconds timeout) {
    DistributedSyntheticPrivateResult result;
    const LocalRows local = load_owned_rows(node, table_id);
    result.partitions_read = local.partitions;
    result.rows_read = local.rows.size();
    result.local_bytes_read = local.bytes;
    const std::uint64_t wire_before = coordinator.collectives().wire_bytes();
    result.status = coordinator.run(
        job, [&](compute::JobRound& round) -> compute::JobCoordinator::Outcome {
            result.is_root = round.is_root();
            if (!local.error.empty()) {
                result.learned.error = local.error;
                return {.ok = false, .detail = local.error};
            }
            PrivateCollectives collectives;
            collectives.is_root = round.is_root();
            collectives.allreduce =
                [&](const std::string& name,
                    const std::vector<double>& values, GbdtReduce operation) {
                    const std::optional<std::vector<double>> reduced =
                        round.allreduce(name, values, collective_op(operation),
                                        timeout);
                    return reduced ? *reduced : std::vector<double>{};
                };
            collectives.gather =
                [&](const std::string& name,
                    const std::vector<double>& values) {
                    const std::optional<std::vector<std::vector<double>>> gathered =
                        round.gather(name, values, timeout);
                    return gathered ? *gathered
                                    : std::vector<std::vector<double>>{};
                };
            collectives.broadcast =
                [&](const std::string& name,
                    const std::vector<double>& values) {
                    const std::optional<std::vector<double>> shared =
                        round.broadcast(name, values, timeout);
                    return shared ? *shared : std::vector<double>{};
                };
            result.learned = run_synthetic_private(
                local.rows, contract, params, collectives, round_tag);
            return {.ok = result.learned.ok(),
                    .detail = result.learned.ok()
                                  ? "M-20 generated an evaluated synthetic table and DP model"
                                  : result.learned.error};
        });
    result.wire_bytes_moved =
        coordinator.collectives().wire_bytes() - wire_before;
    return result;
}

void install_synthetic_private_launch_runner(
    compute::CoordinatedLaunchListener& listener, Node& node) {
    listener.set_runner(std::string(kLaunchKind),
                        [&node](const compute::CoordinatedLaunchRequest& request) {
                            return run_launch(node, request);
                        });
}

}  // namespace zetta::ml
