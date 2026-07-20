#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/node_metrics.hpp"
#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/connect/distributed_fetch.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_manifest.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_scheduler.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

// #833 — **the partition scheduler**. Partition count used to be
// `AddDatasetOptions::partition_count = 8`: eight because eight. This is the one
// place that decides how many partitions a dataset gets, how big each targets,
// and which nodes hold them.
//
// The properties asserted here:
//   * a manual count is honoured EXACTLY — auto-detect is the default, not the
//     only mode;
//   * auto-detect targets a partition SIZE, so a tiny dataset does not pay eight
//     footers and a huge one is not eight unreadable blobs, with the "every node
//     gets work" floor holding underneath;
//   * a nearly-full node is avoided;
//   * missing or stale telemetry degrades to an even spread, never to a failure
//     — and a node the coordinator knows NOTHING about must not out-rank every
//     node that reported (the bias that put a whole dataset on one node);
//   * the plan round-trips through its JSON boundary (it travels in the launch
//     payload now, because a capacity-aware plan is not derivable per node);
//   * a flexible source is fetched straight into the target shape.
//
// See docs/data/partition-scheduler.md.
namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::NodeMetrics;
using zetta::cluster::NodeTelemetrySample;
using zetta::cluster::ReplicationManager;
using zetta::data::AddDatasetOptions;
using zetta::data::AddDatasetResult;
using zetta::data::DefaultPartitionScheduler;
using zetta::data::PartitionPlan;
using zetta::data::PartitionRequest;
using zetta::data::PartitionSchedulerConfig;
using zetta::data::SourceLayout;
using zetta::data::SourceShape;
using zetta::data::add_dataset_rows;
using zetta::data::kDefaultTargetPartitionBytes;
using zetta::data::partition_plan_from_json;
using zetta::data::to_json;
using zetta::test::TempDir;

std::vector<std::string> roster(std::size_t count) {
    std::vector<std::string> members;
    for (std::size_t index = 0; index < count; ++index) {
        members.push_back("node-" + std::string(1, static_cast<char>('a' + index)));
    }
    return members;
}

// A healthy host: room on disk, an idle CPU, no link-utilization reading (which
// is what a real host usually reports — net_percent needs a link-capacity
// reference that is not portable).
NodeTelemetrySample healthy(const std::string& node, double storage_percent = 40.0) {
    NodeTelemetrySample sample;
    sample.node_id = node;
    sample.metrics.storage_available = true;
    sample.metrics.storage_percent = storage_percent;
    sample.metrics.storage_total_bytes = 1000;
    sample.metrics.storage_used_bytes =
        static_cast<std::uint64_t>(10.0 * storage_percent);
    sample.metrics.cpu_percent_available = true;
    sample.metrics.cpu_percent = 10.0;
    return sample;
}

// How many partitions each node is the PRIMARY of.
std::map<std::string, std::size_t> primaries(const PartitionPlan& plan) {
    std::map<std::string, std::size_t> counts;
    for (const std::vector<std::string>& owners : plan.owners) {
        if (!owners.empty()) ++counts[owners.front()];
    }
    return counts;
}

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

}  // namespace

// --- The count ---------------------------------------------------------------

TEST(PartitionScheduler, ManualOverrideIsHonouredExactly) {
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.members = roster(3);
    ask.row_count = 10'000'000;         // auto-detect would say something else
    ask.approx_bytes = 40ull << 30;     // …and so would the size target
    ask.partition_count = 37;
    const PartitionPlan plan = scheduler.plan(ask);
    EXPECT_EQ(plan.partition_count, 37u);
    EXPECT_EQ(plan.owners.size(), 37u);
    EXPECT_NE(plan.rationale.find("manual override"), std::string::npos)
        << plan.rationale;
}

