#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/data_frame_ops.hpp"
#include "zetta/ddf/distributed_sort.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/util/uuid.hpp"

// DDF distributed sort (#385): sample-partition + local sort + merge across the
// cluster. The acceptance is correct sorted output single- AND multi-node, which
// these tests exercise over real in-process loopback clusters (the same shape as
// test_collectives.cpp / test_distributed_scheduler.cpp), plus the partition
// shuffle message's JSON boundary.
namespace {

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::SortOrder;
using zetta::ddf::ColumnSchema;
using zetta::ddf::concat;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::decode_partition_message;
using zetta::ddf::DistributedSorter;
using zetta::ddf::encode_partition_message;
using zetta::ddf::load_data_frame;
using zetta::ddf::PartitionMessage;

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

zetta::JsonStore make_store(const std::vector<double>& keys) {
    zetta::JsonStore store;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        // id == key so we can confirm the payload follows the key through the sort.
        store.add("row-" + std::to_string(i),
                  {{"key", keys[i]}, {"id", static_cast<std::int64_t>(keys[i])}});
    }
    return store;
}

DataFrame load_shard(CpuDevice& device, const std::vector<double>& keys) {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "key", .dtype = DataType::Float64},
        ColumnSchema{.name = "id", .dtype = DataType::Int64},
    };
    auto result = load_data_frame(make_store(keys), schema, device);
    EXPECT_TRUE(result.ok()) << result.status.message;
    return std::move(result.frame);
}

std::vector<double> keys_of(const DataFrame& frame) {
    std::vector<double> out;
    EXPECT_TRUE(frame.to_host<double>("key", out).ok());
    return out;
}

// One node of a loopback cluster: a Cluster + its own CpuDevice + the sorter.
struct Node {
    Node(std::string node_id, std::uint16_t port,
         const std::vector<std::string>& seeds)
        : cluster(std::move(node_id), make_config(port, seeds)),
          sorter(cluster, device) {}

    static ClusterConfig make_config(std::uint16_t port,
                                     const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    Cluster cluster;
    CpuDevice device;
    DistributedSorter sorter;  // registers as the cluster handler (over collectives)
};

// Build + connect `ids.size()` nodes; each seeds from every earlier node so the
// cluster is fully connected (collectives assume fully-connected membership).
std::vector<std::unique_ptr<Node>> make_cluster(
    const std::vector<std::string>& ids, std::uint16_t base_port) {
    std::vector<std::unique_ptr<Node>> nodes;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        nodes.push_back(std::make_unique<Node>(
            ids[index], static_cast<std::uint16_t>(base_port + index), seeds));
    }
    for (auto& node : nodes) node->cluster.start();
    return nodes;
}

bool all_connected(const std::vector<std::unique_ptr<Node>>& nodes) {
    const std::size_t expected = nodes.size() - 1;
    return wait_for([&] {
        return std::all_of(nodes.begin(), nodes.end(), [&](const auto& node) {
            return node->cluster.peer_count() == expected;
        });
    });
}

void stop_all(std::vector<std::unique_ptr<Node>>& nodes) {
    for (auto& node : nodes) node->cluster.stop();
}

// --- partition message JSON boundary -----------------------------------------

TEST(PartitionMessageJson, RoundTrips) {
    PartitionMessage message;
    message.tag = "round-1.shuffle";
    message.from = "node-a";
    message.rows = 2;
    message.columns = {
        {.name = "key", .dtype = DataType::Float64, .values = {1.5, -2.0}},
        {.name = "id", .dtype = DataType::Int64, .values = {7.0, 8.0}},
    };
    const auto parsed = decode_partition_message(encode_partition_message(message));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->tag, "round-1.shuffle");
    EXPECT_EQ(parsed->from, "node-a");
    EXPECT_EQ(parsed->rows, 2u);
    ASSERT_EQ(parsed->columns.size(), 2u);
    EXPECT_EQ(parsed->columns[0].name, "key");
    EXPECT_EQ(parsed->columns[0].dtype, DataType::Float64);
    EXPECT_EQ(parsed->columns[0].values, (std::vector<double>{1.5, -2.0}));
    EXPECT_EQ(parsed->columns[1].dtype, DataType::Int64);
}

TEST(PartitionMessageJson, RejectsBadInput) {
    std::string error;
    // not JSON
    EXPECT_FALSE(decode_partition_message("{not json", &error).has_value());
    // missing required `columns`
    EXPECT_FALSE(
        decode_partition_message(R"({"tag":"t","rows":0})", &error).has_value());
    // unknown dtype (schema enum)
    EXPECT_FALSE(decode_partition_message(
                     R"({"tag":"t","rows":1,"columns":[{"name":"k","dtype":"float16","values":[1]}]})",
                     &error)
                     .has_value());
    // value count disagrees with rows (the cross-field rule)
    EXPECT_FALSE(decode_partition_message(
                     R"({"tag":"t","rows":2,"columns":[{"name":"k","dtype":"int32","values":[1]}]})",
                     &error)
                     .has_value());
    // unknown top-level field
    EXPECT_FALSE(decode_partition_message(
                     R"({"tag":"t","rows":0,"columns":[],"extra":1})", &error)
                     .has_value());
}

// --- single node -------------------------------------------------------------

TEST(DistributedSort, SingleNodeReturnsLocallySortedShard) {
    Cluster cluster{zetta::new_uuid(), ClusterConfig{}};
    CpuDevice device;
    DistributedSorter sorter{cluster, device};
    cluster.start();

    const DataFrame shard = load_shard(device, {5.0, 1.0, 9.0, 3.0, 2.0});
    auto result = sorter.sort(shard, "key", SortOrder::Ascending, "solo");
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(keys_of(result.partition),
              (std::vector<double>{1.0, 2.0, 3.0, 5.0, 9.0}));

    cluster.stop();
}

