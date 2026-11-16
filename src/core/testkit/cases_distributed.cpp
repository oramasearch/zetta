// SPDX-License-Identifier: MIT

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cases.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/local_featurize_job.hpp"
#include "zetta/ddf/local_group_by_job.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_rebalance.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/query/service.hpp"
#include "zetta/data/rendezvous_placement.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/replication_strategy.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/data/table.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/util/uuid.hpp"

// Distributed & cluster cases — run on a real, fully-connected multi-node loopback
// cluster (TCP on 127.0.0.1), not a simulation: cluster membership formation, the
// collectives (allreduce over the signal plane), and a distributed group-by whose
// result is asserted identical to the single-node computation (the heart of "a
// distributed op gives the same answer however the data is sharded").
namespace zetta::testkit {
namespace {

using compute::CpuDevice;
using compute::DataType;

std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::size_t node_count_option(const TestOptions& options) {
    return static_cast<std::size_t>(
        std::min<long>(6, std::max<long>(1, options.get_int("nodes", 3))));
}

// Stand up N real nodes and confirm membership converges (every node sees N-1).
TestResult run_membership(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_count_option(options);
    context.emit("bringing up a " + std::to_string(nodes) + "-node loopback cluster");
    LoopbackCluster cluster(nodes);
    if (!cluster.connected()) {
        return TestResult::fail("cluster did not fully connect within the bring-up window");
    }
    TestResult result = TestResult::pass(
        std::to_string(nodes) + " nodes formed a fully-connected cluster");
    result.metric("nodes", std::to_string(nodes));
    for (std::size_t i = 0; i < cluster.size(); ++i) {
        result.metric("peers[" + std::to_string(i) + "]",
                      std::to_string(cluster.cluster(i).peer_count()));
    }
    return result;
}

// Every node contributes a vector; allreduce-sum returns the same global result on
// every node. Checked against the closed-form sum.
TestResult run_allreduce(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_count_option(options);
    context.emit("allreduce-sum across " + std::to_string(nodes) + " nodes");
    LoopbackCluster cluster(nodes);
    if (!cluster.connected()) return TestResult::fail("cluster did not converge");

    std::vector<std::unique_ptr<zetta::cluster::Collectives>> collectives;
    for (std::size_t i = 0; i < nodes; ++i) {
        collectives.push_back(std::make_unique<zetta::cluster::Collectives>(cluster.cluster(i)));
    }

    constexpr std::size_t width = 4;
    std::vector<std::optional<std::vector<double>>> results(nodes);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < nodes; ++i) {
        threads.emplace_back([&, i] {
            std::vector<double> local(width);
            for (std::size_t j = 0; j < width; ++j) {
                local[j] = static_cast<double>((i + 1) * (j + 1));
            }
            results[i] = collectives[i]->allreduce("tk.allreduce", local,
                                                   zetta::cluster::CollectiveOp::Sum);
        });
    }
    for (auto& thread : threads) thread.join();

    // sum over nodes of (i+1)*(j+1) = (j+1) * sum_{i=0..N-1}(i+1).
    const double node_sum = static_cast<double>(nodes * (nodes + 1) / 2);
    for (std::size_t i = 0; i < nodes; ++i) {
        if (!results[i]) return TestResult::fail("node " + std::to_string(i) +
                                                 " got no allreduce result");
        for (std::size_t j = 0; j < width; ++j) {
            const double expected = static_cast<double>(j + 1) * node_sum;
            if (std::abs((*results[i])[j] - expected) > 1e-6) {
                return TestResult::fail("node " + std::to_string(i) +
                                        " allreduce diverged at index " + std::to_string(j));
            }
        }
    }
    TestResult result = TestResult::pass(
        "every node received the identical allreduce-sum");
    result.metric("nodes", std::to_string(nodes))
        .metric("vector width", std::to_string(width))
        .metric("result[0]", number((*results[0])[0]));
    return result;
}

bool results_match(const ddf::GroupByResult& a, const ddf::GroupByResult& b) {
    if (a.keys.size() != b.keys.size() || a.values.size() != b.values.size()) return false;
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

// Shard rows across N nodes, run a distributed group-by, and assert every node's
// result equals the single-node computation over the whole dataset.
TestResult run_group_by_consistency(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_count_option(options);
    const std::size_t rows =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("rows", 2000)));
    const int groups = static_cast<int>(std::max<long>(1, options.get_int("groups", 5)));