TEST(PartitionScheduler, AutoDetectTargetsASizeAcrossTinyMediumAndHuge) {
    const DefaultPartitionScheduler scheduler;
    const std::size_t target = kDefaultTargetPartitionBytes;

    // TINY — 4 KB. The size target says one partition; the "every node gets
    // work" floor raises it to 2 per node. It must NOT be eight footers because
    // eight was the constant.
    PartitionRequest tiny;
    tiny.members = roster(3);
    tiny.row_count = 40;
    tiny.approx_bytes = 4096;
    const PartitionPlan tiny_plan = scheduler.plan(tiny);
    EXPECT_EQ(tiny_plan.partition_count, 6u) << tiny_plan.rationale;

    // …and a tiny dataset on ONE node is a single partition, not eight.
    PartitionRequest alone = tiny;
    alone.members = roster(1);
    EXPECT_EQ(scheduler.plan(alone).partition_count, 2u);

    // MEDIUM — 1 GiB over 3 nodes. Size decides: 1 GiB / 64 MiB = 16, which is
    // already above the 6-partition floor.
    PartitionRequest medium;
    medium.members = roster(3);
    medium.row_count = 4'000'000;
    medium.approx_bytes = 1ull << 30;
    const PartitionPlan medium_plan = scheduler.plan(medium);
    EXPECT_EQ(medium_plan.partition_count, (1ull << 30) / target)
        << medium_plan.rationale;

    // HUGE — 200 GiB. Still size-driven, still each partition ~the target, and
    // nowhere near the constant 8: 8 partitions of 200 GiB is 25 GiB a piece,
    // which no node decodes a row group of.
    PartitionRequest huge;
    huge.members = roster(5);
    huge.row_count = 800'000'000;
    huge.approx_bytes = 200ull << 30;
    const PartitionPlan huge_plan = scheduler.plan(huge);
    EXPECT_EQ(huge_plan.partition_count, (200ull << 30) / target)
        << huge_plan.rationale;
    const std::size_t bytes_each = (200ull << 30) / huge_plan.partition_count;
    EXPECT_GE(bytes_each, target / 2);
    EXPECT_LE(bytes_each, target * 2);

    // Monotone in size, which is the property that makes the policy defensible.
    EXPECT_LT(tiny_plan.partition_count, medium_plan.partition_count);
    EXPECT_LT(medium_plan.partition_count, huge_plan.partition_count);
}

TEST(PartitionScheduler, EveryNodeGetsWorkAndTheCeilingHolds) {
    const DefaultPartitionScheduler scheduler;

    // The floor: a small dataset on a big cluster still gives every node work.
    PartitionRequest wide;
    wide.members = roster(12);
    wide.row_count = 100'000;
    wide.approx_bytes = 1024;
    const PartitionPlan wide_plan = scheduler.plan(wide);
    EXPECT_GE(wide_plan.partition_count, 12u * 2);
    EXPECT_EQ(primaries(wide_plan).size(), 12u) << "a node with no work at all";

    // The ceiling: an absurd dataset does not put a million entries on the
    // replicate-to-all catalog.
    PartitionRequest absurd;
    absurd.members = roster(3);
    absurd.row_count = 100'000'000'000ull;
    absurd.approx_bytes = 100ull << 40;  // 100 TiB
    EXPECT_EQ(scheduler.plan(absurd).partition_count,
              zetta::data::kMaxPlannedPartitions);

    // Never more partitions than rows — a partition per row is the degenerate
    // end of "every node gets work" and buys nothing but footers.
    PartitionRequest three_rows;
    three_rows.members = roster(8);
    three_rows.row_count = 3;
    three_rows.approx_bytes = 90;
    EXPECT_EQ(scheduler.plan(three_rows).partition_count, 3u);
}

// --- Placement ---------------------------------------------------------------

TEST(PartitionScheduler, ANearlyFullNodeIsAvoided) {
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.members = roster(3);
    ask.row_count = 100'000;
    ask.approx_bytes = 1ull << 30;
    ask.replication_factor = 1;
    ask.telemetry = {healthy("node-a"), healthy("node-b"),
                     healthy("node-c", /*storage_percent=*/97.0)};

    const PartitionPlan plan = scheduler.plan(ask);
    const std::map<std::string, std::size_t> owned = primaries(plan);
    EXPECT_GT(owned.count("node-a") ? owned.at("node-a") : 0u, 0u);
    EXPECT_GT(owned.count("node-b") ? owned.at("node-b") : 0u, 0u);
    const std::size_t on_full = owned.count("node-c") ? owned.at("node-c") : 0;
    EXPECT_LT(on_full, plan.partition_count / 10)
        << "the nearly-full node took " << on_full << " of "
        << plan.partition_count << ": " << plan.rationale;
    EXPECT_NE(plan.rationale.find("nearly-full"), std::string::npos)
        << plan.rationale;

    // Avoided, not BANNED: at RF=2 it is still a legal replica target, because
    // banning it could leave the plan nowhere to put a replica.
    PartitionRequest replicated = ask;
    replicated.replication_factor = 3;
    const PartitionPlan spread = scheduler.plan(replicated);
    for (const std::vector<std::string>& owners : spread.owners) {
        EXPECT_EQ(owners.size(), 3u);
        EXPECT_EQ(std::set<std::string>(owners.begin(), owners.end()).size(), 3u)
            << "a partition listed the same owner twice";
    }
}

