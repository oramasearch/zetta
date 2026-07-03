#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/partitioner.hpp"
#include "zetta/data/rendezvous_placement.hpp"

using zetta::data::Partitioner;
using zetta::data::PartitionId;
using zetta::data::RendezvousPlacement;
using zetta::data::rendezvous_owners;

namespace {

// A cluster of N distinct node ids (UUID-ish strings, like Cluster::nodes()).
std::vector<std::string> make_members(std::size_t count) {
    std::vector<std::string> members;
    members.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        members.push_back("node-" + std::to_string(index) + "-id");
    }
    return members;
}

// The primary owner of every partition in [0, P), under a given membership.
std::vector<std::string> primary_owners(const RendezvousPlacement& placement,
                                        const std::vector<std::string>& members) {
    std::vector<std::string> owners;
    owners.reserve(placement.partition_count());
    for (PartitionId partition = 0; partition < placement.partition_count();
         ++partition) {
        const auto placed = placement.place_partition(partition, members);
        owners.push_back(placed.empty() ? std::string{} : placed.front());
    }
    return owners;
}

}  // namespace

// ---- Basics --------------------------------------------------------------

TEST(RendezvousPlacement, NameAndCounts) {
    RendezvousPlacement placement{1024};
    EXPECT_STREQ(placement.name(), "rendezvous");
    EXPECT_EQ(placement.partition_count(), 1024u);
    EXPECT_EQ(placement.replication_factor(), 1u);
}

TEST(RendezvousPlacement, PartitionCountAndFactorClampToAtLeastOne) {
    RendezvousPlacement zeros{0, 0};
    EXPECT_EQ(zeros.partition_count(), 1u);
    EXPECT_EQ(zeros.replication_factor(), 1u);
}

TEST(RendezvousPlacement, AssignSpreadsAcrossPartitions) {
    RendezvousPlacement placement{64};
    std::set<PartitionId> seen;
    for (int id = 0; id < 1000; ++id) {
        const PartitionId partition =
            placement.assign_partition(nlohmann::json{{"id", id}});
        EXPECT_LT(partition, 64u);
        seen.insert(partition);
    }
    // A hash over 1000 records should touch most of the 64 partitions.
    EXPECT_GT(seen.size(), 50u);
}

TEST(RendezvousPlacement, AssignIsDeterministic) {
    RendezvousPlacement placement{32};
    const nlohmann::json record{{"customer", "acct_fedex_freight"}, {"value", 42}};
    EXPECT_EQ(placement.assign_partition(record),
              placement.assign_partition(record));
}

// ---- Placement: empties / clamps -----------------------------------------

TEST(RendezvousPlacement, NoMembersPlacesNowhere) {
    EXPECT_TRUE(rendezvous_owners(7, {}, 3).empty());
    RendezvousPlacement placement{16, 2};
    EXPECT_TRUE(placement.place_partition(0, {}).empty());
}

TEST(RendezvousPlacement, ReplicationFactorClampsToMemberCount) {
    const auto members = make_members(3);
    // Ask for more replicas than there are nodes → every node, no duplicates.
    const auto owners = rendezvous_owners(5, members, 10);
    EXPECT_EQ(owners.size(), 3u);
    std::set<std::string> unique(owners.begin(), owners.end());
    EXPECT_EQ(unique.size(), 3u);
}

TEST(RendezvousPlacement, ZeroFactorStillPlacesAPrimary) {
    const auto members = make_members(4);
    EXPECT_EQ(rendezvous_owners(9, members, 0).size(), 1u);
}

// ---- Placement: deterministic + order-independent ------------------------

TEST(RendezvousPlacement, DeterministicAndOrderIndependent) {
    auto members = make_members(5);
    RendezvousPlacement placement{256, 2};
    for (PartitionId partition = 0; partition < placement.partition_count();
         ++partition) {
        const auto owners = placement.place_partition(partition, members);
        // Shuffled membership yields the same ordered owners.
        std::vector<std::string> shuffled(members.rbegin(), members.rend());
        EXPECT_EQ(placement.place_partition(partition, shuffled), owners);
    }
}