    ddf::DataFrameSchema schema;
    schema.columns = {ddf::ColumnSchema{.name = "key", .dtype = DataType::Int64},
                      ddf::ColumnSchema{.name = "amount", .dtype = DataType::Float64}};
    ddf::GroupBySpec spec;
    spec.key = "key";
    spec.aggregates = {ddf::Aggregate{.op = ddf::AggregateOp::Count},
                       ddf::Aggregate{.op = ddf::AggregateOp::Sum, .column = "amount"},
                       ddf::Aggregate{.op = ddf::AggregateOp::Mean, .column = "amount"}};

    auto make_row = [&](std::size_t i) {
        return nlohmann::json{{"key", static_cast<std::int64_t>(i % groups)},
                              {"amount", static_cast<double>(i % 9) + 1.0}};
    };

    // Single-node ground truth over the full dataset.
    context.emit("computing the single-node ground truth");
    CpuDevice truth_device;
    zetta::storage::JsonStore full;
    for (std::size_t i = 0; i < rows; ++i) full.add("row-" + std::to_string(i), make_row(i));
    ddf::DataFrameResult full_frame = ddf::scan(full, schema, truth_device);
    if (!full_frame.ok()) return TestResult::fail("truth scan failed");
    const ddf::GroupByOutcome truth = ddf::group_by(full_frame.frame, spec);
    if (!truth.ok()) return TestResult::fail("truth group_by failed");

    // The same rows sharded round-robin across a real cluster.
    context.emit("sharding " + std::to_string(rows) + " rows across " +
                 std::to_string(nodes) + " nodes");
    LoopbackCluster cluster(nodes);
    if (!cluster.connected()) return TestResult::fail("cluster did not converge");

    std::vector<std::unique_ptr<CpuDevice>> devices(nodes);
    std::vector<zetta::storage::JsonStore> shards(nodes);
    for (std::size_t i = 0; i < nodes; ++i) devices[i] = std::make_unique<CpuDevice>();
    for (std::size_t i = 0; i < rows; ++i) {
        shards[i % nodes].add("row-" + std::to_string(i), make_row(i));
    }
    std::vector<ddf::DataFrame> frames;
    frames.reserve(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        ddf::DataFrameResult scanned = ddf::scan(shards[i], schema, *devices[i]);
        if (!scanned.ok()) return TestResult::fail("shard scan failed on node " +
                                                   std::to_string(i));
        frames.push_back(std::move(scanned.frame));
    }

    std::vector<std::unique_ptr<zetta::cluster::Collectives>> collectives;
    for (std::size_t i = 0; i < nodes; ++i) {
        collectives.push_back(std::make_unique<zetta::cluster::Collectives>(cluster.cluster(i)));
    }

    std::vector<ddf::GroupByOutcome> results(nodes);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < nodes; ++i) {
        threads.emplace_back([&, i] {
            results[i] = ddf::distributed_group_by(frames[i], spec, *collectives[i],
                                                   "tk.groupby");
        });
    }
    for (auto& thread : threads) thread.join();

    for (std::size_t i = 0; i < nodes; ++i) {
        if (!results[i].ok()) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " distributed_group_by failed: " +
                                    results[i].status.message);
        }
        if (!results_match(results[i].result, truth.result)) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " disagreed with the single-node ground truth");
        }
    }
    TestResult result = TestResult::pass(
        "the distributed group-by matched the single-node result on every node");
    result.metric("nodes", std::to_string(nodes))
        .metric("rows", std::to_string(rows))
        .metric("groups", std::to_string(truth.result.group_count()));
    return result;
}

