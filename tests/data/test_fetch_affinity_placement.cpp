// SPDX-License-Identifier: MIT

// Fetch-affinity placement (#833 section 6): the node that fetched a slice owns
// the partitions it produced. The properties that matter are that placement is
// NOT a hash of the partition (nothing moves after the write), that it stays
// deterministic, and that a promoted (RF>1) dataset keeps the fetcher primary
// while spreading its replicas.
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/fetch_affinity_placement.hpp"
#include "zetta/data/rendezvous_placement.hpp"

namespace {

using zetta::data::FetchAffinityPlacement;
using zetta::data::PartitionId;
using zetta::data::RendezvousPlacement;

std::vector<std::string> roster() {
    return {"node-a", "node-b", "node-c", "node-d"};
}

TEST(FetchAffinityPlacement, EveryPartitionIsOwnedByTheFetchingNode) {
    const FetchAffinityPlacement placement(8, "node-c");
    for (PartitionId partition = 0; partition < 8; ++partition) {
        const std::vector<std::string> owners =
            placement.place_partition(partition, roster());
        ASSERT_EQ(owners.size(), 1u) << "partition " << partition;
        EXPECT_EQ(owners.front(), "node-c") << "partition " << partition;
    }
}

// The reason this partitioner exists: rendezvous placement would scatter the
// same partitions onto other nodes, forcing a shuffle of bytes already written.
TEST(FetchAffinityPlacement, DivergesFromRendezvousWhichIsWhyItExists) {
    const FetchAffinityPlacement affinity(16, "node-a");
    const RendezvousPlacement hashed(16, 1);
    std::size_t moved = 0;
    for (PartitionId partition = 0; partition < 16; ++partition) {
        if (hashed.place_partition(partition, roster()).front() !=
            affinity.place_partition(partition, roster()).front()) {
            ++moved;
        }
    }
    // With four nodes, ~3/4 of the partitions would have had to move.
    EXPECT_GT(moved, 8u);
}

TEST(FetchAffinityPlacement, PlacementIsIndependentOfMemberOrderAndEmptyRosters) {
    const FetchAffinityPlacement placement(4, "node-b");
    const std::vector<std::string> shuffled = {"node-d", "node-a", "node-c",
                                               "node-b"};
    for (PartitionId partition = 0; partition < 4; ++partition) {
        EXPECT_EQ(placement.place_partition(partition, roster()),
                  placement.place_partition(partition, shuffled));
        // A node that fetched a slice owns it even if the roster it was handed
        // is empty — it is the machine holding the body.
        const std::vector<std::string> alone =
            placement.place_partition(partition, {});
        ASSERT_EQ(alone.size(), 1u);
        EXPECT_EQ(alone.front(), "node-b");
    }
}

TEST(FetchAffinityPlacement, ReplicasKeepTheFetcherPrimaryAndSpreadTheRest) {
    const FetchAffinityPlacement placement(8, "node-a", /*replication_factor=*/3);
    std::set<std::string> replica_nodes;
    for (PartitionId partition = 0; partition < 8; ++partition) {
        const std::vector<std::string> owners =
            placement.place_partition(partition, roster());
        ASSERT_EQ(owners.size(), 3u) << "partition " << partition;
        EXPECT_EQ(owners.front(), "node-a");
        // No duplicates, and the fetcher is never also a replica.
        const std::set<std::string> unique(owners.begin(), owners.end());
        EXPECT_EQ(unique.size(), owners.size());
        replica_nodes.insert(owners[1]);
        replica_nodes.insert(owners[2]);
    }
    // The replicas actually spread rather than all landing on one peer.
    EXPECT_GE(replica_nodes.size(), 2u);
    EXPECT_EQ(replica_nodes.count("node-a"), 0u);
}

TEST(FetchAffinityPlacement, ClampsAFactorLargerThanTheRoster) {
    const FetchAffinityPlacement placement(4, "node-a", /*replication_factor=*/9);
    const std::vector<std::string> owners = placement.place_partition(0, roster());
    EXPECT_EQ(owners.size(), roster().size());
    EXPECT_EQ(owners.front(), "node-a");
}

TEST(FetchAffinityPlacement, SpreadsASliceEvenlyOverItsLocalPartitions) {
    const FetchAffinityPlacement placement(4, "node-a");
    std::vector<std::size_t> counts(4, 0);
    for (int ordinal = 0; ordinal < 40; ++ordinal) {
        const nlohmann::json record{{"_seq", ordinal}, {"value", ordinal * 2}};
        ++counts[placement.assign_partition(record)];
    }
    for (const std::size_t count : counts) EXPECT_EQ(count, 10u);
    // A row with no usable surrogate still lands somewhere in range.
    EXPECT_LT(placement.assign_partition(nlohmann::json{{"value", 1}}), 4u);
    EXPECT_LT(placement.assign_partition(nlohmann::json{{"_seq", -7}}), 4u);
}

TEST(FetchAffinityPlacement, ClampsADegeneratePartitionCount) {
    const FetchAffinityPlacement placement(0, "node-a");
    EXPECT_EQ(placement.partition_count(), 1u);
    EXPECT_EQ(placement.assign_partition(nlohmann::json{{"_seq", 5}}), 0u);
    EXPECT_STREQ(placement.name(), "fetch_affinity");
}

}  // namespace
