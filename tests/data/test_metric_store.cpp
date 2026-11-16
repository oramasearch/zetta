// SPDX-License-Identifier: MIT

// Labelled time series (#1231, docs/data/time-series.md): counters summed over a
// range, gauges aggregated, label filters and grouping, steps, and retention.
// Buckets from several nodes: RecordStoreCluster.ANodeThatJoinsLaterCatchesUpByAntiEntropy.
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "zetta/cluster/replication.hpp"
#include "zetta/data/metric_store.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::cluster::ReplicationManager;
using zetta::data::MetricDefinition;
using zetta::data::MetricKind;
using zetta::data::MetricPoint;
using zetta::data::MetricQuery;
using zetta::data::MetricStatus;
using zetta::data::MetricStore;

constexpr std::int64_t kMinute = 60'000;
constexpr std::int64_t kStart = 1'700'000'000'000 - 1'700'000'000'000 % kMinute;

MetricDefinition definition(std::string name, MetricKind kind,
                            std::optional<std::int64_t> retention_ms = std::nullopt) {
    MetricDefinition value;
    value.name = std::move(name);
    value.kind = kind;
    value.resolution_ms = kMinute;
    value.retention_ms = retention_ms;
    return value;
}

// A node's catalogue, record log (in memory) and metric store.
struct Stores {
    ReplicationManager replication{zetta::util::new_uuid()};
    zetta::data::RecordStore records{replication, replication.node_id(), {}};
    MetricStore metrics{replication, records, replication.node_id()};
    Stores() { records.open(); }
};

MetricPoint point(double value, std::int64_t at, std::map<std::string, std::string> labels) {
    MetricPoint result;
    result.value = value;
    result.at_ms = at;
    result.labels = std::move(labels);
    return result;
}

}  // namespace

TEST(MetricStore, CountersSumOverARangeByLabelAndStep) {
    Stores stores;
    MetricStore& store = stores.metrics;
    ASSERT_TRUE(store.create(definition("requests", MetricKind::Counter)).ok());
    const auto again = store.create(definition("requests", MetricKind::Counter));
    ASSERT_TRUE(again.ok());
    EXPECT_FALSE(again.created);
    EXPECT_EQ(store.create(definition("requests", MetricKind::Gauge)).status,
              MetricStatus::Conflict);

    // Two routes over three minutes.
    const auto recorded = store.record(
        "requests", {point(1, kStart + 1'000, {{"route", "/a"}, {"status", "200"}}),
                     point(2, kStart + 2'000, {{"route", "/a"}, {"status", "500"}}),
                     point(3, kStart + kMinute + 5, {{"route", "/a"}, {"status", "200"}}),
                     point(4, kStart + 2 * kMinute, {{"route", "/b"}, {"status", "200"}})});
    ASSERT_TRUE(recorded.ok()) << recorded.message;
    EXPECT_EQ(recorded.recorded, 4u);

    MetricQuery query;
    query.metric = "requests";
    query.from_ms = kStart;
    query.to_ms = kStart + 3 * kMinute;
    auto all = store.query(query);
    ASSERT_TRUE(all.ok()) << all.message;
    ASSERT_EQ(all.series.size(), 1u);
    EXPECT_DOUBLE_EQ(all.series[0].total, 10);
    ASSERT_EQ(all.series[0].points.size(), 1u);

    query.group_by = {"route"};
    query.step_ms = kMinute;
    auto by_route = store.query(query);
    ASSERT_TRUE(by_route.ok());
    ASSERT_EQ(by_route.series.size(), 2u);
    EXPECT_EQ(by_route.series[0].labels.at("route"), "/a");
    EXPECT_DOUBLE_EQ(by_route.series[0].total, 6);
    ASSERT_EQ(by_route.series[0].points.size(), 2u);
    EXPECT_EQ(by_route.series[0].points[0].first, kStart);
    EXPECT_DOUBLE_EQ(by_route.series[0].points[0].second, 3);
    EXPECT_DOUBLE_EQ(by_route.series[0].points[1].second, 3);
    EXPECT_DOUBLE_EQ(by_route.series[1].total, 4);

    query.labels = {{"status", "200"}};
    query.group_by.clear();
    query.step_ms.reset();
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 8);

    // The range is [from, to): the last minute is outside a two-minute window.
    query.labels.clear();
    query.to_ms = kStart + 2 * kMinute;
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 6);

    // Bad points write nothing.
    EXPECT_EQ(store.record("requests", {point(1, kStart, {}), point(-1, kStart, {})}).status,
              MetricStatus::InvalidInput);
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 6);
    EXPECT_EQ(store.record("missing", {point(1, kStart, {})}).status, MetricStatus::NotFound);
    query.aggregate = "max";
    EXPECT_EQ(store.query(query).status, MetricStatus::InvalidInput);
}

TEST(MetricStore, GaugesAggregateLastAverageMinimumAndMaximum) {
    Stores stores;
    MetricStore& store = stores.metrics;
    ASSERT_TRUE(store.create(definition("sessions", MetricKind::Gauge)).ok());
    ASSERT_TRUE(store.record("sessions", {point(10, kStart + 1, {{"pod", "a"}}),
                                          point(14, kStart + 9, {{"pod", "a"}}),
                                          point(12, kStart + 5, {{"pod", "a"}}),
                                          point(3, kStart + 2, {{"pod", "b"}})})
                    .ok());
    MetricQuery query;
    query.metric = "sessions";
    query.from_ms = kStart;
    query.to_ms = kStart + kMinute;
    // last: each series' latest reading (14 for a, 3 for b), summed.
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 17);
    query.aggregate = "max";
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 14);
    query.aggregate = "min";
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 3);
    query.aggregate = "avg";
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 39.0 / 4);
    query.aggregate = "sum";
    EXPECT_DOUBLE_EQ(store.query(query).series.at(0).total, 39);
    query.step_ms = kMinute / 2;
    EXPECT_EQ(store.query(query).status, MetricStatus::InvalidInput);
}

TEST(MetricStore, RetentionExpiresOldBucketsAndTheSweepRemovesThem) {
    Stores stores;
    ASSERT_TRUE(stores.metrics.create(definition("usage", MetricKind::Counter, 2 * kMinute)).ok());
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    ASSERT_TRUE(stores.metrics.record("usage", {point(1, now - 10 * kMinute, {}),
                                                point(2, now, {})})
                    .ok());
    MetricQuery query;
    query.metric = "usage";
    query.from_ms = now - 20 * kMinute;
    query.to_ms = now + kMinute;
    // The old bucket is past its retention: invisible at once, removed by the sweep.
    EXPECT_DOUBLE_EQ(stores.metrics.query(query).series.at(0).total, 2);
    EXPECT_EQ(stores.records.storage_stats().records, 2u);
    stores.records.sweep(now + 1);
    EXPECT_EQ(stores.records.storage_stats().records, 1u);
}