// Compute-near-data (TC-11, #501): a DDF op runs over the partitions each node
// OWNS locally — no full-data pull. Each node shards its rows into its own
// partitions (write_partitioned places onto `{self}`), then runs the distributed
// group-by over the shard it loads from its own catalog + locally-held blob bodies
// (read near data, off local disk). Only the per-node partial crosses the wire.
// Two things are asserted: (1) the near-data result equals the single-node oracle
// on every node, and (2) the bytes the cluster MOVES over the collectives are ≪ the
// bytes it READS near data — the partials, not the dataset (instrumented via
// `Collectives::wire_bytes()`). The placement-aware replacement for the out-of-band
// shard path in `run_group_by_consistency`.
TestResult run_compute_near_data(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_count_option(options);
    const std::size_t rows =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("rows", 1200)));
    const int groups = static_cast<int>(std::max<long>(1, options.get_int("groups", 5)));
    const std::size_t partitions =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("partitions", 8)));

    ddf::DataFrameSchema schema;
    schema.columns = {ddf::ColumnSchema{.name = "segment", .dtype = DataType::Int64},
                      ddf::ColumnSchema{.name = "amount", .dtype = DataType::Float64}};
    ddf::GroupBySpec spec;
    spec.key = "segment";
    spec.aggregates = {ddf::Aggregate{.op = ddf::AggregateOp::Sum, .column = "amount"},
                       ddf::Aggregate{.op = ddf::AggregateOp::Count},
                       ddf::Aggregate{.op = ddf::AggregateOp::Mean, .column = "amount"}};

    auto make_row = [&](std::size_t i) {
        return nlohmann::json{{"segment", static_cast<std::int64_t>(i % groups)},
                              {"amount", static_cast<double>(i % 9) + 1.0}};
    };

    // Single-node ground truth over the whole dataset.
    context.emit("computing the single-node oracle over the full dataset");
    CpuDevice truth_device;
    zetta::storage::JsonStore full;
    for (std::size_t i = 0; i < rows; ++i) full.add("row-" + std::to_string(i), make_row(i));
    ddf::DataFrameResult full_frame = ddf::scan(full, schema, truth_device);
    if (!full_frame.ok()) return TestResult::fail("oracle scan failed");
    const ddf::GroupByOutcome truth = ddf::group_by(full_frame.frame, spec);
    if (!truth.ok()) return TestResult::fail("oracle group_by failed");

    // A real loopback cluster — each node a Node (its data + partitions) + a Cluster
    // (the compute plane).
    context.emit("bringing up a " + std::to_string(nodes) + "-node loopback cluster");
    LoopbackCluster cluster(nodes);
    if (!cluster.connected()) return TestResult::fail("cluster did not converge");

    // Each node writes its round-robin shard as partitions OWNED by itself, so the
    // near-data loader reads them off local disk — nothing is handed in out-of-band.
    context.emit("writing each node's shard into its own owned partitions");
    std::vector<std::string> table_ids(nodes);
    std::vector<std::unique_ptr<CpuDevice>> devices(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        devices[i] = std::make_unique<CpuDevice>();
        Node& node = cluster.node(i);
        std::shared_ptr<zetta::data::Database> database = node.create_database("data");
        if (!database) return TestResult::fail("create_database failed on node " +
                                               std::to_string(i));
        std::shared_ptr<zetta::data::Table> table = database->create_table("events");
        if (!table) return TestResult::fail("create_table failed on node " +
                                            std::to_string(i));
        table->set_partitioner(std::make_shared<zetta::data::RoundRobinPartitioner>(partitions));
        for (std::size_t r = 0; r < rows; ++r) {
            if (r % nodes == i) table->add_row(make_row(r));
        }
        const zetta::data::PartitionWriteResult written = write_partitioned(node, *database, *table);
        if (!written.ok()) return TestResult::fail("write_partitioned failed on node " +
                                                   std::to_string(i) + ": " + written.error);
        table_ids[i] = table->id();
    }

    // A coordinator per node over its cluster; the near-data op records a ComputeJob.
    std::vector<std::unique_ptr<compute::JobCoordinator>> coordinators(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        coordinators[i] =
            std::make_unique<compute::JobCoordinator>(cluster.cluster(i), /*workers=*/2);
        coordinators[i]->start();
    }

    // SPMD: a thread per node runs the near-data group-by concurrently (the
    // collectives need every participant present).
    context.emit("running the near-data group-by on every node");
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(nodes);
    std::vector<ddf::NearDataGroupByResult> results(nodes);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < nodes; ++i) {
        jobs[i] = cluster.node(i).create_job(zetta::compute::JobKind::Ddf);
        threads.emplace_back([&, i] {
            results[i] = ddf::run_near_data_group_by_job(
                *coordinators[i], cluster.node(i), *jobs[i], table_ids[i], schema, spec,
                *devices[i], "tk.near_data", std::chrono::seconds(8));
        });
    }
    for (auto& thread : threads) thread.join();

    std::uint64_t total_local_bytes = 0;
    std::uint64_t total_wire_bytes = 0;
    std::uint64_t total_rows_read = 0;
    for (std::size_t i = 0; i < nodes; ++i) {
        const ddf::NearDataGroupByResult& result = results[i];
        if (result.status != zetta::compute::JobStatus::Succeeded) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " near-data job did not succeed: " + jobs[i]->detail());
        }
        if (!results_match(result.result, truth.result)) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " disagreed with the single-node oracle");
        }
        if (result.partitions_read == 0) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " read no owned partitions");
        }
        total_local_bytes += result.local_bytes_read;
        total_wire_bytes += result.wire_bytes_moved;
        total_rows_read += result.rows_read;
    }

    // Every row was read near data (across the shards) exactly once.
    if (total_rows_read != rows) {
        return TestResult::fail("rows read near data (" + std::to_string(total_rows_read) +
                                ") != dataset rows (" + std::to_string(rows) + ")");
    }
    // The acceptance: bytes moved over the wire ≈ partials, NOT ≈ dataset. On more
    // than one node the cluster moves a small fraction of what it reads near data
    // (the partials scale with the group count, not the row count). A single-node
    // cluster moves nothing.
    if (nodes > 1 && !(total_wire_bytes * 4 < total_local_bytes)) {
        return TestResult::fail("moved too many bytes over the wire: moved=" +
                                std::to_string(total_wire_bytes) + " read-near-data=" +
                                std::to_string(total_local_bytes));
    }

    const double ratio =
        total_wire_bytes == 0
            ? 0.0
            : static_cast<double>(total_local_bytes) / static_cast<double>(total_wire_bytes);
    TestResult result = TestResult::pass(
        "each node computed over its local partitions; only partials crossed the "
        "wire, and the result matched the single-node oracle");
    result.metric("nodes", std::to_string(nodes))
        .metric("rows", std::to_string(rows))
        .metric("groups", std::to_string(truth.result.group_count()))
        .metric("read near data (bytes)", std::to_string(total_local_bytes))
        .metric("moved over wire (bytes)", std::to_string(total_wire_bytes))
        .metric("read / moved", number(ratio) + "x");
    return result;
}

