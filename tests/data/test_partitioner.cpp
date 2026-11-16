#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"

#include "temp_dir.hpp"

using zetta::data::HashPartitioner;
using zetta::data::Partitioner;
using zetta::data::partitioner_from_json;
using zetta::data::PartitionId;
using zetta::data::RoundRobinPartitioner;
using zetta::data::SinglePartitioner;
using zetta::data::Table;

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

// ---- HashPartitioner: hash(key columns) % P ------------------------------

TEST(HashPartitioner, NameAndPartitionCount) {
    HashPartitioner partitioner{64, {"account_id"}};
    EXPECT_STREQ(partitioner.name(), "hash");
    EXPECT_EQ(partitioner.partition_count(), 64u);
    EXPECT_EQ(partitioner.key_columns(), std::vector<std::string>{"account_id"});
}

TEST(HashPartitioner, ZeroPartitionCountIsClampedToOne) {
    HashPartitioner partitioner{0, {"account_id"}};
    EXPECT_EQ(partitioner.partition_count(), 1u);
    // No mod-by-zero; everything lands in the single partition.
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"account_id", "x"}}), 0u);
}

TEST(HashPartitioner, IsDeterministicForAGivenKey) {
    HashPartitioner partitioner{128, {"account_id"}};
    const nlohmann::json record{{"account_id", "acct_contoso_freight"}, {"arr", 90000}};
    const PartitionId first = partitioner.assign_partition(record);
    // Same key → same partition, every time and regardless of other fields.
    EXPECT_EQ(partitioner.assign_partition(record), first);
    EXPECT_EQ(
        partitioner.assign_partition(nlohmann::json{{"account_id", "acct_contoso_freight"},
                                                    {"arr", 5}}),
        first);
    // A second HashPartitioner with the same config agrees (stable across
    // instances — the property durable placement needs).
    HashPartitioner same{128, {"account_id"}};
    EXPECT_EQ(same.assign_partition(record), first);
}

TEST(HashPartitioner, AlwaysInRange) {
    HashPartitioner partitioner{16, {"account_id"}};
    for (int i = 0; i < 1000; ++i) {
        const PartitionId partition = partitioner.assign_partition(
            nlohmann::json{{"account_id", "acct-" + std::to_string(i)}});
        EXPECT_LT(partition, 16u);
    }
}

TEST(HashPartitioner, IsBalancedForASampleKey) {
    // Acceptance: hash is balanced for a sample key. Hash 10k distinct keys into
    // P=32 partitions and assert no partition is wildly over/under the 312.5 mean.
    const PartitionId partition_count = 32;
    const int sample = 10000;
    HashPartitioner partitioner{partition_count, {"account_id"}};
    std::map<PartitionId, int> counts;
    for (int i = 0; i < sample; ++i) {
        counts[partitioner.assign_partition(
            nlohmann::json{{"account_id", "acct-" + std::to_string(i)}})]++;
    }
    // Every partition got some rows, and each is within +/-40% of the mean — a
    // generous band that a uniform hash clears comfortably while a degenerate
    // (all-same-bucket) hash would fail.
    EXPECT_EQ(counts.size(), partition_count);
    const double mean = static_cast<double>(sample) / partition_count;
    for (const auto& [partition, count] : counts) {
        EXPECT_GT(count, mean * 0.6) << "partition " << partition << " underfull";
        EXPECT_LT(count, mean * 1.4) << "partition " << partition << " overfull";
    }
}

TEST(HashPartitioner, MultiColumnKeyHashesTheCombination) {
    HashPartitioner partitioner{100, {"region", "tier"}};
    const PartitionId a =
        partitioner.assign_partition(nlohmann::json{{"region", "us"}, {"tier", "gold"}});
    const PartitionId b =
        partitioner.assign_partition(nlohmann::json{{"region", "us"}, {"tier", "silver"}});
    // A different combination generally hashes elsewhere (not guaranteed, but the
    // separator guards against the ("us","gold") vs ("usgo","ld") class of collision).
    EXPECT_EQ(a, partitioner.assign_partition(
                     nlohmann::json{{"region", "us"}, {"tier", "gold"}}));
    // A missing column hashes as null (deterministic), never out of range.
    const PartitionId c = partitioner.assign_partition(nlohmann::json{{"region", "us"}});
    EXPECT_LT(c, 100u);
    (void)b;
}