// --- multi node: the acceptance ----------------------------------------------

// Run sort() on every node concurrently (SPMD), collect each node's partition in
// participant (node-id) order, and return their keys ([rank][row]).
std::vector<std::vector<double>> run_distributed_sort(
    std::vector<std::unique_ptr<Node>>& nodes,
    const std::vector<std::vector<double>>& shards_keys, SortOrder order,
    const std::string& round_tag) {
    std::vector<DataFrame> shards;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        shards.push_back(load_shard(nodes[index]->device, shards_keys[index]));
    }
    std::vector<DistributedSorter::Result> results(nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        threads.emplace_back([&, index] {
            results[index] = nodes[index]->sorter.sort(shards[index], "key", order,
                                                       round_tag,
                                                       std::chrono::seconds(10));
        });
    }
    for (auto& thread : threads) thread.join();

    // Order partitions by participant id (== the node ids we assigned, sorted).
    std::vector<std::string> ids;
    for (auto& node : nodes) ids.push_back(node->cluster.node_id());
    std::vector<std::size_t> order_by_id(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) order_by_id[i] = i;
    std::sort(order_by_id.begin(), order_by_id.end(),
              [&](std::size_t a, std::size_t b) { return ids[a] < ids[b]; });

    std::vector<std::vector<double>> partitions;
    for (const std::size_t index : order_by_id) {
        EXPECT_TRUE(results[index].ok()) << results[index].status.message;
        partitions.push_back(keys_of(results[index].partition));
    }
    return partitions;
}

TEST(DistributedSort, TwoNodesProduceAGloballySortedResult) {
    auto nodes = make_cluster({"node-a", "node-b"}, 47100);
    ASSERT_TRUE(all_connected(nodes));

    // Scrambled across the two shards so the shuffle genuinely moves rows.
    const auto partitions = run_distributed_sort(
        nodes, {{5.0, 1.0, 9.0, 3.0}, {8.0, 0.0, 4.0, 2.0, 6.0, 7.0}},
        SortOrder::Ascending, "round-2");

    // Each partition is locally sorted, and partition r's keys all precede r+1's.
    std::vector<double> global;
    double previous_max = -1e18;
    for (const auto& partition : partitions) {
        EXPECT_TRUE(std::is_sorted(partition.begin(), partition.end()));
        if (!partition.empty()) {
            EXPECT_GE(partition.front(), previous_max);
            previous_max = partition.back();
        }
        global.insert(global.end(), partition.begin(), partition.end());
    }
    EXPECT_EQ(global,
              (std::vector<double>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));

    stop_all(nodes);
}

TEST(DistributedSort, ThreeNodesGloballySortedAndDescending) {
    auto nodes = make_cluster({"node-a", "node-b", "node-c"}, 47200);
    ASSERT_TRUE(all_connected(nodes));

    const auto partitions = run_distributed_sort(
        nodes, {{5.0, 12.0, 1.0}, {3.0, 7.0, 11.0, 2.0}, {8.0, 0.0, 4.0, 6.0, 9.0, 10.0}},
        SortOrder::Descending, "round-3");

    std::vector<double> global;
    double previous_min = 1e18;
    for (const auto& partition : partitions) {
        EXPECT_TRUE(std::is_sorted(partition.begin(), partition.end(),
                                   std::greater<double>{}));
        if (!partition.empty()) {
            EXPECT_LE(partition.front(), previous_min);
            previous_min = partition.back();
        }
        global.insert(global.end(), partition.begin(), partition.end());
    }
    EXPECT_EQ(global, (std::vector<double>{12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0}));

    stop_all(nodes);
}

TEST(DistributedSort, HandlesAnEmptyShardOnOneNode) {
    auto nodes = make_cluster({"node-a", "node-b"}, 47300);
    ASSERT_TRUE(all_connected(nodes));

    // node-a has all the data; node-b is empty. The result must still be correct
    // and node-b must end with a valid (possibly empty) partition.
    const auto partitions = run_distributed_sort(
        nodes, {{4.0, 1.0, 3.0, 2.0, 0.0}, {}}, SortOrder::Ascending, "round-empty");

    std::vector<double> global;
    for (const auto& partition : partitions) {
        EXPECT_TRUE(std::is_sorted(partition.begin(), partition.end()));
        global.insert(global.end(), partition.begin(), partition.end());
    }
    EXPECT_EQ(global, (std::vector<double>{0, 1, 2, 3, 4}));

    stop_all(nodes);
}

// --- the concat-merge helper view of the ordered partitions ------------------

// The distributed sort returns ordered partitions, one per node; concat() is the
// helper that merges (an in-order set of) partitions back into one sorted frame.
// Partitions on the same device merge directly into a globally-sorted frame — the
// single-place view of the distributed result.
TEST(DistributedSort, ConcatMergesOrderedPartitionsIntoOneSortedFrame) {
    CpuDevice device;
    // Two already-ordered, range-disjoint partitions (as the distributed sort
    // produces): [0,1,2] then [5,7,9]. concat in partition order is the full sort.
    auto lower = zetta::ddf::sort(load_shard(device, {2.0, 0.0, 1.0}), "key");
    auto upper = zetta::ddf::sort(load_shard(device, {9.0, 5.0, 7.0}), "key");
    ASSERT_TRUE(lower.ok() && upper.ok());

    auto merged = concat({&lower.frame, &upper.frame});
    ASSERT_TRUE(merged.ok()) << merged.status.message;
    EXPECT_EQ(keys_of(merged.frame), (std::vector<double>{0, 1, 2, 5, 7, 9}));
}

}  // namespace