// --- TC-10 (#500): partition placement + rebalance-on-node-join ------------------

// A node's placement read through the queryable `system.dataset_partitions` view,
// keyed by the catalog row id (`part:<uuid>`, the CRDT key — stable across nodes).
struct Placement {
    std::map<std::string, std::string> owners;  // part id -> "n1, n2"
    std::map<std::string, int> rf;              // part id -> owner_count
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;  // [start, end)

    std::size_t owned_by(const std::string& node_id) const {
        std::size_t count = 0;
        for (const auto& [part, owner_list] : owners) {
            if (owner_list.find(node_id) != std::string::npos) ++count;
        }
        return count;
    }
    // Do the ranges tile [0, total) with no gap / overlap (every row readable once)?
    bool tiles(std::uint64_t total) const {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted = ranges;
        std::sort(sorted.begin(), sorted.end());
        std::uint64_t expected = 0;
        for (const auto& [start, end] : sorted) {
            if (start != expected || end <= start) return false;
            expected = end;
        }
        return expected == total;
    }
};

std::optional<Placement> read_placement(Node& node) {
    query::QueryService service{node};
    const auto out = service.run(
        "SELECT partition_id, owner_nodes, owner_count, row_start, row_end "
        "FROM system.dataset_partitions;",
        "");
    if (!out.ok()) return std::nullopt;
    Placement placement;
    for (const nlohmann::json& row : out.rows) {
        const std::string part = row.at("partition_id").get<std::string>();
        placement.owners[part] = row.at("owner_nodes").get<std::string>();
        placement.rf[part] = row.at("owner_count").get<int>();
        placement.ranges.emplace_back(row.at("row_start").get<std::uint64_t>(),
                                      row.at("row_end").get<std::uint64_t>());
    }
    return placement;
}