// ---- RoundRobinPartitioner: surrogate % P for even balance ----------------

TEST(RoundRobinPartitioner, NameAndPartitionCount) {
    RoundRobinPartitioner partitioner{8};
    EXPECT_STREQ(partitioner.name(), "round_robin");
    EXPECT_EQ(partitioner.partition_count(), 8u);
    EXPECT_EQ(partitioner.surrogate_field(), "_seq");
}

TEST(RoundRobinPartitioner, DistributesEvenlyAcrossP) {
    // Acceptance: round-robin distributes evenly across P. A monotonic surrogate
    // 0,1,2,… cycles through the partitions, so 800 rows over P=8 give exactly
    // 100 each — perfectly even regardless of any (absent) key.
    const PartitionId partition_count = 8;
    const int rows = 800;
    RoundRobinPartitioner partitioner{partition_count};
    std::map<PartitionId, int> counts;
    for (int seq = 0; seq < rows; ++seq) {
        const PartitionId partition =
            partitioner.assign_partition(nlohmann::json{{"_seq", seq}});
        EXPECT_EQ(partition, static_cast<PartitionId>(seq % partition_count));
        counts[partition]++;
    }
    EXPECT_EQ(counts.size(), partition_count);
    for (const auto& [partition, count] : counts) {
        EXPECT_EQ(count, rows / partition_count) << "partition " << partition;
    }
}

TEST(RoundRobinPartitioner, IsDeterministicAndSideEffectFree) {
    // The seam contract: a row's partition is a pure function of its surrogate,
    // NOT of call order (no hidden counter) — so the same record always lands in
    // the same partition, which is what makes a row's partition recomputable.
    RoundRobinPartitioner partitioner{5};
    const nlohmann::json record{{"_seq", 42}};
    const PartitionId expected = 42 % 5;
    for (int i = 0; i < 10; ++i) {
        // Interleave other records; the answer for `record` never drifts.
        partitioner.assign_partition(nlohmann::json{{"_seq", i}});
        EXPECT_EQ(partitioner.assign_partition(record), expected);
    }
}

TEST(RoundRobinPartitioner, CustomSurrogateField) {
    RoundRobinPartitioner partitioner{4, "row_index"};
    EXPECT_EQ(partitioner.surrogate_field(), "row_index");
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"row_index", 7}}), 3u);
    // The default field is now ignored.
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"_seq", 7}}), 0u);
}

TEST(RoundRobinPartitioner, NegativeSurrogateFoldsIntoRange) {
    RoundRobinPartitioner partitioner{4};
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"_seq", -1}}), 3u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"_seq", -4}}), 0u);
}

TEST(RoundRobinPartitioner, MissingOrNonIntegerSurrogateDegradesToZero) {
    RoundRobinPartitioner partitioner{8};
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"id", "x"}}), 0u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"_seq", "3"}}), 0u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json{{"_seq", 1.5}}), 0u);
    EXPECT_EQ(partitioner.assign_partition(nlohmann::json::array()), 0u);
}

// ---- JSON config boundary: to_json / partitioner_from_json ----------------

TEST(PartitionerConfig, SingleRoundTrips) {
    SinglePartitioner single;
    const nlohmann::json config = single.to_json();
    EXPECT_EQ(config, (nlohmann::json{{"strategy", "single"}, {"partition_count", 1}}));

    std::shared_ptr<Partitioner> rebuilt = partitioner_from_json(config);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_STREQ(rebuilt->name(), "single");
    EXPECT_EQ(rebuilt->partition_count(), 1u);
}

