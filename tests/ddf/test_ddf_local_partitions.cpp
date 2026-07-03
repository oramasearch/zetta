#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
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
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/local_group_by_job.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/query/service.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/data/table.hpp"

// Compute-near-data: a DDF op executes over a node's LOCAL partitions (#516). A
// node enumerates its owned partitions from the catalog, materializes only the
// partition bodies it physically holds, and runs the distributed group-by over
// that local shard — exchanging only the small per-node partial. This replaces the
// "shard handed in out-of-band" path (test_ddf_group_by.cpp). Two checks:
//   1. the local-partition loader round-trips the partitioned rows and reads only
//      the partitions THIS node owns;
//   2. across a real loopback cluster the near-data group-by matches the
//      single-node oracle, and the instrumented bytes-moved ≈ the partials, not
//      the dataset (the TC-11 / #501 acceptance).
namespace {

using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::data::DatasetPartitionValue;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::data::RoundRobinPartitioner;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::ddf::Aggregate;
using zetta::ddf::AggregateOp;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::GroupByOutcome;
using zetta::ddf::GroupByResult;
using zetta::ddf::GroupBySpec;
using zetta::test::TempDir;

// A two-column schema: integer "segment" (the group key) + real "amount" (value).
DataFrameSchema churn_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "segment", .dtype = DataType::Int64},
        ColumnSchema{.name = "amount", .dtype = DataType::Float64},
    };
    return schema;
}

GroupBySpec sum_count_mean() {
    return GroupBySpec{
        .key = "segment",
        .aggregates = {Aggregate{.op = AggregateOp::Sum, .column = "amount"},
                       Aggregate{.op = AggregateOp::Count},
                       Aggregate{.op = AggregateOp::Mean, .column = "amount"}},
    };
}

nlohmann::json make_row(std::size_t i, int groups) {
    return nlohmann::json{{"segment", static_cast<std::int64_t>(i % groups)},
                          {"amount", static_cast<double>(i % 9) + 1.0}};
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

bool results_match(const GroupByResult& a, const GroupByResult& b) {
    if (a.keys.size() != b.keys.size() || a.values.size() != b.values.size()) {
        return false;
    }
    for (std::size_t g = 0; g < a.keys.size(); ++g) {
        if (std::abs(a.keys[g] - b.keys[g]) > 1e-9) return false;
    }
    for (std::size_t f = 0; f < a.values.size(); ++f) {
        if (a.values[f].size() != b.values[f].size()) return false;
        for (std::size_t g = 0; g < a.values[f].size(); ++g) {
            if (std::abs(a.values[f][g] - b.values[f][g]) > 1e-6) return false;
        }
    }
    return true;
}

NodeConfig node_config(const TempDir& dir) {
    NodeConfig cfg;
    cfg.data_dir = dir.path().string();
    return cfg;
}

ClusterConfig cluster_config(std::uint16_t port,
                             const std::vector<std::string>& seeds) {
    ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;
    config.seeds = seeds;
    return config;
}

}  // namespace

