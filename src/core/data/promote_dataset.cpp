// SPDX-License-Identifier: MIT

#include "zetta/data/promote_dataset.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/rendezvous_placement.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"

namespace zetta::data {
namespace {

// Read a partition's id within its table from the catalog entry's free-form
// `stats` (recorded by write_partitioned). Rendezvous placement is a function of
// that id, so an entry without one cannot be recomputed and is left alone.
bool partition_id_of(const DatasetPartition& partition, PartitionId& out) {
    const nlohmann::json& stats = partition.stats();
    const auto found = stats.find("partition");
    if (found == stats.end() || !found->is_number_unsigned()) return false;
    out = found->get<PartitionId>();
    return true;
}

// The owner set a partition should have at `replication_factor`: its current
// owners first (they already hold the body), then the rendezvous order until the
// target is reached. Never shorter than what it already had.
std::vector<std::string> promoted_owners(const std::vector<std::string>& current,
                                         PartitionId partition,
                                         const std::vector<std::string>& members,
                                         std::size_t replication_factor) {
    std::vector<std::string> owners = current;
    if (owners.size() >= replication_factor) return owners;
    for (const std::string& candidate :
         rendezvous_owners(partition, members, replication_factor)) {
        if (owners.size() >= replication_factor) break;
        if (std::find(owners.begin(), owners.end(), candidate) == owners.end()) {
            owners.push_back(candidate);
        }
    }
    return owners;
}

}  // namespace

PromoteDatasetResult promote_dataset(Node& node, const std::string& database_name,
                                     const std::string& table_name,
                                     std::size_t replication_factor,
                                     const std::vector<std::string>& members) {
    PromoteDatasetResult result;
    result.database = database_name;
    result.table = table_name;

    if (replication_factor == 0) {
        result.error = "replication factor must be at least 1";
        return result;
    }
    const std::shared_ptr<Database> database = node.find_database(database_name);
    if (!database) {
        result.error = "database '" + database_name + "' does not exist";
        return result;
    }
    const std::shared_ptr<Table> table = database->find_table(table_name);
    if (!table) {
        result.error = "table '" + database_name + "." + table_name +
                       "' does not exist";
        return result;
    }
    result.table_id = table->id();

    // The roster placement is computed over: the caller's, else the live
    // membership, else this node alone (a standalone node can still "promote",
    // it just has nowhere to add a copy).
    std::vector<std::string> roster = members;
    if (roster.empty()) roster = node.cluster_members();
    if (roster.empty()) roster = {node.node_id()};
    result.replication_factor = std::min(replication_factor, roster.size());

    std::vector<std::shared_ptr<DatasetPartition>> placed;
    for (const std::shared_ptr<DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table->id()) continue;
        if (partition->blob_ref().empty()) continue;  // unplaced / empty
        placed.push_back(partition);
    }
    if (placed.empty()) {
        result.error = "dataset '" + database_name + "." + table_name +
                       "' has no placed partitions to replicate";
        return result;
    }
    result.partitions = placed.size();

    for (const std::shared_ptr<DatasetPartition>& partition : placed) {
        PartitionId partition_id = 0;
        if (!partition_id_of(*partition, partition_id)) continue;
        const std::vector<std::string> current = partition->owner_nodes();
        std::vector<std::string> desired = promoted_owners(
            current, partition_id, roster, result.replication_factor);
        if (desired == current) continue;
        // Capturing the new owner list IS the copy: the added owner sees the
        // body in missing_blobs() and fetches it over the large-object
        // transport, and every node's catalog agrees on who holds it.
        partition->set_owner_nodes(desired);
        ++result.promoted;
        result.promotions.push_back(PartitionPromotion{
            partition->id(), partition_id, current, std::move(desired)});
    }
    return result;
}

}  // namespace zetta::data
