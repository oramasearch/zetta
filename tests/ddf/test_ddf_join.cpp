// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/join.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

// The DDF distributed equi-join op (#382): repartition two frames by key via
// collectives, then a local hash / sort-merge join per shard. These tests cover
// the pure pieces (local joins, serialize/partition boundary) without a cluster,
// and the headline acceptance: a single-node and a multi-node join produce
// identical results, with collectives doing the repartition.
namespace {

using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::JobCoordinator;
using zetta::compute::JobRound;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::HostRelation;
using zetta::ddf::JoinAlgorithm;
using zetta::ddf::JoinedRows;
using zetta::ddf::load_data_frame;
using zetta::test::TempDir;
using Json = nlohmann::json;

// --- Fixture relations -------------------------------------------------------
//
// A churn-flavored join: accounts (account_id -> tier) inner-joined with usage
// events (account_id -> amount) on account_id. Accounts 1,2,3,5 have events
// (some more than one); account 4 has none; event for account 9 has no account.
// The inner join is the 6 matched pairs below — independent of how the rows are
// sharded across nodes.
const std::vector<Json> kAccounts = {
    {{"account_id", 1}, {"tier", 10}}, {{"account_id", 2}, {"tier", 20}},
    {{"account_id", 3}, {"tier", 30}}, {{"account_id", 4}, {"tier", 40}},
    {{"account_id", 5}, {"tier", 50}},
};
const std::vector<Json> kEvents = {
    {{"account_id", 1}, {"amount", 100.0}}, {{"account_id", 1}, {"amount", 150.0}},
    {{"account_id", 2}, {"amount", 200.0}}, {{"account_id", 3}, {"amount", 300.0}},
    {{"account_id", 3}, {"amount", 350.0}}, {{"account_id", 5}, {"amount", 500.0}},
    {{"account_id", 9}, {"amount", 999.0}},
};
// Output columns are [account_id, tier, amount]; rows as doubles.
const std::vector<std::vector<double>> kExpected = {
    {1, 10, 100}, {1, 10, 150}, {2, 20, 200},
    {3, 30, 300}, {3, 30, 350}, {5, 50, 500},
};

DataFrameSchema accounts_schema() {
    DataFrameSchema schema;
    schema.columns = {{.name = "account_id", .dtype = DataType::Int64},
                      {.name = "tier", .dtype = DataType::Int32}};
    return schema;
}
DataFrameSchema events_schema() {
    DataFrameSchema schema;
    schema.columns = {{.name = "account_id", .dtype = DataType::Int64},
                      {.name = "amount", .dtype = DataType::Float64}};
    return schema;
}

// A HostRelation built directly (no device) — for the pure local-join tests.
HostRelation accounts_relation() {
    HostRelation relation;
    relation.key_name = "account_id";
    relation.value_columns = {"tier"};
    for (const Json& row : kAccounts) {
        relation.keys.push_back(row["account_id"].get<double>());
        relation.values.push_back(row["tier"].get<double>());
    }
    return relation;
}
HostRelation events_relation() {
    HostRelation relation;
    relation.key_name = "account_id";
    relation.value_columns = {"amount"};
    for (const Json& row : kEvents) {
        relation.keys.push_back(row["account_id"].get<double>());
        relation.values.push_back(row["amount"].get<double>());
    }
    return relation;
}

// Load `rows` into a chunked DataFrame on `device` (small chunks to exercise the
// multi-chunk read path in to_host_relation).
DataFrame make_frame(CpuDevice& device, const std::vector<Json>& rows,
                     const DataFrameSchema& schema) {
    zetta::storage::JsonStore store;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.add("r" + std::to_string(i), rows[i]);
    }
    auto result = load_data_frame(store, schema, device, /*chunk_rows=*/2);
    EXPECT_TRUE(result.ok()) << result.status.message;
    return std::move(result.frame);
}

