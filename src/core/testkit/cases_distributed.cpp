#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cases.hpp"
#include "zetta/cluster.hpp"
#include "zetta/collectives.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/database.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/local_group_by_job.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/node.hpp"
#include "zetta/partition_writer.hpp"
#include "zetta/partitioner.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/table.hpp"
#include "zetta/testkit/harness.hpp"
#include "zetta/testkit/test_suite.hpp"

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

    std::vector<std::unique_ptr<Collectives>> collectives;
    for (std::size_t i = 0; i < nodes; ++i) {
        collectives.push_back(std::make_unique<Collectives>(cluster.cluster(i)));
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
                                                   CollectiveOp::Sum);
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
    JsonStore full;
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
    std::vector<JsonStore> shards(nodes);
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

    std::vector<std::unique_ptr<Collectives>> collectives;
    for (std::size_t i = 0; i < nodes; ++i) {
        collectives.push_back(std::make_unique<Collectives>(cluster.cluster(i)));
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
    JsonStore full;
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
        std::shared_ptr<Database> database = node.create_database("data");
        if (!database) return TestResult::fail("create_database failed on node " +
                                               std::to_string(i));
        std::shared_ptr<Table> table = database->create_table("events");
        if (!table) return TestResult::fail("create_table failed on node " +
                                            std::to_string(i));
        table->set_partitioner(std::make_shared<RoundRobinPartitioner>(partitions));
        for (std::size_t r = 0; r < rows; ++r) {
            if (r % nodes == i) table->add_row(make_row(r));
        }
        const PartitionWriteResult written = write_partitioned(node, *database, *table);
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
    std::vector<std::shared_ptr<ComputeJob>> jobs(nodes);
    std::vector<ddf::NearDataGroupByResult> results(nodes);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < nodes; ++i) {
        jobs[i] = cluster.node(i).create_job(JobKind::Ddf);
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
        if (result.status != JobStatus::Succeeded) {
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
}

}  // namespace zetta::testkit
