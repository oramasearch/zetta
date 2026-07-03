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
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/local_train_job.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"

// Compute-near-data GBDT training on the binary `outcome` label over a PARTITIONED
// feature table (#533, the UC-1-at-scale train slice). Each node enumerates its owned
// partitions of the feature table, materializes only the bodies it physically holds,
// and runs the whole train → explain → rank pipeline over that local shard — exchanging
// only the per-node partials. The acceptances:
//   1. a calibrated `outcome` model is produced over the partitioned dataset, with
//      AUC/log-loss/accuracy recorded, per-row TreeSHAP drivers, and a ranked at-risk
//      list — every node ending with the identical model/metrics/drivers/ranking;
//   2. that distributed result matches a single-node oracle over the whole dataset
//      (single-node == multi-node), within floating-point reduction tolerance.
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
using zetta::ml::NearDataTrainResult;
using zetta::test::TempDir;
using Json = nlohmann::json;

constexpr std::size_t kRows = 240;
const std::vector<std::string> kFeatureColumns = {
    "customer_engagement", "sales_effectiveness", "conversation_length"};

// The saas-sales-conversations feature-table shape: each row is one conversation
// (entity) carrying already-engineered numeric columns + the binary outcome label.
DataFrameSchema engineered_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "conversation_id", .dtype = DataType::Int64},
        ColumnSchema{.name = "customer_engagement", .dtype = DataType::Float64},
        ColumnSchema{.name = "sales_effectiveness", .dtype = DataType::Float64},
        ColumnSchema{.name = "conversation_length", .dtype = DataType::Int64},
        ColumnSchema{.name = "outcome", .dtype = DataType::Int64}};
    return schema;
}

// Modest GBDT knobs — enough trees to learn the signal, small enough to stay fast.
zetta::ml::GbdtParams gbdt_params() {
    zetta::ml::GbdtParams params;
    params.num_trees = 20;
    params.max_depth = 3;
    params.max_bins = 32;
    params.learning_rate = 0.2;
    return params;
}

// Opt into held-out Platt calibration — the issue's "calibrated outcome model".
zetta::ml::CalibrationConfig calibration_config() {
    zetta::ml::CalibrationConfig calibration;
    calibration.enabled = true;
    calibration.method = zetta::ml::CalibrationMethod::Platt;
    calibration.holdout_fraction = 0.2;
    return calibration;
}

// One conversation. `customer_engagement` is the dominant signal (outcome ~ low
// engagement), the other columns are weak/noise, and ~9% of labels are flipped — so
// the model learns a strong-but-imperfect signal (AUC in (0.5, 1.0), calibration
// meaningful) and customer_engagement is the unambiguous top SHAP driver.
Json make_conversation(std::size_t index) {
    const double engagement = static_cast<double>(index % 100) / 100.0;
    const double effectiveness = static_cast<double>((index * 31) % 100) / 100.0;
    const std::int64_t length = static_cast<std::int64_t>((index * 17) % 40) + 1;
    const int base = engagement < 0.5 ? 1 : 0;
    const bool flip = (index % 11 == 0);
    const int outcome = flip ? 1 - base : base;
    return Json{{"conversation_id", static_cast<std::int64_t>(index)},
                {"customer_engagement", engagement},
                {"sales_effectiveness", effectiveness},
                {"conversation_length", length},
                {"outcome", static_cast<std::int64_t>(outcome)}};
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

// One node of a near-data loopback cluster: a Node holding its shard of the feature
// table as its OWN partitions (round-robin into 4 blobs, owned by itself), a Cluster
// (the compute plane), and a JobCoordinator over it.
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

// Build an N-node loopback cluster over the given per-node shards, wait for it to
// converge, and start each node's coordinator. The nodes are returned alive (the
// caller drives the pipeline + assertions over them, then lets them tear down).
std::vector<std::unique_ptr<NearDataNode>> build_cluster(
    const std::vector<std::vector<Json>>& shards, std::uint16_t base_port) {
    const std::size_t count = shards.size();
    std::vector<std::unique_ptr<NearDataNode>> nodes;
    for (std::size_t index = 0; index < count; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        nodes.push_back(std::make_unique<NearDataNode>(
            "node-" + std::to_string(base_port) + "-" + std::to_string(index),
            static_cast<std::uint16_t>(base_port + index), seeds,
            /*workers=*/2, shards[index]));
    }
    for (auto& node : nodes) node->cluster.start();
    EXPECT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != count - 1) return false;
        }
        return true;
    })) << "cluster did not converge";
    for (auto& node : nodes) node->coordinator.start();
    return nodes;
}

