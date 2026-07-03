#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
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
#include "zetta/data/database.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/local_featurize_job.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/data/table.hpp"

// Compute-near-data point-in-time featurization of engineered numeric columns over a
// PARTITIONED table (#532, the UC-1-at-scale featurize slice). Each node enumerates
// its owned partitions of the engineered table, materializes only the bodies it
// physically holds, and runs the distributed (static) featurization over that local
// shard — exchanging only the per-entity partials. Two acceptances:
//   1. a feature table is produced over the partitioned table with no leakage
//      (event_ts <= as_of — the post-cutoff conversations are excluded);
//   2. across a real loopback cluster the near-data feature table matches the
//      single-node oracle on every node.
namespace {

using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::data::RoundRobinPartitioner;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::FeatureTable;
using zetta::ddf::FeaturizeSpec;
using zetta::test::TempDir;
using Json = nlohmann::json;

constexpr std::int64_t kAsOf = 1000;

// The saas-sales-conversations shape: each row is one conversation (entity) carrying
// already-engineered numeric columns + an event timestamp + the binary outcome.
DataFrameSchema engineered_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "conversation_id", .dtype = DataType::Int64},
        ColumnSchema{.name = "customer_engagement", .dtype = DataType::Float64},
        ColumnSchema{.name = "sales_effectiveness", .dtype = DataType::Float64},
        ColumnSchema{.name = "conversation_length", .dtype = DataType::Int64},
        ColumnSchema{.name = "event_ts", .dtype = DataType::Int64},
        ColumnSchema{.name = "outcome", .dtype = DataType::Int64}};
    return schema;
}

// The static featurize spec: engineered columns carried through, outcome the label,
// point-in-time filtered to event_ts <= as_of.
FeaturizeSpec engineered_spec() {
    FeaturizeSpec spec;
    spec.entity_column = "conversation_id";
    spec.time_column = "event_ts";
    spec.as_of = static_cast<double>(kAsOf);
    spec.account_features = {"customer_engagement", "sales_effectiveness",
                             "conversation_length"};
    spec.label_column = "outcome";
    return spec;
}

// One conversation. Deterministic engineered columns; ~1 in 7 conversations is AFTER
// the as_of cutoff (event_ts > as_of) — the leakage trap that must be excluded.
Json make_conversation(std::size_t index) {
    const bool future = (index % 7 == 3);
    const std::int64_t ts = future ? kAsOf + 1 + static_cast<std::int64_t>(index % 50)
                                   : static_cast<std::int64_t>(index % 900);
    return Json{{"conversation_id", static_cast<std::int64_t>(index)},
                {"customer_engagement", static_cast<double>(index % 100) / 100.0},
                {"sales_effectiveness", static_cast<double>((index * 7) % 100) / 100.0},
                {"conversation_length", static_cast<std::int64_t>(index % 40) + 1},
                {"event_ts", ts},
                {"outcome", static_cast<std::int64_t>(index % 3 == 0 ? 1 : 0)}};
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

NodeConfig node_config(const TempDir& dir) {
    NodeConfig cfg;
    cfg.data_dir = dir.path().string();
    return cfg;
}
ClusterConfig cluster_config(std::uint16_t port, const std::vector<std::string>& seeds) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;
    config.seeds = seeds;
    return config;
}

// The single-node feature table over the whole dataset — the oracle the near-data
// distributed result must match.
FeatureTable single_node_oracle(const std::vector<Json>& rows, CpuDevice& device) {
    zetta::storage::JsonStore store;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.add("row-" + std::to_string(i), rows[i]);
    }
    zetta::ddf::DataFrameResult frame =
        zetta::ddf::load_data_frame(store, engineered_schema(), device);
    EXPECT_TRUE(frame.ok()) << frame.status.message;
    const zetta::ddf::FeaturizeResult result =
        zetta::ddf::point_in_time_featurize(frame.frame, frame.frame, engineered_spec());
    EXPECT_TRUE(result.ok()) << result.status.message;
    return result.table;
}

void expect_tables_equal(const FeatureTable& actual, const FeatureTable& expected) {
    ASSERT_EQ(actual.entities, expected.entities);
    ASSERT_EQ(actual.feature_names, expected.feature_names);
    for (const std::string& name : expected.feature_names) {
        const std::vector<double>* a = actual.feature(name);
        const std::vector<double>* e = expected.feature(name);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(a->size(), e->size());
        for (std::size_t row = 0; row < a->size(); ++row) {
            EXPECT_DOUBLE_EQ((*a)[row], (*e)[row]) << "feature " << name << " row " << row;
        }
    }
    EXPECT_EQ(actual.label_name, expected.label_name);
    ASSERT_EQ(actual.labels.size(), expected.labels.size());
    for (std::size_t row = 0; row < actual.labels.size(); ++row) {
        EXPECT_DOUBLE_EQ(actual.labels[row], expected.labels[row]) << "label row " << row;
    }
}

