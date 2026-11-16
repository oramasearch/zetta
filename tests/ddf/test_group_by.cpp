// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

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
#include "zetta/ddf/group_by.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

// DDF distributed groupby-aggregate (#383): groupby + count/sum/min/max/mean over a
// frame's columns, distributed via partial-per-node + gather/merge/broadcast. These
// tests cover the JSON boundary, the single-node op across dtypes, the mergeable
// partial + its wire form, and — the acceptance — a multi-node loopback cluster
// producing results identical to the single-node op over the whole frame.
namespace {

using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::storage::JsonStore;
using zetta::Node;
using zetta::NodeConfig;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::JobCoordinator;
using zetta::compute::JobRound;
using zetta::ddf::Aggregate;
using zetta::ddf::AggregateOp;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::distributed_group_by;
using zetta::ddf::group_by;
using zetta::ddf::GroupByPartial;
using zetta::ddf::GroupByResult;
using zetta::ddf::GroupBySpec;
using zetta::ddf::load_data_frame;
using zetta::ddf::local_partial;
using zetta::test::TempDir;

// Build an in-memory store from a list of row docs.
JsonStore make_store(const std::vector<nlohmann::json>& rows) {
    JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

// The fixture schema: an integer group key + an integer measure.
DataFrameSchema churn_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "region", .dtype = DataType::Int64},
        ColumnSchema{.name = "amount", .dtype = DataType::Int64},
    };
    return schema;
}

// count, sum/min/max/mean(amount) grouped by region — the full aggregate set.
GroupBySpec churn_spec() {
    GroupBySpec spec;
    spec.key = "region";
    spec.aggregates = {
        Aggregate{.op = AggregateOp::Count},
        Aggregate{.op = AggregateOp::Sum, .column = "amount"},
        Aggregate{.op = AggregateOp::Min, .column = "amount"},
        Aggregate{.op = AggregateOp::Max, .column = "amount"},
        Aggregate{.op = AggregateOp::Mean, .column = "amount"},
    };
    return spec;
}

// The fixture's three shards (one per node). Each shard sees all three regions, so
// the merge must combine the same key arriving from different nodes.
//   region 10: amounts 1,3,6  -> count 3, sum 10, min 1, max 6, mean 10/3
//   region 20: amounts 2,4,8  -> count 3, sum 14, min 2, max 8, mean 14/3
//   region 30: amounts 5,7,9  -> count 3, sum 21, min 5, max 9, mean 7
std::vector<std::vector<nlohmann::json>> fixture_shards() {
    return {
        {{{"region", 10}, {"amount", 1}},
         {{"region", 20}, {"amount", 2}},
         {{"region", 10}, {"amount", 3}}},
        {{{"region", 20}, {"amount", 4}},
         {{"region", 30}, {"amount", 5}},
         {{"region", 10}, {"amount", 6}}},
        {{{"region", 30}, {"amount", 7}},
         {{"region", 20}, {"amount", 8}},
         {{"region", 30}, {"amount", 9}}},
    };
}

void expect_results_equal(const GroupByResult& actual,
                          const GroupByResult& expected) {
    ASSERT_EQ(actual.key_name, expected.key_name);
    ASSERT_EQ(actual.aggregate_names, expected.aggregate_names);
    ASSERT_EQ(actual.keys, expected.keys);
    ASSERT_EQ(actual.values.size(), expected.values.size());
    for (std::size_t aggregate = 0; aggregate < expected.values.size();
         ++aggregate) {
        ASSERT_EQ(actual.values[aggregate].size(),
                  expected.values[aggregate].size())
            << "aggregate " << aggregate;
        for (std::size_t group = 0; group < expected.values[aggregate].size();
             ++group) {
            EXPECT_DOUBLE_EQ(actual.values[aggregate][group],
                             expected.values[aggregate][group])
                << "aggregate " << aggregate << " group " << group;
        }
    }
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
        NodeConfig config;
        config.data_dir = dir.path().string();
        return config;
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

}  // namespace

// --- AggregateOp names -------------------------------------------------------

TEST(GroupByOp, NamesRoundTrip) {
    for (const AggregateOp op : {AggregateOp::Count, AggregateOp::Sum,
                                 AggregateOp::Min, AggregateOp::Max,
                                 AggregateOp::Mean}) {
        EXPECT_EQ(zetta::ddf::aggregate_op_from_string(zetta::ddf::to_string(op)),
                  op);
    }
    EXPECT_FALSE(zetta::ddf::aggregate_op_from_string("median"));
}

TEST(GroupByOp, DefaultOutputNames) {
    EXPECT_EQ((Aggregate{.op = AggregateOp::Count}).output_name(), "count");
    EXPECT_EQ((Aggregate{.op = AggregateOp::Sum, .column = "amount"}).output_name(),
              "sum_amount");
    EXPECT_EQ(
        (Aggregate{.op = AggregateOp::Mean, .column = "x", .output = "avg_x"})
            .output_name(),
        "avg_x");
}

