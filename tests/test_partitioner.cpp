#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/partitioner.hpp"
#include "zetta/table.hpp"

#include "temp_dir.hpp"

using zetta::Partitioner;
using zetta::PartitionId;
using zetta::SinglePartitioner;
using zetta::Table;

namespace {

// A custom, non-trivial partitioner — proves the seam is genuinely pluggable
// without depending on the concrete strategies that land in later slices
// (HashPartitioner / RoundRobinPartitioner #513, RendezvousPlacement #514).
//
//   record → partition: an integer `shard` field, modulo P (a stand-in for a
//                       hash-of-key); a missing/non-integer field → partition 0.
//   partition → node:   a single deterministic owner, sorted(members)[p % count]
//                       (a stand-in for rendezvous placement).
class ModuloPartitioner final : public Partitioner {
public:
    explicit ModuloPartitioner(PartitionId partition_count)
        : partition_count_(partition_count) {}

    const char* name() const override { return "modulo"; }

    PartitionId partition_count() const override { return partition_count_; }

    PartitionId assign_partition(const nlohmann::json& record) const override {
        if (!record.is_object() || !record.contains("shard") ||
            !record.at("shard").is_number_integer()) {
            return 0;
        }
        const std::int64_t shard = record.at("shard").get<std::int64_t>();
        const std::int64_t bucket = ((shard % partition_count_) + partition_count_) %
                                    partition_count_;
        return static_cast<PartitionId>(bucket);
    }

    std::vector<std::string> place_partition(
        PartitionId partition,
        const std::vector<std::string>& members) const override {
        if (members.empty()) return {};
        std::vector<std::string> sorted = members;
        std::sort(sorted.begin(), sorted.end());
        return {sorted[partition % sorted.size()]};
    }

private:
    PartitionId partition_count_;
};

}  // namespace

// ---- SinglePartitioner: the trivial baseline -----------------------------

TEST(SinglePartitioner, IsOnePartition) {
    SinglePartitioner partitioner;
    EXPECT_STREQ(partitioner.name(), "single");
    EXPECT_EQ(partitioner.partition_count(), 1u);
}

TEST(SinglePartitioner, EverythingLandsInPartitionZero) {
    SinglePartitioner partitioner;
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"id", "a"}}), 0u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"id", "b"}}), 0u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json::array()), 0u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json(nullptr)), 0u);
}

TEST(SinglePartitioner, EveryMemberOwnsTheSinglePartition) {
    SinglePartitioner partitioner;
    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    EXPECT_EQ(partitioner.place_partition(0, members), members);
}

TEST(SinglePartitioner, NoMembersPlacesNowhere) {
    SinglePartitioner partitioner;
    EXPECT_TRUE(partitioner.place_partition(0, {}).empty());
}

// ---- A custom impl through the seam --------------------------------------

TEST(Partitioner, CustomImplAssignsWithinRange) {
    const PartitionId partition_count = 4;
    ModuloPartitioner partitioner{partition_count};
    EXPECT_EQ(partitioner.partition_count(), partition_count);

    for (std::int64_t shard = 0; shard < 20; ++shard) {
        const PartitionId partition =
            partitioner.assign_partition(nlohmann::json{{"shard", shard}});
        EXPECT_LT(partition, partition_count);
        EXPECT_EQ(partition, static_cast<PartitionId>(shard % partition_count));
    }

    // A missing/odd record degrades to partition 0, never out of range.
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"id", "x"}}), 0u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json::array()), 0u);
}

TEST(Partitioner, CustomImplIsDeterministicAndOrderIndependent) {
    ModuloPartitioner partitioner{8};
    // Same partition, same members in any order → same single owner.
    const std::vector<std::string> ascending{"n1", "n2", "n3"};
    const std::vector<std::string> shuffled{"n3", "n1", "n2"};
    for (PartitionId partition = 0; partition < 8; ++partition) {
        const auto owners = partitioner.place_partition(partition, ascending);
        ASSERT_EQ(owners.size(), 1u);
        EXPECT_EQ(partitioner.place_partition(partition, shuffled), owners);
    }
}

TEST(Partitioner, CustomImplCanBeUsedThroughTheBasePointer) {
    std::shared_ptr<Partitioner> seam = std::make_shared<ModuloPartitioner>(3);
    EXPECT_STREQ(seam->name(), "modulo");
    EXPECT_EQ(seam->assign_partition(nlohmann::json{{"shard", 7}}), 1u);  // 7 % 3
}

// ---- A table can be configured with a partitioner ------------------------

TEST(Table, IsNotPartitionedByDefault) {
    zetta::test::TempDir dir;
    Table table{"tbl-id", "events", dir.path() / "events"};
    EXPECT_EQ(table.partitioner(), nullptr);
}

TEST(Table, CanBeConfiguredWithAPartitioner) {
    zetta::test::TempDir dir;
    Table table{"tbl-id", "events", dir.path() / "events"};

    table.set_partitioner(std::make_shared<SinglePartitioner>());
    ASSERT_NE(table.partitioner(), nullptr);
    EXPECT_STREQ(table.partitioner()->name(), "single");
    EXPECT_EQ(table.partitioner()->partition_count(), 1u);

    // Swapping in another partitioner replaces it; clearing it un-partitions.
    table.set_partitioner(std::make_shared<ModuloPartitioner>(16));
    ASSERT_NE(table.partitioner(), nullptr);
    EXPECT_STREQ(table.partitioner()->name(), "modulo");
    EXPECT_EQ(table.partitioner()->partition_count(), 16u);

    table.set_partitioner(nullptr);
    EXPECT_EQ(table.partitioner(), nullptr);
}