// The sorted set of rows of a JoinedRows, for order-independent comparison.
std::vector<std::vector<double>> row_set(const JoinedRows& rows) {
    std::vector<std::vector<double>> out;
    const std::size_t width = rows.width();
    for (std::size_t row = 0; row < rows.row_count(); ++row) {
        out.emplace_back(rows.data.begin() + row * width,
                         rows.data.begin() + (row + 1) * width);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

// One node of a loopback compute cluster (mirrors test_job_coordinator).
struct ComputeNode {
    ComputeNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds, unsigned workers)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, workers) {
        node.start();
    }

    static NodeConfig node_config(const TempDir& dir) {
        NodeConfig cfg;
        cfg.data_dir = dir.path().string();
        return cfg;
    }
    static ClusterConfig cluster_config(std::uint16_t port,
                                        const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    TempDir temp_dir;  // declared first so it outlives `node`
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
};

// Slice [begin, end) of a row list.
std::vector<Json> slice(const std::vector<Json>& rows, std::size_t begin,
                        std::size_t end) {
    return std::vector<Json>(rows.begin() + begin,
                             rows.begin() + std::min(end, rows.size()));
}

// Run a distributed join across `node_count` loopback nodes, each owning a
// contiguous shard of accounts + events, and return the union of every node's
// returned shard as a sorted row set. Asserts every node's job Succeeded.
std::vector<std::vector<double>> run_distributed_join(std::size_t node_count,
                                                      std::uint16_t base_port,
                                                      JoinAlgorithm algorithm) {
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c", "node-d"};
    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < node_count; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        nodes.push_back(std::make_unique<ComputeNode>(
            ids[index], static_cast<std::uint16_t>(base_port + index), seeds,
            /*workers=*/2));
    }
    for (auto& node : nodes) node->cluster.start();
    EXPECT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != node_count - 1) return false;
        }
        return true;
    }));
    for (auto& node : nodes) node->coordinator.start();

    // Each node loads its own contiguous shard of both relations onto its device.
    std::vector<std::unique_ptr<CpuDevice>> devices;
    std::vector<DataFrame> lefts;
    std::vector<DataFrame> rights;
    const std::size_t acc_per = (kAccounts.size() + node_count - 1) / node_count;
    const std::size_t evt_per = (kEvents.size() + node_count - 1) / node_count;
    for (std::size_t index = 0; index < node_count; ++index) {
        devices.push_back(std::make_unique<CpuDevice>());
        lefts.push_back(make_frame(*devices[index],
                                   slice(kAccounts, index * acc_per,
                                         (index + 1) * acc_per),
                                   accounts_schema()));
        rights.push_back(make_frame(*devices[index],
                                    slice(kEvents, index * evt_per,
                                          (index + 1) * evt_per),
                                    events_schema()));
    }

    // Every node drives its own Ddf job on its own thread; the body runs the
    // distributed join (collectives need all nodes participating concurrently).
    std::vector<JoinedRows> shards(node_count);
    std::vector<JobStatus> statuses(node_count, JobStatus::Queued);
    std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < node_count; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Ddf);
        threads.emplace_back([&, index] {
            statuses[index] = nodes[index]->coordinator.run(
                *jobs[index], [&, index](JobRound& round) -> JobCoordinator::Outcome {
                    auto result = zetta::ddf::distributed_join(
                        round, lefts[index], "account_id", rights[index],
                        "account_id", algorithm);
                    if (!result.ok()) {
                        return {.ok = false, .detail = result.status.message};
                    }
                    shards[index] = std::move(result.rows);
                    return {.ok = true,
                            .detail = "rows=" +
                                      std::to_string(shards[index].row_count())};
                });
        });
    }
    for (auto& thread : threads) thread.join();

    std::vector<std::vector<double>> joined;
    for (std::size_t index = 0; index < node_count; ++index) {
        EXPECT_EQ(statuses[index], JobStatus::Succeeded)
            << "node " << index << ": " << jobs[index]->detail();
        for (std::vector<double>& row : row_set(shards[index])) {
            joined.push_back(std::move(row));
        }
    }
    std::sort(joined.begin(), joined.end());

    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
    return joined;
}

// --- Local join (pure, no cluster) -------------------------------------------

TEST(DdfJoin, HashJoinMatchesExpected) {
    const JoinedRows joined =
        zetta::ddf::hash_join(accounts_relation(), events_relation());
    EXPECT_EQ(joined.columns,
              (std::vector<std::string>{"account_id", "tier", "amount"}));
    EXPECT_EQ(joined.row_count(), kExpected.size());
    EXPECT_EQ(row_set(joined), kExpected);
}

TEST(DdfJoin, SortMergeJoinMatchesHashJoin) {
    const JoinedRows hash =
        zetta::ddf::hash_join(accounts_relation(), events_relation());
    const JoinedRows merge =
        zetta::ddf::sort_merge_join(accounts_relation(), events_relation());
    EXPECT_EQ(row_set(merge), kExpected);
    EXPECT_EQ(row_set(merge), row_set(hash));  // same set, possibly different order
}

TEST(DdfJoin, DuplicateKeysProduceCartesianProduct) {
    // Both sides carry key 7 twice -> 2 x 2 = 4 output rows for that key.
    HostRelation left;
    left.key_name = "k";
    left.value_columns = {"l"};
    left.keys = {7, 7};
    left.values = {1, 2};
    HostRelation right;
    right.key_name = "k";
    right.value_columns = {"r"};
    right.keys = {7, 7};
    right.values = {10, 20};

    EXPECT_EQ(zetta::ddf::hash_join(left, right).row_count(), 4u);
    EXPECT_EQ(zetta::ddf::sort_merge_join(left, right).row_count(), 4u);
    EXPECT_EQ(row_set(zetta::ddf::hash_join(left, right)),
              row_set(zetta::ddf::sort_merge_join(left, right)));
}