// --- JSON boundary -----------------------------------------------------------

TEST(GroupBySpecJson, RoundTrips) {
    const GroupBySpec spec = churn_spec();
    const std::optional<GroupBySpec> parsed = GroupBySpec::from_json(spec.to_json());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->key, "region");
    ASSERT_EQ(parsed->aggregates.size(), 5u);
    EXPECT_EQ(parsed->aggregates[0].op, AggregateOp::Count);
    EXPECT_EQ(parsed->aggregates[1].op, AggregateOp::Sum);
    EXPECT_EQ(parsed->aggregates[1].column, "amount");
}

TEST(GroupBySpecJson, RejectsMalformed) {
    using nlohmann::json;
    EXPECT_FALSE(GroupBySpec::from_json(json::array()));              // not an object
    EXPECT_FALSE(GroupBySpec::from_json(json{{"aggregates", json::array(
                                                {{{"op", "count"}}})}}));  // no key
    EXPECT_FALSE(GroupBySpec::from_json(json{{"key", ""},
                                             {"aggregates", json::array(
                                                 {{{"op", "count"}}})}}));  // empty key
    EXPECT_FALSE(GroupBySpec::from_json(
        json{{"key", "region"}, {"aggregates", json::array()}}));  // empty aggregates
    EXPECT_FALSE(GroupBySpec::from_json(
        json{{"key", "region"},
             {"aggregates", json::array({{{"op", "median"}}})}}));  // unknown op
    // sum needs a column.
    EXPECT_FALSE(GroupBySpec::from_json(
        json{{"key", "region"},
             {"aggregates", json::array({{{"op", "sum"}}})}}));
    // count needs no column — accepted.
    EXPECT_TRUE(GroupBySpec::from_json(
        json{{"key", "region"},
             {"aggregates", json::array({{{"op", "count"}}})}}));
}

// --- Single-node group-by ----------------------------------------------------

TEST(GroupBy, SingleNodeComputesEveryAggregate) {
    std::vector<nlohmann::json> rows;
    for (const auto& shard : fixture_shards()) {
        for (const auto& row : shard) rows.push_back(row);
    }
    const JsonStore store = make_store(rows);

    CpuDevice device;
    auto loaded = load_data_frame(store, churn_schema(), device);
    ASSERT_TRUE(loaded.ok()) << loaded.status.message;

    const auto outcome = group_by(loaded.frame, churn_spec());
    ASSERT_TRUE(outcome.ok()) << outcome.status.message;
    const GroupByResult& result = outcome.result;

    EXPECT_EQ(result.key_name, "region");
    EXPECT_EQ(result.keys, (std::vector<double>{10, 20, 30}));  // ascending
    EXPECT_EQ(result.aggregate_names,
              (std::vector<std::string>{"count", "sum_amount", "min_amount",
                                        "max_amount", "mean_amount"}));

    EXPECT_EQ(*result.value("count"), (std::vector<double>{3, 3, 3}));
    EXPECT_EQ(*result.value("sum_amount"), (std::vector<double>{10, 14, 21}));
    EXPECT_EQ(*result.value("min_amount"), (std::vector<double>{1, 2, 5}));
    EXPECT_EQ(*result.value("max_amount"), (std::vector<double>{6, 8, 9}));
    const std::vector<double>& mean = *result.value("mean_amount");
    EXPECT_DOUBLE_EQ(mean[0], 10.0 / 3.0);
    EXPECT_DOUBLE_EQ(mean[1], 14.0 / 3.0);
    EXPECT_DOUBLE_EQ(mean[2], 7.0);
}

TEST(GroupBy, ReportsMissingColumns) {
    const JsonStore store = make_store({{{"region", 1}, {"amount", 2}}});
    CpuDevice device;
    auto loaded = load_data_frame(store, churn_schema(), device);
    ASSERT_TRUE(loaded.ok());

    GroupBySpec bad_key;
    bad_key.key = "nope";
    bad_key.aggregates = {Aggregate{.op = AggregateOp::Count}};
    EXPECT_FALSE(group_by(loaded.frame, bad_key).ok());

    GroupBySpec bad_column;
    bad_column.key = "region";
    bad_column.aggregates = {Aggregate{.op = AggregateOp::Sum, .column = "nope"}};
    EXPECT_FALSE(group_by(loaded.frame, bad_column).ok());
}

// --- The mergeable partial ---------------------------------------------------

TEST(GroupByPartial, WireRoundTrips) {
    std::vector<nlohmann::json> rows;
    for (const auto& row : fixture_shards()[0]) rows.push_back(row);
    const JsonStore store = make_store(rows);
    CpuDevice device;
    auto loaded = load_data_frame(store, churn_schema(), device);
    ASSERT_TRUE(loaded.ok());

    const auto outcome = local_partial(loaded.frame, churn_spec());
    ASSERT_TRUE(outcome.ok()) << outcome.status.message;

    const std::vector<double> wire = outcome.partial.to_wire();
    const std::optional<GroupByPartial> back =
        GroupByPartial::from_wire(outcome.partial.ops, wire);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->groups, outcome.partial.groups);
    EXPECT_EQ(back->ops, outcome.partial.ops);
}