// The loader enumerates only THIS node's owned partitions, reads their bodies from
// the local LO store, and round-trips the rows — a foreign-owned catalog entry is
// skipped (its body lives elsewhere), and the group-by over the loaded shard
// equals the group-by over the original table.
TEST(DdfLocalPartitions, LoaderReadsOnlyOwnedPartitions) {
    TempDir dir;
    Node node(node_config(dir));
    node.start();

    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("events");
    ASSERT_TRUE(table);
    table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));

    constexpr std::size_t kRows = 40;
    constexpr int kGroups = 5;
    for (std::size_t i = 0; i < kRows; ++i) table->add_row(make_row(i, kGroups));

    // Partition the table: round-robin over 4 partitions, all owned by this node
    // (place defaults to this node). N non-empty partitions -> N catalog rows.
    const zetta::data::PartitionWriteResult written =
        zetta::data::write_partitioned(node, *database, *table);
    ASSERT_TRUE(written.ok()) << written.error;
    ASSERT_EQ(written.rows_written, kRows);
    ASSERT_EQ(written.partitions.size(), 4u);

    // A foreign-owned catalog entry for the SAME table — its body lives on another
    // node, so the loader must skip it (no error, no rows from it).
    DatasetPartitionValue foreign;
    foreign.table = table->id();
    foreign.blob_ref = "lo-on-another-node";  // not present in this node's LO store
    foreign.row_range = {1000, 1010};
    foreign.owner_nodes = {"some-other-node"};
    ASSERT_TRUE(node.create_dataset_partition(foreign));

    CpuDevice device;
    zetta::ddf::LocalShardResult shard =
        zetta::ddf::load_local_partitions(node, table->id(), churn_schema(), device);
    ASSERT_TRUE(shard.ok()) << shard.status.message;
    EXPECT_EQ(shard.partitions_read, 4u);  // the foreign partition was skipped
    EXPECT_EQ(shard.rows_read, kRows);
    EXPECT_EQ(shard.frame.row_count(), kRows);
    EXPECT_GT(shard.bytes_read, 0u);

    // The group-by over the loaded shard equals the group-by over the full table.
    zetta::storage::JsonStore full;
    for (std::size_t i = 0; i < kRows; ++i) {
        full.add("row-" + std::to_string(i), make_row(i, kGroups));
    }
    zetta::ddf::DataFrameResult full_frame =
        zetta::ddf::load_data_frame(full, churn_schema(), device);
    ASSERT_TRUE(full_frame.ok()) << full_frame.status.message;
    const GroupByOutcome truth = zetta::ddf::group_by(full_frame.frame, sum_count_mean());
    const GroupByOutcome from_partitions =
        zetta::ddf::group_by(shard.frame, sum_count_mean());
    ASSERT_TRUE(truth.ok());
    ASSERT_TRUE(from_partitions.ok());
    EXPECT_TRUE(results_match(from_partitions.result, truth.result));
}

// The loader is the reusable shard source for ANY DDF op, not just group-by (the
// issue lists group-by / featurize / GBDT round). A filter — then a group-by — over
// the locally-loaded shard equals the same ops over the full table.
TEST(DdfLocalPartitions, LoaderFeedsOtherDdfOps) {
    TempDir dir;
    Node node(node_config(dir));
    node.start();

    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("events");
    ASSERT_TRUE(table);
    table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));

    constexpr std::size_t kRows = 60;
    constexpr int kGroups = 5;
    for (std::size_t i = 0; i < kRows; ++i) table->add_row(make_row(i, kGroups));
    ASSERT_TRUE(zetta::data::write_partitioned(node, *database, *table).ok());

    CpuDevice device;
    zetta::ddf::LocalShardResult shard =
        zetta::ddf::load_local_partitions(node, table->id(), churn_schema(), device);
    ASSERT_TRUE(shard.ok()) << shard.status.message;
    ASSERT_EQ(shard.rows_read, kRows);

    // amount = (i % 9) + 1.0, so this keeps a strict subset of the rows.
    const zetta::ddf::FilterPredicate keep_high{
        .column = "amount", .comparison = zetta::ddf::Comparison::GreaterEqual, .value = 5.0};

    zetta::ddf::DataFrameResult filtered_shard = zetta::ddf::filter(shard.frame, keep_high);
    ASSERT_TRUE(filtered_shard.ok()) << filtered_shard.status.message;

    zetta::storage::JsonStore full;
    for (std::size_t i = 0; i < kRows; ++i) {
        full.add("row-" + std::to_string(i), make_row(i, kGroups));
    }
    zetta::ddf::DataFrameResult full_frame =
        zetta::ddf::load_data_frame(full, churn_schema(), device);
    ASSERT_TRUE(full_frame.ok()) << full_frame.status.message;
    zetta::ddf::DataFrameResult filtered_full =
        zetta::ddf::filter(full_frame.frame, keep_high);
    ASSERT_TRUE(filtered_full.ok()) << filtered_full.status.message;

    EXPECT_GT(filtered_shard.frame.row_count(), 0u);
    EXPECT_LT(filtered_shard.frame.row_count(), kRows);  // a real subset
    EXPECT_EQ(filtered_shard.frame.row_count(), filtered_full.frame.row_count());

    // A group-by over the filtered near-data shard equals the same over the full
    // table — the loader feeds the downstream op transparently.
    const GroupByOutcome from_shard =
        zetta::ddf::group_by(filtered_shard.frame, sum_count_mean());
    const GroupByOutcome truth = zetta::ddf::group_by(filtered_full.frame, sum_count_mean());
    ASSERT_TRUE(from_shard.ok());
    ASSERT_TRUE(truth.ok());
    EXPECT_TRUE(results_match(from_shard.result, truth.result));
}