TEST(DdfJoin, NoMatchesYieldsEmpty) {
    HostRelation left = accounts_relation();
    HostRelation right;
    right.key_name = "account_id";
    right.value_columns = {"amount"};
    right.keys = {99};
    right.values = {1.0};
    EXPECT_EQ(zetta::ddf::hash_join(left, right).row_count(), 0u);
    EXPECT_EQ(zetta::ddf::sort_merge_join(left, right).row_count(), 0u);
}

TEST(DdfJoin, KeyOnlyRelationsJoin) {
    // Relations with no value columns: output is just the key.
    HostRelation left;
    left.key_name = "k";
    left.keys = {1, 2, 3};
    HostRelation right;
    right.key_name = "k";
    right.keys = {2, 3, 4};
    const JoinedRows joined = zetta::ddf::hash_join(left, right);
    EXPECT_EQ(joined.columns, (std::vector<std::string>{"k"}));
    EXPECT_EQ(row_set(joined),
              (std::vector<std::vector<double>>{{2}, {3}}));
}

// --- Repartition primitives (pure) -------------------------------------------

TEST(DdfJoin, SerializeRoundTrips) {
    const HostRelation relation = events_relation();
    const std::vector<double> blob = zetta::ddf::serialize_relation(relation);
    EXPECT_EQ(blob.size(), relation.row_count() * (1 + relation.width()));
    const auto back = zetta::ddf::deserialize_relation(blob, relation.key_name,
                                                       relation.value_columns);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->keys, relation.keys);
    EXPECT_EQ(back->values, relation.values);
    EXPECT_EQ(back->value_columns, relation.value_columns);
}

TEST(DdfJoin, DeserializeRejectsMalformedBlob) {
    // 3 doubles cannot be a whole number of rows of width 2 (key + 1 value).
    const std::vector<double> blob = {1.0, 2.0, 3.0};
    EXPECT_FALSE(zetta::ddf::deserialize_relation(blob, "k", {"v"}).has_value());
    // Empty blob is a valid 0-row relation.
    EXPECT_TRUE(zetta::ddf::deserialize_relation({}, "k", {"v"}).has_value());
}

TEST(DdfJoin, PartitionIsStableAndInRange) {
    constexpr std::size_t kParts = 3;
    for (double key = 0; key < 50; ++key) {
        const std::size_t part = zetta::ddf::partition_index(key, kParts);
        EXPECT_LT(part, kParts);
        EXPECT_EQ(part, zetta::ddf::partition_index(key, kParts));  // deterministic
    }
    EXPECT_EQ(zetta::ddf::partition_index(42.0, 1), 0u);  // single participant
}

TEST(DdfJoin, FilterPartitionsCoverEveryRowExactlyOnce) {
    constexpr std::size_t kParts = 3;
    const HostRelation relation = events_relation();
    std::size_t total = 0;
    for (std::size_t part = 0; part < kParts; ++part) {
        total += zetta::ddf::filter_partition(relation, part, kParts).row_count();
    }
    EXPECT_EQ(total, relation.row_count());  // a partition of the rows
}

// --- to_host_relation (frame -> host) ----------------------------------------

TEST(DdfJoin, ToHostRelationReadsKeyAndValueColumns) {
    CpuDevice device;
    const DataFrame frame = make_frame(device, kEvents, events_schema());
    const auto result = zetta::ddf::to_host_relation(frame, "account_id");
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.relation.key_name, "account_id");
    EXPECT_EQ(result.relation.value_columns, (std::vector<std::string>{"amount"}));
    EXPECT_EQ(result.relation.row_count(), kEvents.size());
    EXPECT_DOUBLE_EQ(result.relation.keys.front(), 1.0);
    EXPECT_DOUBLE_EQ(result.relation.values.front(), 100.0);  // amount widened
}

TEST(DdfJoin, ToHostRelationRejectsUnknownKey) {
    CpuDevice device;
    const DataFrame frame = make_frame(device, kEvents, events_schema());
    const auto result = zetta::ddf::to_host_relation(frame, "nope");
    EXPECT_FALSE(result.ok());
}

// --- The acceptance: single-node and multi-node, identical results -----------

TEST(DdfJoin, SingleNodeJoinMatchesExpected) {
    const auto joined = run_distributed_join(/*node_count=*/1, 47300,
                                             JoinAlgorithm::Hash);
    EXPECT_EQ(joined, kExpected);
}

TEST(DdfJoin, MultiNodeJoinMatchesSingleNode) {
    const auto single = run_distributed_join(1, 47310, JoinAlgorithm::Hash);
    const auto multi = run_distributed_join(3, 47320, JoinAlgorithm::Hash);
    EXPECT_EQ(multi, kExpected);
    EXPECT_EQ(multi, single);  // distribution does not change the result
}

TEST(DdfJoin, MultiNodeSortMergeMatchesHash) {
    const auto multi = run_distributed_join(3, 47340, JoinAlgorithm::SortMerge);
    EXPECT_EQ(multi, kExpected);
}

}  // namespace
