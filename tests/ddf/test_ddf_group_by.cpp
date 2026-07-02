#include <gtest/gtest.h>

#include <chrono>
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
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/group_by_job.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/storage/json_store.hpp"

// Running a DDF distributed groupby-aggregate as a recorded ComputeJob (ARC-12 /
// FEAT-8, #387). The groupby op itself — the local op, the mergeable-partial
// monoid, and the op-spec JSON boundary — is covered by test_group_by.cpp (#383);
// this test covers the slice #387 adds on top: a distributed groupby that runs as
// a ComputeJob (kind ddf) across a multi-node loopback cluster, is driven through
// its lifecycle on every node, and shows up in system.zetta_jobs.
namespace {

using namespace std::chrono_literals;

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::ComputeJob;
using zetta::JobKind;
using zetta::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::JobCoordinator;
using zetta::ddf::Aggregate;
using zetta::ddf::AggregateOp;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::GroupByResult;
using zetta::ddf::GroupBySpec;
using zetta::ddf::run_group_by_job;
using zetta::query::QueryService;
using zetta::test::TempDir;

// Build an in-memory store from row docs (ids are not exercised here).
zetta::JsonStore make_store(const std::vector<nlohmann::json>& rows) {
    zetta::JsonStore store;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.add("row-" + std::to_string(i), rows[i]);
    }
    return store;
}

// A two-column schema: integer "segment" (the group key) + real "amount" (value).
DataFrameSchema churn_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "segment", .dtype = DataType::Int32},
        ColumnSchema{.name = "amount", .dtype = DataType::Float64},
    };
    return schema;
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

// Look up the value of aggregate `output` for group `key` in a columnar
// GroupByResult (keys[] aligned with value(output)[]).
double value_for(const GroupByResult& result, double key, const std::string& output) {
    const std::vector<double>* column = result.value(output);
    EXPECT_NE(column, nullptr) << "no aggregate column '" << output << "'";
    if (column == nullptr) return 0.0;
    for (std::size_t group = 0; group < result.keys.size(); ++group) {
        if (result.keys[group] == key) return (*column)[group];
    }
    ADD_FAILURE() << "key not found: " << key;
    return 0.0;
}

// One node of a loopback compute cluster: a data Node (for the ComputeJob record),
// a Cluster (the compute plane), the JobCoordinator over it, and this node's local
// DataFrame shard (loaded onto its own CpuDevice). The data Node is standalone (no
// replication) — the test asserts the recorded job + the merged result locally.
struct ComputeNode {
    ComputeNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds, unsigned workers,
                const std::vector<nlohmann::json>& rows)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, workers) {
        node.start();
        const zetta::JsonStore store = make_store(rows);
        auto loaded = zetta::ddf::load_data_frame(store, churn_schema(), device);
        EXPECT_TRUE(loaded.ok()) << loaded.status.message;
        frame = std::move(loaded.frame);
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

    // temp_dir first (outlives node, which writes under it); device before frame
    // (the frame references the device and must be destroyed first).
    TempDir temp_dir;
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
    CpuDevice device;
    DataFrame frame;
};

}  // namespace

TEST(DdfGroupByJob, DistributedSumRecordsJobAndShowsInSystemTable) {
    // Controlled cluster ids so "node-a" is the deterministic collective root.
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    constexpr std::uint16_t kBasePort = 46300;

    // Each node owns a shard of (segment, amount) rows. Global per-segment sums:
    //   seg 1: 10 + 5 + 20            = 35   (count 3)
    //   seg 2: 100 + 200 + 50         = 350  (count 3)
    //   seg 3: 7 + 3                  = 10   (count 2)
    const std::vector<std::vector<nlohmann::json>> shards = {
        {{{"segment", 1}, {"amount", 10.0}},
         {{"segment", 1}, {"amount", 5.0}},
         {{"segment", 2}, {"amount", 100.0}}},
        {{{"segment", 1}, {"amount", 20.0}},
         {{"segment", 2}, {"amount", 200.0}},
         {{"segment", 3}, {"amount", 7.0}}},
        {{{"segment", 2}, {"amount", 50.0}},
         {{"segment", 3}, {"amount", 3.0}}},
    };

    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<ComputeNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds,
            /*workers=*/2, shards[index]));
    }

    for (auto& node : nodes) node->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    }));
    for (auto& node : nodes) node->coordinator.start();

    // A sum + count per segment (count rides as a second aggregate over the same
    // grouping, so the test asserts both the summed value and the group size).
    const GroupBySpec spec{
        .key = "segment",
        .aggregates = {Aggregate{.op = AggregateOp::Sum, .column = "amount"},
                       Aggregate{.op = AggregateOp::Count}},
    };

    // Every node creates + drives its own ComputeJob (kind ddf) on its own thread;
    // the collective needs all participants concurrently.
    std::vector<std::shared_ptr<ComputeJob>> jobs(nodes.size());
    std::vector<zetta::ddf::DistributedGroupByResult> results(nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Ddf);
        threads.emplace_back([&, index] {
            // A shared round tag — every node must agree on it (each node's own job
            // id differs, so it cannot serve as the cross-node round name).
            results[index] = run_group_by_job(nodes[index]->coordinator, *jobs[index],
                                              nodes[index]->frame, spec,
                                              "segment-sum-round", 5s);
        });
    }
    for (auto& thread : threads) thread.join();

    // Every node's job succeeded and is durably recorded.
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        EXPECT_EQ(results[index].status, JobStatus::Succeeded)
            << "node " << index << " detail=" << jobs[index]->detail();
        EXPECT_EQ(jobs[index]->status(), JobStatus::Succeeded) << "node " << index;
        EXPECT_EQ(jobs[index]->kind(), JobKind::Ddf) << "node " << index;
        EXPECT_TRUE(nodes[index]->node.find_job(jobs[index]->id())) << "node " << index;
    }

    // distributed_group_by broadcasts the merged partial, so every node ends with
    // the same global aggregation (not the root alone).
    EXPECT_TRUE(results[0].is_root);
    EXPECT_FALSE(results[1].is_root);
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const GroupByResult& global = results[index].result;
        ASSERT_EQ(global.group_count(), 3u) << "node " << index;
        EXPECT_DOUBLE_EQ(value_for(global, 1.0, "sum_amount"), 35.0) << "node " << index;
        EXPECT_DOUBLE_EQ(value_for(global, 1.0, "count"), 3.0) << "node " << index;
        EXPECT_DOUBLE_EQ(value_for(global, 2.0, "sum_amount"), 350.0) << "node " << index;
        EXPECT_DOUBLE_EQ(value_for(global, 2.0, "count"), 3.0) << "node " << index;
        EXPECT_DOUBLE_EQ(value_for(global, 3.0, "sum_amount"), 10.0) << "node " << index;
        EXPECT_DOUBLE_EQ(value_for(global, 3.0, "count"), 2.0) << "node " << index;
    }

    // The acceptance: the recorded job shows up in system.zetta_jobs.
    QueryService svc{nodes[0]->node};
    auto rows = svc.run(
        "SELECT job_id, kind, status FROM system.zetta_jobs WHERE kind = 'ddf';", "");
    ASSERT_TRUE(rows.ok()) << rows.error;
    ASSERT_EQ(rows.rows.size(), 1u);
    EXPECT_EQ(rows.rows[0]["job_id"], jobs[0]->id());
    EXPECT_EQ(rows.rows[0]["kind"], "ddf");
    EXPECT_EQ(rows.rows[0]["status"], "succeeded");

    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
}