// Run the near-data train pipeline concurrently on every node of a converged cluster,
// returning each node's result (index-aligned with `nodes`). `jobs` is filled with the
// headline train ComputeJob per node so the caller can assert it is recorded.
std::vector<NearDataTrainResult> run_pipeline(
    std::vector<std::unique_ptr<NearDataNode>>& nodes,
    std::vector<std::shared_ptr<ComputeJob>>& jobs) {
    const std::size_t count = nodes.size();
    std::vector<NearDataTrainResult> results(count);
    jobs.assign(count, nullptr);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < count; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Train);
        threads.emplace_back([&, index] {
            results[index] = zetta::ml::run_near_data_train_job(
                nodes[index]->coordinator, nodes[index]->node, *jobs[index],
                nodes[index]->table_id, engineered_schema(), kFeatureColumns,
                "outcome", gbdt_params(), nodes[index]->device, "conversation_id",
                calibration_config(), "outcome-gbdt", /*top_k=*/0,
                /*round_tag=*/"near-data-train", 8s);
        });
    }
    for (auto& thread : threads) thread.join();
    return results;
}

// entity -> calibrated risk, from a ranked at-risk list.
std::map<std::int64_t, double> risk_by_entity(
    const std::vector<zetta::ml::EntityScore>& ranked) {
    std::map<std::int64_t, double> out;
    for (const auto& score : ranked) out[score.entity] = score.risk;
    return out;
}

}  // namespace