// A partitioned table on a writer node converges its catalog to a peer (both
// replicated); when a node joins, the rebalance re-places ~1/N of the partitions
// and the new placement reflects on **every** node, with rows still readable. This
// is the cluster-level acceptance of TC-10 (#500) — the gtest counterpart is
// tests/test_partition_rebalance.cpp::CatalogConvergesOnEveryNodeAfterJoin.
TestResult run_partition_rebalance(const TestOptions& options, TestContext& context) {
    const std::size_t write_members =
        static_cast<std::size_t>(std::min<long>(6, std::max<long>(1,
            options.get_int("nodes", 3))));
    const std::size_t replication_factor =
        static_cast<std::size_t>(std::min<long>(static_cast<long>(write_members),
            std::max<long>(1, options.get_int("rf", 1))));
    const std::size_t rows =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("rows", 120)));

    // Two replicated nodes: the writer (A) and a peer (B). No TCP — convergence is
    // driven by merging A's CRDT document into B and reconciling on tick(), the
    // deterministic form of the gossip path.
    ScratchDir dir_a;
    ScratchDir dir_b;
    zetta::cluster::ReplicationManager repl_a{zetta::util::new_uuid()};
    zetta::cluster::ReplicationManager repl_b{zetta::util::new_uuid()};
    NodeConfig cfg_a;
    cfg_a.data_dir = dir_a.path().string();
    cfg_a.preflight = false;
    NodeConfig cfg_b;
    cfg_b.data_dir = dir_b.path().string();
    cfg_b.preflight = false;
    Node writer{cfg_a, &repl_a};
    Node peer{cfg_b, &repl_b};
    writer.start();
    peer.start();

    auto gossip = [&] {
        repl_b.merge(repl_a.save());
        peer.tick();
    };

    // The writer partitions a table over `write_members`.
    context.emit("partitioning " + std::to_string(rows) + " rows over " +
                 std::to_string(write_members) + " members (rf=" +
                 std::to_string(replication_factor) + ")");
    std::shared_ptr<zetta::data::Database> database = writer.create_database("warehouse");
    std::shared_ptr<zetta::data::Table> table =
        database->create_table("events", zetta::data::ReplicationStrategy::partitioned());
    table->set_partitioner(std::make_shared<zetta::data::RendezvousPlacement>(
        /*partition_count=*/64, replication_factor));
    for (std::size_t i = 0; i < rows; ++i) {
        table->add_row(nlohmann::json{{"id", "row-" + std::to_string(i)},
                                      {"n", static_cast<long>(i)}});
    }

    std::vector<std::string> members;
    for (std::size_t i = 0; i < write_members; ++i) {
        members.push_back("node-" + std::to_string(i));
    }
    const auto write =
        zetta::data::write_partitioned(writer, *database, *table, members);
    if (!write.ok()) return TestResult::fail("partitioned write failed: " + write.error);
    if (write.partitions.size() < 2) {
        return TestResult::fail("expected a spread of partitions, got " +
                                std::to_string(write.partitions.size()));
    }
    const std::uint64_t total_rows = write.rows_written;

    // The peer converges to the initial placement.
    gossip();
    const auto a_initial = read_placement(writer);
    const auto b_initial = read_placement(peer);
    if (!a_initial || !b_initial) return TestResult::fail("could not read the catalog");
    if (a_initial->owners != b_initial->owners) {
        return TestResult::fail("initial placement diverged across nodes");
    }

    // A node joins; the writer rebalances and the peer reconciles the new placement.
    const std::string joiner = "node-" + std::to_string(write_members);
    std::vector<std::string> grown = members;
    grown.push_back(joiner);
    context.emit(joiner + " joins — rebalancing between jobs");
    const zetta::data::RebalanceResult rebalanced =
        zetta::data::rebalance_partitions(writer, grown, members);
    if (rebalanced.moved == 0) {
        return TestResult::fail("the join moved no partitions");
    }
    gossip();

    // The acceptance: the new placement is identical on every node, the joiner took
    // ~1/N (not a full rehash), rf held, and the rows still tile [0, total).
    const auto a_after = read_placement(writer);
    const auto b_after = read_placement(peer);
    if (!a_after || !b_after) return TestResult::fail("could not read the catalog");
    if (a_after->owners != b_after->owners) {
        return TestResult::fail("the rebalanced catalog diverged across nodes");
    }
    if (a_after->owners.size() != a_initial->owners.size()) {
        return TestResult::fail("a rebalance lost or invented a partition");
    }
    const std::size_t owned_by_joiner = a_after->owned_by(joiner);
    if (owned_by_joiner == 0) {
        return TestResult::fail("the joiner took no partitions");
    }
    if (owned_by_joiner >= a_after->owners.size()) {
        return TestResult::fail("the join triggered a full rehash, not ~1/N");
    }
    if (b_after->owned_by(joiner) != owned_by_joiner) {
        return TestResult::fail("the joiner's share disagreed across nodes");
    }
    for (const auto& [part, count] : a_after->rf) {
        if (static_cast<std::size_t>(count) != replication_factor) {
            return TestResult::fail("replication factor not preserved for " + part);
        }
    }
    if (!a_after->tiles(total_rows) || !b_after->tiles(total_rows)) {
        return TestResult::fail("rows no longer tile [0, total) after the rebalance");
    }

    peer.stop();
    writer.stop();

    TestResult result = TestResult::pass(
        "the rebalanced catalog is identical on every node; the joiner took ~1/N "
        "and every row stays readable");
    result.metric("write members", std::to_string(write_members))
        .metric("replication factor", std::to_string(replication_factor))
        .metric("partitions", std::to_string(a_after->owners.size()))
        .metric("moved to joiner", std::to_string(owned_by_joiner))
        .metric("rows", std::to_string(total_rows));
    return result;
}

