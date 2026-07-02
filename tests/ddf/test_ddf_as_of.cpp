#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/as_of.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/storage/json_store.hpp"

// The DDF window / as-of (point-in-time) operator (#384): for each entity,
// aggregate only the rows whose timestamp is at or before the as-of cutoff — the
// leakage-free featurization primitive. These tests cover the acceptance (future
// rows excluded — a synthetic timestamped fixture AND the real churn fixture with
// its deliberate post-t0 events), the trailing-window lower bound, the per-entity
// dense output, the JSON boundary, and the error cases.
namespace {

namespace fs = std::filesystem;

using zetta::compute::ComputeStatusCode;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::ReduceOp;
using zetta::ddf::AsOfAggregate;
using zetta::ddf::AsOfResult;
using zetta::ddf::as_of_aggregate;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::load_data_frame;

// Build an in-memory store from a list of row docs.
zetta::JsonStore make_store(const std::vector<nlohmann::json>& rows) {
    zetta::JsonStore store;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.add("row-" + std::to_string(i), rows[i]);
    }
    return store;
}

// A numeric (entity, ts, value) schema — the shape the as-of op operates over.
DataFrameSchema event_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "entity", .dtype = DataType::Int64},
        ColumnSchema{.name = "ts", .dtype = DataType::Int64},
        ColumnSchema{.name = "value", .dtype = DataType::Float64},
    };
    return schema;
}

// --- the literal acceptance: as-of excludes future rows ----------------------