// Across a real 3-node loopback cluster, each node trains the GBDT on `outcome` over
// its OWN partitions of the feature table, calibrates, explains, and ranks. The model
// + metrics + per-row drivers + ranking are identical on every node and match a
// single-node oracle over the whole dataset (single-node == multi-node), with the
// near-data instrumentation populated and every step recorded as a ComputeJob.
TEST(GbdtTrainPartitions, NearDataTrainCalibratedModelDriversRankMatchOracle) {
    std::vector<Json> all;
    for (std::size_t i = 0; i < kRows; ++i) all.push_back(make_conversation(i));

    // --- the single-node oracle: one node holding ALL rows as its own partitions ---
    std::vector<std::unique_ptr<NearDataNode>> oracle_nodes =
        build_cluster({all}, /*base_port=*/47400);
    std::vector<std::shared_ptr<ComputeJob>> oracle_jobs;
    const std::vector<NearDataTrainResult> oracle = run_pipeline(oracle_nodes, oracle_jobs);
    ASSERT_EQ(oracle.size(), 1u);
    ASSERT_EQ(oracle[0].status, JobStatus::Succeeded)
        << "oracle detail=" << oracle_jobs[0]->detail();
    // The model learned a real, calibrated signal.
    EXPECT_TRUE(oracle[0].calibrated);
    EXPECT_TRUE(oracle[0].model.calibration.has_value());
    EXPECT_GT(oracle[0].metrics.auc, 0.6);
    EXPECT_LT(oracle[0].metrics.auc, 1.0);
    EXPECT_GT(oracle[0].metrics.log_loss, 0.0);
    EXPECT_GT(oracle[0].metrics.accuracy, 0.6);
    EXPECT_EQ(oracle[0].ranked.size(), kRows);
    EXPECT_EQ(oracle[0].top_driver, "customer_engagement");

    // --- the 3-node distributed run, same data sharded across the cluster ---
    constexpr std::size_t kNodes = 3;
    std::vector<std::vector<Json>> shards(kNodes);
    for (std::size_t i = 0; i < kRows; ++i) shards[i % kNodes].push_back(all[i]);

    std::vector<std::unique_ptr<NearDataNode>> nodes =
        build_cluster(shards, /*base_port=*/47410);
    std::vector<std::shared_ptr<ComputeJob>> jobs;
    const std::vector<NearDataTrainResult> results = run_pipeline(nodes, jobs);

    std::uint64_t total_rows_read = 0;
    std::size_t total_explanations = 0;
    for (std::size_t index = 0; index < kNodes; ++index) {
        const NearDataTrainResult& result = results[index];
        EXPECT_EQ(result.status, JobStatus::Succeeded)
            << "node " << index << " detail=" << jobs[index]->detail();

        // (1) Calibrated outcome model, with AUC/log-loss/accuracy recorded.
        EXPECT_TRUE(result.calibrated) << "node " << index;
        EXPECT_TRUE(result.model.calibration.has_value()) << "node " << index;
        EXPECT_GT(result.metrics.auc, 0.6) << "node " << index;
        EXPECT_GT(result.metrics.accuracy, 0.6) << "node " << index;

        // Every node converges on the IDENTICAL model + metrics + ranking (broadcast).
        EXPECT_EQ(result.model.to_json().dump(), results[0].model.to_json().dump())
            << "node " << index << " model differs";
        EXPECT_DOUBLE_EQ(result.metrics.auc, results[0].metrics.auc) << "node " << index;
        EXPECT_DOUBLE_EQ(result.metrics.log_loss, results[0].metrics.log_loss)
            << "node " << index;
        EXPECT_EQ(result.ranked.size(), results[0].ranked.size()) << "node " << index;

        // (2) Per-row TreeSHAP drivers for this node's shard; the global driver is the
        // signal column, identical on every node.
        EXPECT_EQ(result.explanations.size(), shards[index].size()) << "node " << index;
        EXPECT_EQ(result.feature_names, kFeatureColumns) << "node " << index;
        EXPECT_EQ(result.top_driver, "customer_engagement") << "node " << index;
        total_explanations += result.explanations.size();

        // (3) The global ranked at-risk list: every entity once, sorted by risk desc.
        EXPECT_EQ(result.ranked.size(), kRows) << "node " << index;
        for (std::size_t i = 1; i < result.ranked.size(); ++i) {
            EXPECT_GE(result.ranked[i - 1].risk, result.ranked[i].risk)
                << "node " << index << " rank not descending at " << i;
        }
        for (const auto& score : result.ranked) {
            EXPECT_GE(score.risk, 0.0);
            EXPECT_LE(score.risk, 1.0);
        }

        // Near-data instrumentation: this node read its own partitions, near data, and
        // moved only partials over the wire.
        EXPECT_EQ(result.rows_read, shards[index].size()) << "node " << index;
        EXPECT_GT(result.partitions_read, 0u) << "node " << index;
        EXPECT_GT(result.local_bytes_read, 0u) << "node " << index;
        EXPECT_GT(result.wire_bytes_moved, 0u) << "node " << index;
        total_rows_read += result.rows_read;

        // Every step is durably recorded (system.jobs): the train job passed in,
        // plus the explain + score jobs this runner created.
        EXPECT_TRUE(nodes[index]->node.find_job(jobs[index]->id())) << "node " << index;
        EXPECT_EQ(jobs[index]->kind(), JobKind::Train) << "node " << index;
        EXPECT_FALSE(result.explain_job_id.empty()) << "node " << index;
        EXPECT_FALSE(result.score_job_id.empty()) << "node " << index;
        EXPECT_TRUE(nodes[index]->node.find_job(result.explain_job_id)) << "node " << index;
        EXPECT_TRUE(nodes[index]->node.find_job(result.score_job_id)) << "node " << index;
    }
    EXPECT_TRUE(results[0].is_root);
    // Every conversation was read near data, across the shards, exactly once; and
    // explained exactly once across the cluster.
    EXPECT_EQ(total_rows_read, kRows);
    EXPECT_EQ(total_explanations, kRows);

    // --- single-node == multi-node: the distributed result matches the oracle ---
    EXPECT_EQ(results[0].model.tree_count(), oracle[0].model.tree_count());
    EXPECT_EQ(results[0].top_driver, oracle[0].top_driver);
    EXPECT_NEAR(results[0].metrics.auc, oracle[0].metrics.auc, 1e-3);
    EXPECT_NEAR(results[0].metrics.log_loss, oracle[0].metrics.log_loss, 1e-3);
    EXPECT_NEAR(results[0].metrics.accuracy, oracle[0].metrics.accuracy, 1e-3);

    // Per-entity calibrated risk matches the oracle within fp reduction tolerance.
    const std::map<std::int64_t, double> oracle_risk = risk_by_entity(oracle[0].ranked);
    const std::map<std::int64_t, double> dist_risk = risk_by_entity(results[0].ranked);
    ASSERT_EQ(oracle_risk.size(), dist_risk.size());
    for (const auto& [entity, risk] : oracle_risk) {
        const auto it = dist_risk.find(entity);
        ASSERT_NE(it, dist_risk.end()) << "entity " << entity << " missing in distributed";
        EXPECT_NEAR(it->second, risk, 1e-3) << "entity " << entity << " risk diverged";
    }
}