// --- near-data featurize over partitions (#532) ------------------------------

constexpr std::int64_t kFeaturizeAsOf = 1000;

// The saas-sales-conversations shape: each row is one conversation (entity) carrying
// already-engineered numeric columns + an event timestamp + a binary outcome.
ddf::DataFrameSchema engineered_schema() {
    ddf::DataFrameSchema schema;
    schema.columns = {
        ddf::ColumnSchema{.name = "conversation_id", .dtype = DataType::Int64},
        ddf::ColumnSchema{.name = "customer_engagement", .dtype = DataType::Float64},
        ddf::ColumnSchema{.name = "sales_effectiveness", .dtype = DataType::Float64},
        ddf::ColumnSchema{.name = "conversation_length", .dtype = DataType::Int64},
        ddf::ColumnSchema{.name = "event_ts", .dtype = DataType::Int64},
        ddf::ColumnSchema{.name = "outcome", .dtype = DataType::Int64}};
    return schema;
}

// A static featurize spec: engineered columns carried through, outcome the label,
// point-in-time filtered to event_ts <= as_of.
ddf::FeaturizeSpec engineered_spec() {
    ddf::FeaturizeSpec spec;
    spec.entity_column = "conversation_id";
    spec.time_column = "event_ts";
    spec.as_of = static_cast<double>(kFeaturizeAsOf);
    spec.account_features = {"customer_engagement", "sales_effectiveness",
                             "conversation_length"};
    spec.label_column = "outcome";
    return spec;
}

// One conversation; ~1 in 7 is AFTER the cutoff (event_ts > as_of) — the leakage trap
// that the point-in-time filter must exclude.
nlohmann::json make_conversation(std::size_t index) {
    const bool future = (index % 7 == 3);
    const std::int64_t ts = future ? kFeaturizeAsOf + 1 + static_cast<std::int64_t>(index % 50)
                                   : static_cast<std::int64_t>(index % 900);
    return nlohmann::json{
        {"conversation_id", static_cast<std::int64_t>(index)},
        {"customer_engagement", static_cast<double>(index % 100) / 100.0},
        {"sales_effectiveness", static_cast<double>((index * 7) % 100) / 100.0},
        {"conversation_length", static_cast<std::int64_t>(index % 40) + 1},
        {"event_ts", ts},
        {"outcome", static_cast<std::int64_t>(index % 3 == 0 ? 1 : 0)}};
}

bool feature_tables_equal(const ddf::FeatureTable& a, const ddf::FeatureTable& b) {
    if (a.entities != b.entities || a.feature_names != b.feature_names) return false;
    if (a.label_name != b.label_name || a.labels.size() != b.labels.size()) return false;
    for (const std::string& name : b.feature_names) {
        const std::vector<double>* x = a.feature(name);
        const std::vector<double>* y = b.feature(name);
        if (!x || !y || x->size() != y->size()) return false;
        for (std::size_t i = 0; i < x->size(); ++i) {
            if (std::abs((*x)[i] - (*y)[i]) > 1e-6) return false;
        }
    }
    for (std::size_t i = 0; i < a.labels.size(); ++i) {
        if (std::abs(a.labels[i] - b.labels[i]) > 1e-6) return false;
    }
    return true;
}