TEST(RendezvousPlacement, ReplicasAreDistinctAndPrimaryFirst) {
    const auto members = make_members(6);
    for (PartitionId partition = 0; partition < 200; ++partition) {
        const auto owners = rendezvous_owners(partition, members, 3);
        ASSERT_EQ(owners.size(), 3u);
        std::set<std::string> unique(owners.begin(), owners.end());
        EXPECT_EQ(unique.size(), 3u) << "replicas must be distinct nodes";
        // The primary is the single highest-weight member.
        const auto single = rendezvous_owners(partition, members, 1);
        ASSERT_EQ(single.size(), 1u);
        EXPECT_EQ(owners.front(), single.front());
    }
}

// ---- Balance: partitions spread roughly evenly across nodes --------------

TEST(RendezvousPlacement, PrimariesAreRoughlyBalanced) {
    const std::size_t node_count = 5;
    const auto members = make_members(node_count);
    RendezvousPlacement placement{5000};

    std::map<std::string, int> counts;
    for (const std::string& owner : primary_owners(placement, members)) {
        ++counts[owner];
    }
    ASSERT_EQ(counts.size(), node_count);
    const double expected = 5000.0 / static_cast<double>(node_count);
    for (const auto& [node, count] : counts) {
        // Each node should own ~1/N of the partitions, within a loose band.
        EXPECT_GT(count, expected * 0.7) << node;
        EXPECT_LT(count, expected * 1.3) << node;
    }
}

// ---- The acceptance: a membership change moves only ~1/N partitions ------

TEST(RendezvousPlacement, AddingOneNodeReassignsAboutOneOverN) {
    const std::size_t node_count = 8;
    auto members = make_members(node_count);
    RendezvousPlacement placement{8192};

    const auto before = primary_owners(placement, members);

    // Add one node: membership goes from N to N+1.
    members.push_back("node-new-id");
    const auto after = primary_owners(placement, members);

    std::size_t moved = 0;
    for (PartitionId partition = 0; partition < placement.partition_count();
         ++partition) {
        if (before[partition] != after[partition]) ++moved;
    }
    const double fraction =
        static_cast<double>(moved) / placement.partition_count();
    // Expected ~1/(N+1) of partitions move (to the new node). Allow a band.
    const double expected = 1.0 / (node_count + 1);
    EXPECT_GT(fraction, expected * 0.6);
    EXPECT_LT(fraction, expected * 1.5);

    // Every partition that moved went *to* the new node, and nothing else
    // reshuffled between the pre-existing nodes (the HRW stability guarantee).
    for (PartitionId partition = 0; partition < placement.partition_count();
         ++partition) {
        if (before[partition] != after[partition]) {
            EXPECT_EQ(after[partition], "node-new-id");
        }
    }
}

TEST(RendezvousPlacement, RemovingOneNodeReassignsOnlyItsPartitions) {
    const std::size_t node_count = 8;
    auto members = make_members(node_count);
    RendezvousPlacement placement{8192};

    const auto before = primary_owners(placement, members);

    // Remove one node (node-3-id).
    const std::string removed = "node-3-id";
    members.erase(std::remove(members.begin(), members.end(), removed),
                  members.end());
    const auto after = primary_owners(placement, members);

    std::size_t moved = 0;
    for (PartitionId partition = 0; partition < placement.partition_count();
         ++partition) {
        if (before[partition] != after[partition]) ++moved;
        // A partition only changes owner if the removed node owned it before.
        if (before[partition] != after[partition]) {
            EXPECT_EQ(before[partition], removed);
        }
        // No partition is reassigned away from a node that is still present.
        if (before[partition] != removed) {
            EXPECT_EQ(after[partition], before[partition]);
        }
    }
    const double fraction =
        static_cast<double>(moved) / placement.partition_count();
    const double expected = 1.0 / node_count;  // the removed node's ~1/N share
    EXPECT_GT(fraction, expected * 0.6);
    EXPECT_LT(fraction, expected * 1.5);
}

// ---- Usable through the base seam pointer ---------------------------------

TEST(RendezvousPlacement, WorksThroughTheBasePointer) {
    std::shared_ptr<Partitioner> seam =
        std::make_shared<RendezvousPlacement>(128, 2);
    EXPECT_STREQ(seam->name(), "rendezvous");
    EXPECT_EQ(seam->partition_count(), 128u);
    const auto members = make_members(4);
    EXPECT_EQ(seam->place_partition(0, members).size(), 2u);
    EXPECT_LT(seam->assign_partition(nlohmann::json{{"k", "v"}}), 128u);
}