// One node of a near-data loopback cluster: a Node holding its shard of the
// engineered table as its OWN partitions (round-robin into 4 blobs), a Cluster (the
// compute plane), and a JobCoordinator over it.
struct NearDataNode {
    NearDataNode(std::string cluster_id, std::uint16_t port,
                 const std::vector<std::string>& seeds, unsigned workers,
                 const std::vector<Json>& shard_rows)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, workers) {
        node.start();
        auto database = node.create_database("data");
        EXPECT_TRUE(database);
        auto table = database->create_table("conversations");
        EXPECT_TRUE(table);
        table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));
        for (const Json& row : shard_rows) table->add_row(row);
        const zetta::data::PartitionWriteResult written =
            zetta::data::write_partitioned(node, *database, *table);
        EXPECT_TRUE(written.ok()) << written.error;
        table_id = table->id();
    }

    TempDir temp_dir;  // declared first so it outlives `node`
    Node node;
    Cluster cluster;
    zetta::compute::JobCoordinator coordinator;
    CpuDevice device;
    std::string table_id;
};

}  // namespace

// Across a real 3-node loopback cluster, each node featurizes the engineered columns
// over its OWN partitions, point-in-time. The global feature table matches the
// single-node oracle on every node, the post-cutoff conversations are excluded (no
// leakage), and the near-data instrumentation is populated.
TEST(DdfFeaturizePartitions, NearDataFeaturizeMatchesOracleNoLeakage) {
    constexpr std::size_t kNodes = 3;
    constexpr std::size_t kRows = 420;
    constexpr std::uint16_t kBasePort = 47200;

    std::vector<Json> all;
    for (std::size_t i = 0; i < kRows; ++i) all.push_back(make_conversation(i));

    // The single-node ground truth over the whole dataset.
    CpuDevice truth_device;
    const FeatureTable oracle = single_node_oracle(all, truth_device);
    // The leakage trap is real: fewer rows than conversations (the post-cutoff ones
    // were excluded), and none of the kept rows is after the cutoff.
    ASSERT_LT(oracle.row_count(), kRows);
    std::size_t expected_future = 0;
    for (std::size_t i = 0; i < kRows; ++i) {
        if (i % 7 == 3) ++expected_future;
    }
    EXPECT_EQ(oracle.row_count(), kRows - expected_future);

    // The same conversations sharded contiguously across the nodes — each node holds
    // its shard as its own partitions (every conversation lives on exactly one node).
    std::vector<std::vector<Json>> shards(kNodes);
    for (std::size_t i = 0; i < kRows; ++i) shards[i % kNodes].push_back(all[i]);

    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    std::vector<std::unique_ptr<NearDataNode>> nodes;
    for (std::size_t index = 0; index < kNodes; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<NearDataNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds,
            /*workers=*/2, shards[index]));
    }

    for (auto& node : nodes) node->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    })) << "cluster did not converge";
    for (auto& node : nodes) node->coordinator.start();

    const FeaturizeSpec spec = engineered_spec();
    std::vector<std::shared_ptr<ComputeJob>> jobs(kNodes);
    std::vector<zetta::ddf::NearDataFeaturizeResult> results(kNodes);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < kNodes; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Featurize);
        threads.emplace_back([&, index] {
            results[index] = zetta::ddf::run_near_data_featurize_job(
                nodes[index]->coordinator, nodes[index]->node, *jobs[index],
                nodes[index]->table_id, engineered_schema(), spec,
                nodes[index]->device, "near-data-featurize", 5s);
        });
    }
    for (auto& thread : threads) thread.join();

    std::uint64_t total_rows_read = 0;
    for (std::size_t index = 0; index < kNodes; ++index) {
        const auto& result = results[index];
        EXPECT_EQ(result.status, JobStatus::Succeeded)
            << "node " << index << " detail=" << jobs[index]->detail();
        // The global feature table is identical on every node (broadcast) and equals
        // the single-node oracle.
        expect_tables_equal(result.table, oracle);
        // Near-data instrumentation: this node read its own partitions, near data.
        EXPECT_EQ(result.rows_read, shards[index].size()) << "node " << index;
        EXPECT_GT(result.partitions_read, 0u) << "node " << index;
        EXPECT_GT(result.local_bytes_read, 0u) << "node " << index;
        EXPECT_GT(result.wire_bytes_moved, 0u) << "node " << index;
        // The job is durably recorded (system.zetta_jobs).
        EXPECT_TRUE(nodes[index]->node.find_job(jobs[index]->id())) << "node " << index;
        EXPECT_EQ(jobs[index]->kind(), JobKind::Featurize) << "node " << index;
        total_rows_read += result.rows_read;
    }
    EXPECT_TRUE(results[0].is_root);
    // Every conversation was read near data, across the shards, exactly once.
    EXPECT_EQ(total_rows_read, kRows);
}