// Featurize engineered columns compute-near-data over a partitioned table: each node
// owns a shard (its partitions), point-in-time featurizes it, and the global feature
// table is asserted identical to the single-node oracle on every node, leakage-free.
TestResult run_featurize_partitions(const TestOptions& options, TestContext& context) {
    const std::size_t nodes = node_count_option(options);
    const std::size_t rows =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("rows", 300)));

    // The single-node featurize oracle over the whole dataset.
    context.emit("computing the single-node featurize oracle");
    CpuDevice truth_device;
    zetta::storage::JsonStore full;
    std::size_t excluded = 0;
    for (std::size_t i = 0; i < rows; ++i) {
        full.add("row-" + std::to_string(i), make_conversation(i));
        if (i % 7 == 3) ++excluded;
    }
    ddf::DataFrameResult full_frame =
        ddf::load_data_frame(full, engineered_schema(), truth_device);
    if (!full_frame.ok()) return TestResult::fail("oracle scan failed: " + full_frame.status.message);
    const ddf::FeaturizeResult oracle = ddf::point_in_time_featurize(
        full_frame.frame, full_frame.frame, engineered_spec());
    if (!oracle.ok()) return TestResult::fail("oracle featurize failed: " + oracle.status.message);
    if (oracle.table.row_count() >= rows) {
        return TestResult::fail("the leakage trap did not fire (no post-cutoff rows excluded)");
    }

    // Each node holds its shard as its OWN partitions (every conversation on one node).
    context.emit("partitioning " + std::to_string(rows) + " conversations across " +
                 std::to_string(nodes) + " nodes");
    LoopbackCluster cluster(nodes);
    if (!cluster.connected()) return TestResult::fail("cluster did not converge");

    std::vector<std::string> table_ids(nodes);
    std::vector<std::unique_ptr<CpuDevice>> devices(nodes);
    std::vector<std::unique_ptr<compute::JobCoordinator>> coordinators(nodes);
    for (std::size_t i = 0; i < nodes; ++i) {
        Node& node = cluster.node(i);
        // A per-node database name avoids a name collision in the replicated catalog
        // (the cluster is already connected); isolation is by table id + owner anyway.
        auto database = node.create_database("data_" + std::to_string(i));
        if (!database) return TestResult::fail("node " + std::to_string(i) + " has no database");
        auto table = database->create_table("conversations");
        if (!table) return TestResult::fail("node " + std::to_string(i) + " has no table");
        table->set_partitioner(std::make_shared<zetta::data::RoundRobinPartitioner>(4));
        for (std::size_t r = i; r < rows; r += nodes) table->add_row(make_conversation(r));
        const zetta::data::PartitionWriteResult written = write_partitioned(node, *database, *table);
        if (!written.ok()) {
            return TestResult::fail("node " + std::to_string(i) + " partition write failed: " +
                                    written.error);
        }
        table_ids[i] = table->id();
        devices[i] = std::make_unique<CpuDevice>();
        coordinators[i] = std::make_unique<compute::JobCoordinator>(cluster.cluster(i), 2);
    }
    for (auto& coordinator : coordinators) coordinator->start();

    const ddf::FeaturizeSpec spec = engineered_spec();
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(nodes);
    std::vector<ddf::NearDataFeaturizeResult> results(nodes);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < nodes; ++i) {
        jobs[i] = cluster.node(i).create_job(zetta::compute::JobKind::Featurize);
        threads.emplace_back([&, i] {
            results[i] = ddf::run_near_data_featurize_job(
                *coordinators[i], cluster.node(i), *jobs[i], table_ids[i],
                engineered_schema(), spec, *devices[i], "tk.featurize",
                std::chrono::seconds(10));
        });
    }
    for (auto& thread : threads) thread.join();
    for (auto& coordinator : coordinators) coordinator->stop();

    std::uint64_t total_rows_read = 0;
    for (std::size_t i = 0; i < nodes; ++i) {
        if (results[i].status != zetta::compute::JobStatus::Succeeded) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " featurize job failed: " + jobs[i]->detail());
        }
        if (!feature_tables_equal(results[i].table, oracle.table)) {
            return TestResult::fail("node " + std::to_string(i) +
                                    " disagreed with the single-node oracle");
        }
        total_rows_read += results[i].rows_read;
    }
    if (total_rows_read != rows) {
        return TestResult::fail("not every conversation was read near data exactly once");
    }

    TestResult result = TestResult::pass(
        "near-data featurize matched the single-node oracle on every node, leakage-free");
    result.metric("nodes", std::to_string(nodes))
        .metric("conversations", std::to_string(rows))
        .metric("feature rows", std::to_string(oracle.table.row_count()))
        .metric("excluded (post-cutoff)", std::to_string(excluded))
        .metric("features", std::to_string(oracle.table.feature_count()));
    return result;
}

}  // namespace

