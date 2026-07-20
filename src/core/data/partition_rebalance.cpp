#include "zetta/data/partition_rebalance.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/data/rendezvous_placement.hpp"

namespace zetta::data {
namespace {

// The set of owners as a sorted set, for an order-independent "did ownership
// change" comparison (the catalog stores owners in placement order — primary
// first — but a move is about *which* nodes own it, not their order).
std::set<std::string> owner_set(const std::vector<std::string>& owners) {
    return std::set<std::string>(owners.begin(), owners.end());
}

// Read a partition's id (within its table) from the catalog entry's free-form
// `stats` (`{partition, rows}`, recorded by write_partitioned). Returns false when
// absent / not an unsigned integer — such an entry can't be HRW-recomputed, so the
// caller skips it.
bool partition_id_of(const DatasetPartition& partition, PartitionId& out) {
    const nlohmann::json& stats = partition.stats();
    const auto it = stats.find("partition");
    if (it == stats.end() || !it->is_number_unsigned()) return false;
    out = it->get<PartitionId>();
    return true;
}

}  // namespace

RebalanceResult rebalance_partitions(
    Node& node, const std::vector<std::string>& members,
    const std::vector<std::string>& previous_members,
    bool include_fetch_affinity) {
    RebalanceResult result;
    if (members.empty()) return result;  // nowhere to place

    const std::set<std::string> previous_set = owner_set(previous_members);

    for (const std::shared_ptr<DatasetPartition>& partition :
         node.dataset_partitions()) {
        const std::vector<std::string>& current = partition->owner_nodes();
        if (current.empty()) {  // unplaced — left to the replicate-to-all fallback
            ++result.skipped;
            continue;
        }
        const std::set<std::string> current_set = owner_set(current);

        // **Fetch-affinity partitions are skipped by the OPPORTUNISTIC trigger**
        // (#833 section 6). Their owner is the node whose disk holds the bytes,
        // chosen because it is the node that pulled them — recomputing a hash on
        // a membership change would order a shuffle of a dataset that was placed
        // precisely to avoid one, and the dataset is a cache at RF=1 anyway (an
        // owner that leaves means re-run the ingest, not re-place the entry).
        //
        // An EXPLICIT caller that means to re-shape what an ingest just wrote
        // passes include_fetch_affinity — otherwise the skip above and the
        // ingest's own rebalance would cancel each other out, which is exactly
        // the trap this parameter exists to avoid.
        if (!include_fetch_affinity && partition->stats().is_object() &&
            partition->stats().value("placement", std::string{}) ==
                "fetch_affinity") {
            ++result.skipped;
            continue;
        }

        // Replicate-to-all detection: a partition owned by exactly the previous
        // full membership is replicate-to-all (SinglePartitioner / Hash /
        // RoundRobin), not sharded — leave it untouched (growth is out of scope).
        if (!previous_set.empty() && current_set == previous_set) {
            ++result.skipped;
            continue;
        }

        PartitionId partition_id = 0;
        if (!partition_id_of(*partition, partition_id)) {
            ++result.skipped;  // no recorded partition id → can't HRW-recompute
            continue;
        }

        // Preserve the replication factor the catalog records (the owners the
        // partitioner placed), clamped to the live membership.
        const std::size_t replication_factor = std::min(
            std::max<std::size_t>(current.size(), 1), members.size());
        std::vector<std::string> desired =
            rendezvous_owners(partition_id, members, replication_factor);

        ++result.examined;
        if (owner_set(desired) == current_set) continue;  // already correct

        PartitionMove move;
        move.partition_uuid = partition->id();
        move.partition = partition_id;
        move.from = current;            // copy before the mutator overwrites it
        move.to = desired;              // copy before the move below
        partition->set_owner_nodes(std::move(desired));
        result.moves.push_back(std::move(move));
        ++result.moved;
    }

    return result;
}

}  // namespace zetta::data