TEST(GroupByPartial, FromWireRejectsMalformed) {
    const std::vector<AggregateOp> ops = {AggregateOp::Sum};  // width = key+count+1
    EXPECT_FALSE(GroupByPartial::from_wire(ops, {}));            // too short
    EXPECT_FALSE(GroupByPartial::from_wire(ops, {2, 0}));        // k mismatch (2 != 1)
    EXPECT_FALSE(GroupByPartial::from_wire(ops, {1, 1}));        // truncated group
    // one valid group: k=1, g=1, (key=5, count=2, sum=9)
    EXPECT_TRUE(GroupByPartial::from_wire(ops, {1, 1, 5, 2, 9}));
}

TEST(GroupByPartial, MergeCombinesSharedKeys) {
    const GroupBySpec spec = churn_spec();
    const auto shards = fixture_shards();

    // Merge the three per-shard partials by hand and finalize — must match the
    // single-node result.
    CpuDevice device_a, device_b, device_c;
    CpuDevice* devices[] = {&device_a, &device_b, &device_c};
    std::vector<JsonStore> stores;
    std::vector<zetta::ddf::DataFrame> frames;
    stores.reserve(3);
    frames.reserve(3);
    GroupByPartial merged;
    merged.ops = {AggregateOp::Count, AggregateOp::Sum, AggregateOp::Min,
                  AggregateOp::Max, AggregateOp::Mean};
    for (std::size_t index = 0; index < shards.size(); ++index) {
        stores.push_back(make_store(shards[index]));
        auto loaded = load_data_frame(stores.back(), churn_schema(), *devices[index]);
        ASSERT_TRUE(loaded.ok());
        frames.push_back(std::move(loaded.frame));
        const auto outcome = local_partial(frames.back(), spec);
        ASSERT_TRUE(outcome.ok());
        EXPECT_TRUE(merged.merge(outcome.partial));
    }

    const GroupByResult result = merged.finalize(spec);
    EXPECT_EQ(result.keys, (std::vector<double>{10, 20, 30}));
    EXPECT_EQ(*result.value("sum_amount"), (std::vector<double>{10, 14, 21}));
    EXPECT_EQ(*result.value("min_amount"), (std::vector<double>{1, 2, 5}));
    EXPECT_EQ(*result.value("max_amount"), (std::vector<double>{6, 8, 9}));
}

// --- The acceptance: distributed == single-node ------------------------------

TEST(GroupBy, DistributedMatchesSingleNode) {
    // The reference: the single-node group-by over the whole frame.
    std::vector<nlohmann::json> all_rows;
    for (const auto& shard : fixture_shards()) {
        for (const auto& row : shard) all_rows.push_back(row);
    }
    const JsonStore reference_store = make_store(all_rows);
    CpuDevice reference_device;
    auto reference_loaded =
        load_data_frame(reference_store, churn_schema(), reference_device);
    ASSERT_TRUE(reference_loaded.ok());
    const auto reference = group_by(reference_loaded.frame, churn_spec());
    ASSERT_TRUE(reference.ok()) << reference.status.message;

    // A 3-node loopback cluster; controlled ids so "node-a" is the collective root.
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    constexpr std::uint16_t kBasePort = 46400;
    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<ComputeNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds,
            /*workers=*/2));
    }

    for (auto& node : nodes) node->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    }));
    for (auto& node : nodes) node->coordinator.start();

    const auto shards = fixture_shards();
    std::vector<GroupByResult> results(nodes.size());
    std::vector<JobStatus> statuses(nodes.size(), JobStatus::Queued);
    std::vector<std::shared_ptr<ComputeJob>> jobs(nodes.size());

    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Ddf);
        threads.emplace_back([&, index] {
            // Each node loads only its own shard into a frame.
            CpuDevice device;
            const JsonStore store = make_store(shards[index]);
            auto loaded = load_data_frame(store, churn_schema(), device);
            ASSERT_TRUE(loaded.ok());
            statuses[index] = nodes[index]->coordinator.run(
                *jobs[index], [&](JobRound& round) -> JobCoordinator::Outcome {
                    const auto outcome = distributed_group_by(
                        loaded.frame, churn_spec(), round, "groupby", 5s);
                    if (!outcome.ok()) {
                        return {.ok = false, .detail = outcome.status.message};
                    }
                    results[index] = std::move(outcome.result);
                    return {.ok = true,
                            .detail = "groups=" +
                                      std::to_string(results[index].group_count())};
                });
        });
    }
    for (auto& thread : threads) thread.join();

    // Every node succeeded and produced the same result as the single-node op.
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        EXPECT_EQ(statuses[index], JobStatus::Succeeded) << "node " << index;
        SCOPED_TRACE("node " + std::to_string(index));
        expect_results_equal(results[index], reference.result);
    }

    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
}