TEST(PartitionScheduler, HealthyNodesWithADisagreeingStorageReadingStaySpread) {
    // 60% vs 77% full are both "has room". Weighting by the free fraction would
    // rank them 0.40 vs 0.23 and skew the whole dataset onto the emptier disk;
    // storage is a gate with a taper, not a gradient of used-ness.
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.members = roster(2);
    ask.row_count = 1'000'000;
    ask.approx_bytes = 1ull << 30;
    ask.replication_factor = 1;
    ask.telemetry = {healthy("node-a", 60.0), healthy("node-b", 77.0)};

    const PartitionPlan plan = scheduler.plan(ask);
    const std::map<std::string, std::size_t> owned = primaries(plan);
    ASSERT_EQ(owned.size(), 2u);
    const double ratio = static_cast<double>(owned.at("node-a")) /
                         static_cast<double>(owned.at("node-b"));
    EXPECT_GT(ratio, 0.8) << plan.rationale;
    EXPECT_LT(ratio, 1.25) << plan.rationale;
}

TEST(PartitionScheduler, MissingTelemetryFallsBackToAnEvenDistribution) {
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.members = roster(4);
    ask.row_count = 1'000'000;
    ask.approx_bytes = 1ull << 30;
    ask.replication_factor = 1;
    // No samples at all — the exchange has a TTL, so this is also the STALE
    // case: a peer past its TTL simply is not in the snapshot.
    ASSERT_TRUE(ask.telemetry.empty());

    const PartitionPlan plan = scheduler.plan(ask);
    const std::map<std::string, std::size_t> owned = primaries(plan);
    ASSERT_EQ(owned.size(), 4u);
    const std::size_t even = plan.partition_count / 4;
    for (const auto& [node, count] : owned) {
        EXPECT_GE(count, even) << node;
        EXPECT_LE(count, even + 1) << node;
    }
    EXPECT_NE(plan.rationale.find("no live telemetry"), std::string::npos)
        << plan.rationale;
}

TEST(PartitionScheduler, AnUnmeasuredNodeDoesNotOutrankEveryMeasuredOne) {
    // The bias that put an entire dataset on one node: a measured node can only
    // ever score at or below neutral, so an unknown node scored 1.0 always won.
    // An unmeasured member is weighted like a TYPICAL one instead.
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.members = roster(3);
    ask.row_count = 1'000'000;
    ask.approx_bytes = 1ull << 30;
    ask.replication_factor = 1;
    ask.telemetry = {healthy("node-a", 77.0), healthy("node-b", 77.0)};
    // node-c has not gossiped a sample yet.

    const PartitionPlan plan = scheduler.plan(ask);
    const std::map<std::string, std::size_t> owned = primaries(plan);
    ASSERT_EQ(owned.size(), 3u) << plan.rationale;
    for (const auto& [node, count] : owned) {
        EXPECT_GT(count, plan.partition_count / 6) << node << ": " << plan.rationale;
    }
}

TEST(PartitionScheduler, NoMembersStillYieldsAUsablePlan) {
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.row_count = 100;
    ask.approx_bytes = 4096;
    const PartitionPlan plan = scheduler.plan(ask);
    EXPECT_GE(plan.partition_count, 1u);
    EXPECT_EQ(plan.owners.size(), plan.partition_count);
    for (const std::vector<std::string>& owners : plan.owners) {
        EXPECT_TRUE(owners.empty());
    }
}