void register_distributed_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "cluster.membership",
        .category = TestCategory::Distributed,
        .name = "Cluster membership",
        .description = "Bring up an N-node loopback cluster over TCP and confirm "
                       "membership converges (every node sees the other N-1).",
        .options = {TestOption::integer("nodes", "Nodes", 3, 1, 6)},
        .run = run_membership,
    });
    suite.add(TestCase{
        .id = "cluster.allreduce",
        .category = TestCategory::Distributed,
        .name = "Collectives: allreduce",
        .description = "Each node contributes a vector; an allreduce-sum over the "
                       "signal plane returns the identical global result on every "
                       "node, checked against the closed form.",
        .options = {TestOption::integer("nodes", "Nodes", 3, 1, 6)},
        .run = run_allreduce,
    });
    suite.add(TestCase{
        .id = "cluster.group_by_consistency",
        .category = TestCategory::Distributed,
        .name = "Distributed group-by == single-node",
        .description = "Shard rows across a real cluster, run a distributed "
                       "group-by, and assert every node's result equals the "
                       "single-node computation over the whole dataset.",
        .options = {TestOption::integer("nodes", "Nodes", 3, 1, 6),
                    TestOption::integer("rows", "Rows", 2000, 1, 1000000),
                    TestOption::integer("groups", "Distinct groups", 5, 1, 512)},
        .run = run_group_by_consistency,
    });
    suite.add(TestCase{
        .id = "cluster.compute_near_data",
        .category = TestCategory::Distributed,
        .name = "Compute near data (TC-11)",
        .description = "Each node runs a DDF group-by over the partitions it owns "
                       "locally (read off local disk) — no full-data pull. Asserts "
                       "the near-data result equals the single-node oracle on every "
                       "node, and that the bytes moved over the wire are the partials "
                       "(≈ group count), not the dataset (≈ row count).",
        .options = {TestOption::integer("nodes", "Nodes", 3, 1, 6),
                    TestOption::integer("rows", "Rows", 1200, 1, 1000000),
                    TestOption::integer("groups", "Distinct groups", 5, 1, 512),
                    TestOption::integer("partitions", "Partitions per node", 8, 1, 256)},
        .run = run_compute_near_data,
    });
    suite.add(TestCase{
        .id = "cluster.partition_rebalance",
        .category = TestCategory::Distributed,
        .name = "Partition rebalance on node join (TC-10)",
        .description = "Partition a table across the cluster; when a node joins, the "
                       "rendezvous rebalance re-places ~1/N of the partitions and the "
                       "new placement reflects in system.dataset_partitions on every "
                       "node, with the replication factor preserved and every row "
                       "still readable exactly once.",
        .options = {TestOption::integer("nodes", "Write members", 3, 1, 6),
                    TestOption::integer("rf", "Replication factor", 1, 1, 3),
                    TestOption::integer("rows", "Rows", 120, 1, 1000000)},
        .run = run_partition_rebalance,
    });
    suite.add(TestCase{
        .id = "cluster.featurize_partitions",
        .category = TestCategory::Distributed,
        .name = "Featurize over partitions == single-node",
        .description = "Point-in-time featurize engineered numeric columns (the "
                       "saas-sales-conversations shape) compute-near-data over a "
                       "partitioned table: each node owns a shard (its partitions), "
                       "featurizes it (event_ts <= as_of, no leakage), and the global "
                       "feature table is asserted identical to the single-node oracle "
                       "on every node (#532).",
        .options = {TestOption::integer("nodes", "Nodes", 3, 1, 6),
                    TestOption::integer("rows", "Conversations", 300, 1, 1000000)},
        .run = run_featurize_partitions,
    });
}

}  // namespace zetta::testkit
