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
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/storage/json_store.hpp"
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
}

}  // namespace zetta::testkit