TEST(PartitionerConfig, HashRoundTrips) {
    HashPartitioner original{256, {"account_id", "region"}};
    const nlohmann::json config = original.to_json();
    EXPECT_EQ(config["strategy"], "hash");
    EXPECT_EQ(config["partition_count"], 256);

    std::shared_ptr<Partitioner> rebuilt = partitioner_from_json(config);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_STREQ(rebuilt->name(), "hash");
    EXPECT_EQ(rebuilt->partition_count(), 256u);
    // The reconstructed partitioner assigns identically to the original.
    const nlohmann::json record{{"account_id", "acct_globex"}, {"region", "us"}};
    EXPECT_EQ(rebuilt->assign_partition(record), original.assign_partition(record));
}

TEST(PartitionerConfig, RoundRobinRoundTrips) {
    RoundRobinPartitioner original{16, "row_index"};
    const nlohmann::json config = original.to_json();
    EXPECT_EQ(config["strategy"], "round_robin");
    EXPECT_EQ(config["surrogate_field"], "row_index");

    std::shared_ptr<Partitioner> rebuilt = partitioner_from_json(config);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_STREQ(rebuilt->name(), "round_robin");
    EXPECT_EQ(rebuilt->partition_count(), 16u);
    EXPECT_EQ(rebuilt->assign_partition(nlohmann::json{{"row_index", 17}}), 1u);
}

TEST(PartitionerConfig, RoundRobinDefaultsSurrogateField) {
    std::shared_ptr<Partitioner> rebuilt = partitioner_from_json(
        nlohmann::json{{"strategy", "round_robin"}, {"partition_count", 4}});
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_EQ(rebuilt->assign_partition(nlohmann::json{{"_seq", 5}}), 1u);
}

TEST(PartitionerConfig, RejectsMalformedConfig) {
    std::string error;
    // Not an object.
    EXPECT_EQ(partitioner_from_json(nlohmann::json::array(), &error), nullptr);
    EXPECT_FALSE(error.empty());
    // Unknown strategy.
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"strategy", "modulo"}}), nullptr);
    // Missing strategy.
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"partition_count", 4}}), nullptr);
    // partition_count < 1.
    EXPECT_EQ(partitioner_from_json(
                  nlohmann::json{{"strategy", "hash"},
                                 {"partition_count", 0},
                                 {"key_columns", {"id"}}}),
              nullptr);
    // partition_count not an integer.
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"strategy", "hash"},
                                                   {"partition_count", 4.5},
                                                   {"key_columns", {"id"}}}),
              nullptr);
    // hash without key_columns.
    EXPECT_EQ(partitioner_from_json(
                  nlohmann::json{{"strategy", "hash"}, {"partition_count", 4}}),
              nullptr);
    // hash with empty key_columns.
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"strategy", "hash"},
                                                   {"partition_count", 4},
                                                   {"key_columns", nlohmann::json::array()}}),
              nullptr);
    // hash with a duplicate / empty key column.
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"strategy", "hash"},
                                                   {"partition_count", 4},
                                                   {"key_columns", {"id", "id"}}}),
              nullptr);
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"strategy", "hash"},
                                                   {"partition_count", 4},
                                                   {"key_columns", {""}}}),
              nullptr);
    // round_robin without partition_count.
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"strategy", "round_robin"}}),
              nullptr);
    // round_robin with an empty surrogate_field.
    EXPECT_EQ(partitioner_from_json(nlohmann::json{{"strategy", "round_robin"},
                                                   {"partition_count", 4},
                                                   {"surrogate_field", ""}}),
              nullptr);
    // single with a partition_count != 1.
    EXPECT_EQ(partitioner_from_json(
                  nlohmann::json{{"strategy", "single"}, {"partition_count", 4}}),
              nullptr);
}

TEST(PartitionerConfig, ConfiguresATableThroughTheBoundary) {
    zetta::test::TempDir dir;
    Table table{"tbl-id", "events", dir.path() / "events"};
    table.set_partitioner(partitioner_from_json(
        nlohmann::json{{"strategy", "hash"},
                       {"partition_count", 64},
                       {"key_columns", {"account_id"}}}));
    ASSERT_NE(table.partitioner(), nullptr);
    EXPECT_STREQ(table.partitioner()->name(), "hash");
    EXPECT_EQ(table.partitioner()->partition_count(), 64u);
}