TEST(PartitionScheduler, ACacheIsPlannedAtReplicationFactorOne) {
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.members = roster(5);
    ask.row_count = 1000;
    ask.approx_bytes = 1 << 20;
    ask.replication_factor = 3;
    ask.cache = true;  // a connector result set: re-fetchable from its source
    const PartitionPlan plan = scheduler.plan(ask);
    EXPECT_EQ(plan.replication_factor, 1u);
    for (const std::vector<std::string>& owners : plan.owners) {
        EXPECT_EQ(owners.size(), 1u);
    }
}

TEST(PartitionScheduler, ThePlanIsConfigurable) {
    PartitionSchedulerConfig config;
    config.target_bytes_per_partition = 16ull << 20;  // 16 MiB
    config.partitions_per_node = 4;
    const DefaultPartitionScheduler scheduler{config};

    PartitionRequest ask;
    ask.members = roster(2);
    ask.row_count = 1'000'000;
    ask.approx_bytes = 256ull << 20;
    const PartitionPlan plan = scheduler.plan(ask);
    EXPECT_EQ(plan.partition_count, 16u);  // 256 MiB / 16 MiB
    EXPECT_EQ(plan.target_bytes_per_partition, 16ull << 20);
}

// --- The JSON boundary (it travels now) --------------------------------------

TEST(PartitionScheduler, ThePlanRoundTripsThroughItsJsonBoundary) {
    const DefaultPartitionScheduler scheduler;
    PartitionRequest ask;
    ask.members = roster(3);
    ask.row_count = 500'000;
    ask.approx_bytes = 512ull << 20;
    ask.replication_factor = 2;
    ask.telemetry = {healthy("node-a"), healthy("node-b"), healthy("node-c", 95.0)};
    const PartitionPlan plan = scheduler.plan(ask);

    std::string error;
    const std::optional<PartitionPlan> parsed =
        partition_plan_from_json(to_json(plan), &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->partition_count, plan.partition_count);
    EXPECT_EQ(parsed->owners, plan.owners);
    EXPECT_EQ(parsed->replication_factor, plan.replication_factor);
    EXPECT_EQ(parsed->target_bytes_per_partition, plan.target_bytes_per_partition);
    EXPECT_EQ(parsed->target_rows_per_partition, plan.target_rows_per_partition);
    EXPECT_EQ(parsed->rationale, plan.rationale);
}

TEST(PartitionScheduler, AMalformedPlanIsRejectedAtTheBoundary) {
    std::string error;
    // owners short of the count — the one cross-field invariant a schema cannot
    // state, and the one that would silently under-place a dataset.
    EXPECT_FALSE(partition_plan_from_json(
        nlohmann::json{{"partition_count", 4},
                       {"owners", nlohmann::json::array({{"node-a"}})},
                       {"replication_factor", 1}},
        &error));
    EXPECT_NE(error.find("/owners"), std::string::npos) << error;
    EXPECT_FALSE(partition_plan_from_json(nlohmann::json{{"owners", nlohmann::json::array()},
                                                         {"replication_factor", 1}}));
    EXPECT_FALSE(partition_plan_from_json(
        nlohmann::json{{"partition_count", 0},
                       {"owners", nlohmann::json::array()},
                       {"replication_factor", 1}}));
    EXPECT_FALSE(partition_plan_from_json(nlohmann::json::array()));
}

// --- A flexible source is fetched into the target shape ----------------------