// A partition OWNED BY THIS NODE whose body is missing locally is a fail-fast error,
// not a silent drop — a dropped row would diverge from the single-node oracle. (The
// sibling LoaderReadsOnlyOwnedPartitions check covers the opposite: a *foreign*-owned
// entry is skipped, since its body legitimately lives elsewhere.)
TEST(DdfLocalPartitions, MissingOwnedBodyFailsFast) {
    TempDir dir;
    Node node(node_config(dir));
    node.start();

    auto database = node.create_database("data");
    ASSERT_TRUE(database);
    auto table = database->create_table("events");
    ASSERT_TRUE(table);
    table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));

    constexpr std::size_t kRows = 40;
    for (std::size_t i = 0; i < kRows; ++i) table->add_row(make_row(i, 5));
    ASSERT_TRUE(zetta::data::write_partitioned(node, *database, *table).ok());

    // A catalog entry owned by THIS node, but whose body was never stored locally.
    DatasetPartitionValue missing;
    missing.table = table->id();
    missing.blob_ref = "lo-missing-body";  // absent from this node's LO store
    missing.row_range = {1000, 1010};
    missing.owner_nodes = {node.node_id()};  // owned by self
    ASSERT_TRUE(node.create_dataset_partition(missing));

    CpuDevice device;
    zetta::ddf::LocalShardResult shard =
        zetta::ddf::load_local_partitions(node, table->id(), churn_schema(), device);
    EXPECT_FALSE(shard.ok());  // fail-fast, not a silent partial load
    EXPECT_NE(shard.status.message.find("not present locally"), std::string::npos)
        << shard.status.message;
}

namespace {

// One node of a near-data loopback cluster: a standalone data Node holding a
// partitioned table (its shard, written to its own owned partitions + blobs), a
// Cluster (the compute plane), and a JobCoordinator over it. The op reads the
// shard from the node's OWNED partitions — nothing is handed in out-of-band.
struct NearDataNode {
    NearDataNode(std::string cluster_id, std::uint16_t port,
                 const std::vector<std::string>& seeds, unsigned workers,
                 const std::vector<nlohmann::json>& shard_rows)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, workers) {
        node.start();
        auto database = node.create_database("data");
        EXPECT_TRUE(database);
        auto table = database->create_table("events");
        EXPECT_TRUE(table);
        table->set_partitioner(std::make_shared<RoundRobinPartitioner>(4));
        for (const nlohmann::json& row : shard_rows) table->add_row(row);
        const zetta::data::PartitionWriteResult written =
            zetta::data::write_partitioned(node, *database, *table);
        EXPECT_TRUE(written.ok()) << written.error;
        table_id = table->id();
    }

    // temp_dir first (outlives node, which writes under it); device before any
    // frame so it is destroyed last.
    TempDir temp_dir;
    Node node;
    Cluster cluster;
    zetta::compute::JobCoordinator coordinator;
    CpuDevice device;
    std::string table_id;
};

}  // namespace

