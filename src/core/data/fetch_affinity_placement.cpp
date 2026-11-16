// SPDX-License-Identifier: MIT

#include "zetta/data/fetch_affinity_placement.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "zetta/data/rendezvous_placement.hpp"

namespace zetta::data {
namespace {

// The surrogate the partitioned write path stamps on each row (its table
// ordinal). Kept in sync with RoundRobinPartitioner's default and with
// partition_writer.cpp's kSurrogateField.
constexpr const char* kSurrogateField = "_seq";

}  // namespace

FetchAffinityPlacement::FetchAffinityPlacement(PartitionId partition_count,
                                               std::string owner_node,
                                               std::size_t replication_factor)
    : partition_count_(partition_count == 0 ? 1 : partition_count),
      owner_node_(std::move(owner_node)),
      replication_factor_(std::max<std::size_t>(replication_factor, 1)) {}

PartitionId FetchAffinityPlacement::assign_partition(
    const nlohmann::json& record) const {
    // Round-robin over the stamped surrogate: an even split of this node's own
    // slice, independent of key skew. A record without a usable surrogate lands
    // in partition 0 — in range and deterministic, never a throw.
    if (!record.is_object() || !record.contains(kSurrogateField) ||
        !record.at(kSurrogateField).is_number_integer()) {
        return 0;
    }
    const std::int64_t surrogate = record.at(kSurrogateField).get<std::int64_t>();
    const std::int64_t modulus = static_cast<std::int64_t>(partition_count_);
    const std::int64_t bucket = ((surrogate % modulus) + modulus) % modulus;
    return static_cast<PartitionId>(bucket);
}

std::vector<std::string> FetchAffinityPlacement::place_partition(
    PartitionId partition, const std::vector<std::string>& members) const {
    // The fetching node is the primary owner, unconditionally: it is the node
    // whose disk the body was just written to. This is the whole point — no
    // hash gets to move it somewhere else.
    std::vector<std::string> owners{owner_node_};
    if (replication_factor_ <= 1) return owners;

    // Extra owners (a promoted result set) spread by rendezvous over everyone
    // else, so replica placement stays leaderless and identical on every node.
    std::vector<std::string> others;
    others.reserve(members.size());
    for (const std::string& member : members) {
        if (member != owner_node_) others.push_back(member);
    }
    for (std::string& replica :
         rendezvous_owners(partition, others, replication_factor_ - 1)) {
        owners.push_back(std::move(replica));
    }
    return owners;
}

}  // namespace zetta::data