TEST(DdfAsOf, ExcludesFutureRowsAcrossEntities) {
    // Two entities, each with one row before and one row after the cutoff (ts=100).
    const zetta::JsonStore store = make_store({
        {{"entity", 1}, {"ts", 50}, {"value", 10.0}},   // <= as_of  (kept)
        {{"entity", 1}, {"ts", 150}, {"value", 99.0}},  // >  as_of  (the leak)
        {{"entity", 2}, {"ts", 80}, {"value", 5.0}},    // <= as_of  (kept)
        {{"entity", 2}, {"ts", 100}, {"value", 7.0}},   // == as_of  (kept, inclusive)
        {{"entity", 2}, {"ts", 200}, {"value", 88.0}},  // >  as_of  (the leak)
    });

    CpuDevice device;
    auto frame = load_data_frame(store, event_schema(), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;

    const AsOfAggregate spec{.entity_column = "entity",
                             .time_column = "ts",
                             .value_column = "value",
                             .as_of = 100.0,
                             .op = ReduceOp::Sum};
    const AsOfResult result = as_of_aggregate(frame.frame, spec);
    ASSERT_TRUE(result.ok()) << result.status.message;

    // Dense, ascending entity order.
    EXPECT_EQ(result.column.entities, (std::vector<std::int64_t>{1, 2}));
    // The post-cutoff values (99, 88) are excluded; the on-cutoff value (7) is kept.
    EXPECT_EQ(result.column.values, (std::vector<double>{10.0, 12.0}));
    EXPECT_EQ(result.column.counts, (std::vector<std::int64_t>{1, 2}));
}

TEST(DdfAsOf, EntityWithOnlyFutureRowsStaysDenseWithZeroCount) {
    const zetta::JsonStore store = make_store({
        {{"entity", 1}, {"ts", 50}, {"value", 10.0}},
        {{"entity", 7}, {"ts", 999}, {"value", 42.0}},  // entirely in the future
    });

    CpuDevice device;
    auto frame = load_data_frame(store, event_schema(), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;

    const AsOfResult result = as_of_aggregate(
        frame.frame, AsOfAggregate{.entity_column = "entity",
                                   .time_column = "ts",
                                   .value_column = "value",
                                   .as_of = 100.0,
                                   .op = ReduceOp::Sum});
    ASSERT_TRUE(result.ok()) << result.status.message;

    // Entity 7 still appears (dense), but contributes nothing: count 0, identity 0.
    EXPECT_EQ(result.column.entities, (std::vector<std::int64_t>{1, 7}));
    EXPECT_EQ(result.column.values, (std::vector<double>{10.0, 0.0}));
    EXPECT_EQ(result.column.counts, (std::vector<std::int64_t>{1, 0}));
}

// --- the trailing window (the "window function" half) ------------------------

TEST(DdfAsOf, WindowStartExcludesRowsBeforeTheLowerBound) {
    const zetta::JsonStore store = make_store({
        {{"entity", 1}, {"ts", 10}, {"value", 1.0}},   // before window  (excluded)
        {{"entity", 1}, {"ts", 60}, {"value", 2.0}},   // in window      (kept)
        {{"entity", 1}, {"ts", 90}, {"value", 3.0}},   // in window      (kept)
        {{"entity", 1}, {"ts", 150}, {"value", 9.0}},  // after as_of    (excluded)
    });

    CpuDevice device;
    auto frame = load_data_frame(store, event_schema(), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;

    // Trailing window [50, 100]: only ts 60 and 90 qualify.
    const AsOfResult result = as_of_aggregate(
        frame.frame, AsOfAggregate{.entity_column = "entity",
                                   .time_column = "ts",
                                   .value_column = "value",
                                   .as_of = 100.0,
                                   .window_start = 50.0,
                                   .op = ReduceOp::Sum});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.column.entities, (std::vector<std::int64_t>{1}));
    EXPECT_EQ(result.column.values, (std::vector<double>{5.0}));  // 2 + 3
    EXPECT_EQ(result.column.counts, (std::vector<std::int64_t>{2}));  // ts 60 and 90
}

// --- other reductions --------------------------------------------------------

TEST(DdfAsOf, MaximumReducesQualifyingRowsOnly) {
    const zetta::JsonStore store = make_store({
        {{"entity", 1}, {"ts", 10}, {"value", 3.0}},
        {{"entity", 1}, {"ts", 20}, {"value", 8.0}},
        {{"entity", 1}, {"ts", 999}, {"value", 100.0}},  // future high value (the leak)
    });
    CpuDevice device;
    auto frame = load_data_frame(store, event_schema(), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;

    const AsOfResult result = as_of_aggregate(
        frame.frame, AsOfAggregate{.entity_column = "entity",
                                   .time_column = "ts",
                                   .value_column = "value",
                                   .as_of = 100.0,
                                   .op = ReduceOp::Maximum});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.column.values, (std::vector<double>{8.0}));  // not 100
}

// --- the make-or-break: the real churn fixture's leakage trap ----------------

// Strip an ISO-8601 date "YYYY-MM-DD[Thh:mm:ssZ]" to a day-granularity integer
// (YYYYMMDD). Strictly monotonic for the fixture's dates, which is all the as-of
// comparison needs — and the fixture has no event exactly on the cutoff date.
std::int64_t iso_date_ordinal(const std::string& iso) {
    std::int64_t ordinal = 0;
    for (char c : iso) {
        if (c == 'T' || c == ' ') break;  // stop at the time portion
        if (c >= '0' && c <= '9') ordinal = ordinal * 10 + (c - '0');
    }
    return ordinal;
}

TEST(DdfAsOf, ChurnFixtureFeaturizeDropsPostT0Events) {
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    const zetta::ChurnDataset& dataset = loaded.dataset;

    // Featurize: convert the (string id, ISO timestamp) events into the numeric
    // (entity, ts, value) frame the as-of op operates over. A real featurize step
    // would do exactly this projection before windowing.
    std::map<std::string, std::int64_t> entity_index;  // account_id -> stable int key
    const auto key_for = [&](const std::string& account_id) -> std::int64_t {
        auto [it, inserted] =
            entity_index.try_emplace(account_id, static_cast<std::int64_t>(entity_index.size()));
        return it->second;
    };
    std::vector<nlohmann::json> rows;
    for (const zetta::ChurnEvent& event : dataset.events) {
        rows.push_back({{"entity", key_for(event.account_id)},
                        {"ts", iso_date_ordinal(event.event_ts)},
                        {"value", event.value}});
    }
    const zetta::JsonStore store = make_store(rows);

    CpuDevice device;
    auto frame = load_data_frame(store, event_schema(), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;

    const std::int64_t as_of = iso_date_ordinal(dataset.as_of_date);  // 2026-03-01

    // Ground truth computed independently from the dataset: per-entity sum + count
    // of events with event_ts <= as_of, and the same with NO cutoff (the leak).
    std::map<std::int64_t, double> expected_sum, leaky_sum;
    std::map<std::int64_t, std::int64_t> expected_count;
    std::size_t total_qualifying = 0;
    for (const zetta::ChurnEvent& event : dataset.events) {
        const std::int64_t key = entity_index.at(event.account_id);
        leaky_sum[key] += event.value;
        if (iso_date_ordinal(event.event_ts) <= as_of) {
            expected_sum[key] += event.value;
            ++expected_count[key];
            ++total_qualifying;
        }
    }
    // Sanity: the fixture deliberately carries post-t0 events (the leakage trap).
    ASSERT_LT(total_qualifying, dataset.events.size());

    const AsOfResult result = as_of_aggregate(
        frame.frame, AsOfAggregate{.entity_column = "entity",
                                   .time_column = "ts",
                                   .value_column = "value",
                                   .as_of = static_cast<double>(as_of),
                                   .op = ReduceOp::Sum});
    ASSERT_TRUE(result.ok()) << result.status.message;

    // Every account appears (dense), and the as-of sum matches the leakage-free
    // ground truth — never the leaky (all-events) sum where they differ.
    EXPECT_EQ(result.column.size(), entity_index.size());
    std::size_t counted = 0;
    bool saw_a_dropped_event = false;
    for (std::size_t i = 0; i < result.column.size(); ++i) {
        const std::int64_t key = result.column.entities[i];
        EXPECT_DOUBLE_EQ(result.column.values[i], expected_sum[key]) << "entity " << key;
        EXPECT_EQ(result.column.counts[i], expected_count[key]) << "entity " << key;
        counted += static_cast<std::size_t>(result.column.counts[i]);
        if (leaky_sum[key] != expected_sum[key]) saw_a_dropped_event = true;
    }
    // The op excludes the future rows: fewer rows counted than exist, and at least
    // one entity's windowed sum is strictly below its all-events sum.
    EXPECT_EQ(counted, total_qualifying);
    EXPECT_LT(counted, dataset.events.size());
    EXPECT_TRUE(saw_a_dropped_event);
}

// --- the JSON boundary -------------------------------------------------------

TEST(DdfAsOfJson, RoundTrips) {
    const AsOfAggregate spec{.entity_column = "account",
                             .time_column = "event_ts",
                             .value_column = "amount",
                             .as_of = 1700000000.0,
                             .window_start = 1690000000.0,
                             .op = ReduceOp::Maximum};
    const auto parsed = AsOfAggregate::from_json(spec.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, spec);
}

TEST(DdfAsOfJson, OmitsWindowStartWhenUnset) {
    const AsOfAggregate spec{.entity_column = "a",
                             .time_column = "t",
                             .value_column = "v",
                             .as_of = 5.0,
                             .op = ReduceOp::Sum};
    const nlohmann::json json = spec.to_json();
    EXPECT_FALSE(json.contains("window_start"));
    EXPECT_EQ(json.at("op"), "sum");
    const auto parsed = AsOfAggregate::from_json(json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->window_start.has_value());
}

TEST(DdfAsOfJson, RejectsBadInput) {
    using J = nlohmann::json;
    const J good{{"entity_column", "a"}, {"time_column", "t"},
                 {"value_column", "v"},  {"as_of", 5.0},
                 {"op", "sum"}};
    ASSERT_TRUE(AsOfAggregate::from_json(good).has_value());

    EXPECT_FALSE(AsOfAggregate::from_json(J::array()).has_value());  // not an object
    {  // missing a required field
        J bad = good;
        bad.erase("value_column");
        EXPECT_FALSE(AsOfAggregate::from_json(bad).has_value());
    }
    {  // empty column name
        J bad = good;
        bad["entity_column"] = "";
        EXPECT_FALSE(AsOfAggregate::from_json(bad).has_value());
    }
    {  // as_of not a number
        J bad = good;
        bad["as_of"] = "soon";
        EXPECT_FALSE(AsOfAggregate::from_json(bad).has_value());
    }
    {  // unknown op
        J bad = good;
        bad["op"] = "median";
        EXPECT_FALSE(AsOfAggregate::from_json(bad).has_value());
    }
    {  // window_start > as_of (empty window)
        J bad = good;
        bad["window_start"] = 10.0;  // > as_of (5)
        EXPECT_FALSE(AsOfAggregate::from_json(bad).has_value());
    }
    {  // unknown key
        J bad = good;
        bad["surprise"] = 1;
        EXPECT_FALSE(AsOfAggregate::from_json(bad).has_value());
    }
}

// --- error cases -------------------------------------------------------------

TEST(DdfAsOf, RejectsMissingColumn) {
    const zetta::JsonStore store = make_store({{{"entity", 1}, {"ts", 1}, {"value", 1.0}}});
    CpuDevice device;
    auto frame = load_data_frame(store, event_schema(), device);
    ASSERT_TRUE(frame.ok());

    const AsOfResult result = as_of_aggregate(
        frame.frame, AsOfAggregate{.entity_column = "nope",
                                   .time_column = "ts",
                                   .value_column = "value",
                                   .as_of = 1.0,
                                   .op = ReduceOp::Sum});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DdfAsOf, RejectsNonIntegerEntityColumn) {
    const zetta::JsonStore store = make_store({{{"entity", 1.5}, {"ts", 1}, {"value", 1.0}}});
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "entity", .dtype = DataType::Float64},  // not an integer key
        ColumnSchema{.name = "ts", .dtype = DataType::Int64},
        ColumnSchema{.name = "value", .dtype = DataType::Float64},
    };
    CpuDevice device;
    auto frame = load_data_frame(store, schema, device);
    ASSERT_TRUE(frame.ok());

    const AsOfResult result = as_of_aggregate(
        frame.frame, AsOfAggregate{.entity_column = "entity",
                                   .time_column = "ts",
                                   .value_column = "value",
                                   .as_of = 1.0,
                                   .op = ReduceOp::Sum});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DdfAsOf, RejectsWindowStartAboveAsOf) {
    const zetta::JsonStore store = make_store({{{"entity", 1}, {"ts", 1}, {"value", 1.0}}});
    CpuDevice device;
    auto frame = load_data_frame(store, event_schema(), device);
    ASSERT_TRUE(frame.ok());

    const AsOfResult result = as_of_aggregate(
        frame.frame, AsOfAggregate{.entity_column = "entity",
                                   .time_column = "ts",
                                   .value_column = "value",
                                   .as_of = 5.0,
                                   .window_start = 10.0,  // empty window
                                   .op = ReduceOp::Sum});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

}  // namespace