// Across a real 3-node loopback cluster, each node loads its shard from its own
// partitions and runs the distributed group-by. The global result matches the
// single-node oracle on every node, and the bytes each node MOVES over the wire
// are the partials (≪ the dataset it read near-data).
TEST(DdfLocalPartitions, NearDataGroupByMatchesOracleAndMovesOnlyPartials) {
    constexpr std::size_t kNodes = 3;
    constexpr std::size_t kRows = 900;
    constexpr int kGroups = 5;
    constexpr std::uint16_t kBasePort = 46500;

    // Single-node ground truth over the whole dataset.
    CpuDevice truth_device;
    zetta::storage::JsonStore full;
    for (std::size_t i = 0; i < kRows; ++i) {
        full.add("row-" + std::to_string(i), make_row(i, kGroups));
    }
    zetta::ddf::DataFrameResult full_frame =
        zetta::ddf::load_data_frame(full, churn_schema(), truth_device);
    ASSERT_TRUE(full_frame.ok()) << full_frame.status.message;
    const GroupByOutcome truth = zetta::ddf::group_by(full_frame.frame, sum_count_mean());
    ASSERT_TRUE(truth.ok());

    // The same rows sharded round-robin across the nodes — each node holds its
    // shard as its own partitions.
    std::vector<std::vector<nlohmann::json>> shards(kNodes);
    for (std::size_t i = 0; i < kRows; ++i) shards[i % kNodes].push_back(make_row(i, kGroups));

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

    const GroupBySpec spec = sum_count_mean();
    std::vector<std::shared_ptr<ComputeJob>> jobs(kNodes);
    std::vector<zetta::ddf::NearDataGroupByResult> results(kNodes);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < kNodes; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Ddf);
        threads.emplace_back([&, index] {
            results[index] = zetta::ddf::run_near_data_group_by_job(
                nodes[index]->coordinator, nodes[index]->node, *jobs[index],
                nodes[index]->table_id, churn_schema(), spec, nodes[index]->device,
                "near-data-round", 5s);
        });
    }
    for (auto& thread : threads) thread.join();

    std::uint64_t total_local_bytes = 0;
    std::uint64_t total_wire_bytes = 0;
    std::uint64_t total_rows_read = 0;
    for (std::size_t index = 0; index < kNodes; ++index) {
        const auto& result = results[index];
        EXPECT_EQ(result.status, JobStatus::Succeeded)
            << "node " << index << " detail=" << jobs[index]->detail();
        EXPECT_TRUE(results_match(result.result, truth.result))
            << "node " << index << " disagreed with the single-node oracle";
        EXPECT_EQ(result.rows_read, shards[index].size()) << "node " << index;
        EXPECT_GT(result.partitions_read, 0u) << "node " << index;
        EXPECT_GT(result.local_bytes_read, 0u) << "node " << index;
        EXPECT_GT(result.wire_bytes_moved, 0u) << "node " << index;
        // The op moved far fewer bytes over the wire than it read near data — the
        // partials, not the dataset.
        EXPECT_LT(result.wire_bytes_moved, result.local_bytes_read) << "node " << index;
        // The job is durably recorded (system.jobs).
        EXPECT_TRUE(nodes[index]->node.find_job(jobs[index]->id())) << "node " << index;

        total_local_bytes += result.local_bytes_read;
        total_wire_bytes += result.wire_bytes_moved;
        total_rows_read += result.rows_read;
    }
    // Every row was read (near data, across the shards) exactly once.
    EXPECT_EQ(total_rows_read, kRows);
    // Bytes-moved ≈ partials, NOT ≈ dataset: the whole cluster moved a small
    // fraction of what it read near data.
    EXPECT_LT(total_wire_bytes * 5, total_local_bytes)
        << "moved=" << total_wire_bytes << " read-near-data=" << total_local_bytes;

    // #589: the live run's instrumentation is queryable as a system.<table> on
    // every node. system.pipeline_jobs surfaces the per-job near-data counters the
    // runner recorded — the same local-vs-wire bytes asserted above — so the GUI
    // (and any operator) can read them with SQL instead of a bespoke hook.
    for (std::size_t index = 0; index < kNodes; ++index) {
        zetta::query::QueryService svc{nodes[index]->node};
        auto sel = svc.run("SELECT * FROM system.pipeline_jobs;", "");
        ASSERT_TRUE(sel.ok()) << sel.error;
        ASSERT_EQ(sel.rows.size(), 1u) << "node " << index;
        const auto& row = sel.rows[0];
        EXPECT_EQ(row["job_id"].get<std::string>(), jobs[index]->id())
            << "node " << index;
        EXPECT_EQ(row["kind"].get<std::string>(), "group_by") << "node " << index;
        EXPECT_EQ(row["node"].get<std::string>(), nodes[index]->node.node_id())
            << "node " << index;
        EXPECT_EQ(row["status"].get<std::string>(), "succeeded") << "node " << index;
        EXPECT_EQ(row["rows_read"].get<std::uint64_t>(), shards[index].size())
            << "node " << index;
        EXPECT_EQ(row["local_bytes_read"].get<std::uint64_t>(),
                  results[index].local_bytes_read)
            << "node " << index;
        EXPECT_EQ(row["wire_bytes_moved"].get<std::uint64_t>(),
                  results[index].wire_bytes_moved)
            << "node " << index;
        EXPECT_LT(row["wire_bytes_moved"].get<std::uint64_t>(),
                  row["local_bytes_read"].get<std::uint64_t>())
            << "node " << index;
    }
}