TEST(PartitionScheduler, AFlexibleSourceIsTiledIntoThePlansShape) {
    // The plan gives node-a four of six partitions; the fetch must hand node-a
    // roughly four sixths of the ROWS, so the bodies land the size the scheduler
    // targeted and there is nothing to rebalance afterwards.
    PartitionPlan plan;
    plan.partition_count = 6;
    plan.replication_factor = 1;
    plan.owners = {{"node-a"}, {"node-a"}, {"node-a"}, {"node-a"},
                   {"node-b"}, {"node-c"}};

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    EXPECT_EQ(plan.fetch_weights(members), (std::vector<double>{4.0, 1.0, 1.0}));

    // 13 equal source chunks: index 0 arrives inline with the submission, 1..12
    // are tiled. 12 chunks over shares 4:1:1 is 8:2:2.
    const std::vector<std::uint64_t> chunk_rows(13, 100);
    const std::vector<zetta::connect::PartitionAssignment> tiling =
        zetta::connect::plan_fetch_assignments(members, chunk_rows,
                                               plan.fetch_weights(members));
    ASSERT_EQ(tiling.size(), 3u);
    EXPECT_EQ(tiling[0].partitions.size(), 8u);
    EXPECT_EQ(tiling[1].partitions.size(), 2u);
    EXPECT_EQ(tiling[2].partitions.size(), 2u);

    // Disjoint and total over [1, 13) — no chunk fetched twice, none missed.
    std::set<std::size_t> seen;
    for (const auto& assignment : tiling) {
        for (const std::size_t chunk : assignment.partitions) {
            EXPECT_TRUE(seen.insert(chunk).second) << "chunk " << chunk << " twice";
            EXPECT_GE(chunk, 1u);
            EXPECT_LT(chunk, 13u);
        }
    }
    EXPECT_EQ(seen.size(), 12u);

    // No weights = the previous behaviour exactly: an even, row-balanced tiling.
    const std::vector<zetta::connect::PartitionAssignment> even =
        zetta::connect::plan_fetch_assignments(members, chunk_rows);
    for (const auto& assignment : even) EXPECT_EQ(assignment.partitions.size(), 4u);
}

// --- Wired into the write path ------------------------------------------------

TEST(PartitionScheduler, AddDatasetUsesThePlanAndReportsIt) {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    Node node{config_for(dir), &replication};
    node.start();

    std::vector<nlohmann::json> rows;
    for (int index = 0; index < 200; ++index) {
        rows.push_back(nlohmann::json{{"id", index}, {"amount", index * 3}});
    }

    // Auto: a 200-row dataset on one node is one partition's worth of bytes, so
    // the per-node floor decides — NOT the old constant 8.
    AddDatasetOptions options;
    options.table_name = "auto";
    const AddDatasetResult automatic = add_dataset_rows(node, rows, options);
    ASSERT_TRUE(automatic.ok()) << automatic.error;
    EXPECT_EQ(automatic.plan.partition_count, 2u) << automatic.plan.rationale;
    EXPECT_EQ(automatic.partitions.size(), 2u);
    EXPECT_FALSE(automatic.plan.rationale.empty());
    EXPECT_GT(automatic.plan.target_bytes_per_partition, 0u);

    // Manual: honoured exactly, through the same path.
    AddDatasetOptions manual;
    manual.table_name = "manual";
    manual.partition_count = 5;
    const AddDatasetResult overridden = add_dataset_rows(node, rows, manual);
    ASSERT_TRUE(overridden.ok()) << overridden.error;
    EXPECT_EQ(overridden.plan.partition_count, 5u);
    EXPECT_EQ(overridden.partitions.size(), 5u);

    // The plan and the manifest agree, so the completeness check (#837) is
    // checked against what the scheduler actually decided.
    ASSERT_TRUE(overridden.table->partition_manifest());
    EXPECT_EQ(overridden.table->partition_manifest()->partitions,
              overridden.plan.partition_count);

    node.stop();
}

TEST(PartitionScheduler, ThePlansCountSurvivesARestartThroughTheManifest) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::uint32_t planned = 0;

    {
        ReplicationManager replication{node_id};
        Node node{config_for(dir), &replication};
        node.start();
        std::vector<nlohmann::json> rows;
        for (int index = 0; index < 120; ++index) {
            rows.push_back(nlohmann::json{{"id", index}});
        }
        AddDatasetOptions options;
        options.table_name = "planned";
        options.partition_count = 7;
        const AddDatasetResult added = add_dataset_rows(node, rows, options);
        ASSERT_TRUE(added.ok()) << added.error;
        planned = added.plan.partition_count;
        EXPECT_EQ(planned, 7u);
        node.tick();
        node.stop();
    }

    ReplicationManager replication{node_id};
    Node node{config_for(dir), &replication};
    node.start();
    const auto database = node.find_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->find_table("planned");
    ASSERT_TRUE(table);
    ASSERT_TRUE(table->partition_manifest());
    EXPECT_EQ(table->partition_manifest()->partitions, planned);
    node.stop();
}
