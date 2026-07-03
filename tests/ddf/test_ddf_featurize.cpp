#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/featurize_job.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

// Point-in-time featurization on DDF (#388): express the churn churn_features.sql
// shape as a DDF graph (join + groupby-aggregate + as-of window) and featurize the
// synthetic churn fixture into a point-in-time feature table — every feature uses
// event_ts <= as_of, with a leakage assertion that the deliberate post-t0 events
// never enter a feature. Covers the JSON boundary, the single-node op (synthetic +
// the real fixture), to_data_frame, and the distributed `featurize` ComputeJob
// (single-node == multi-node).
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::JobCoordinator;
using zetta::ddf::AggregateOp;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::FeaturizeSpec;
using zetta::ddf::FeatureTable;
using zetta::ddf::load_data_frame;
using zetta::ddf::point_in_time_featurize;
using zetta::ddf::run_featurize_job;
using zetta::ddf::WindowedFeature;
using zetta::test::TempDir;
using Json = nlohmann::json;

zetta::storage::JsonStore make_store(const std::vector<Json>& rows) {
    zetta::storage::JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

// =====================================================================
// JSON boundary
// =====================================================================

FeaturizeSpec sample_spec() {
    FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = 60.0;
    spec.event_features = {
        WindowedFeature{.name = "failed_payments",
                        .agg = AggregateOp::Count,
                        .type_column = "type",
                        .type_value = 3.0},
        WindowedFeature{.name = "support_sum",
                        .agg = AggregateOp::Sum,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 4.0},
        WindowedFeature{.name = "recent_logins",
                        .agg = AggregateOp::Count,
                        .window_start = 30.0,
                        .type_column = "type",
                        .type_value = 1.0},
    };
    spec.account_features = {"mrr", "seats"};
    spec.label_column = "churned";
    return spec;
}

TEST(DdfFeaturizeJson, RoundTrips) {
    const FeaturizeSpec spec = sample_spec();
    const auto parsed = FeaturizeSpec::from_json(spec.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->entity_column, spec.entity_column);
    EXPECT_EQ(parsed->time_column, spec.time_column);
    EXPECT_DOUBLE_EQ(parsed->as_of, spec.as_of);
    ASSERT_EQ(parsed->event_features.size(), spec.event_features.size());
    EXPECT_EQ(parsed->event_features[1].agg, AggregateOp::Sum);
    EXPECT_EQ(parsed->event_features[1].value_column, "value");
    EXPECT_DOUBLE_EQ(*parsed->event_features[2].window_start, 30.0);
    EXPECT_EQ(parsed->account_features, spec.account_features);
    EXPECT_EQ(parsed->label_column, "churned");
}

TEST(DdfFeaturizeJson, OmitsEmptyOptionals) {
    FeaturizeSpec spec;
    spec.entity_column = "e";
    spec.time_column = "t";
    spec.as_of = 1.0;
    spec.event_features = {WindowedFeature{.name = "n", .agg = AggregateOp::Count}};
    const Json json = spec.to_json();
    EXPECT_FALSE(json.contains("account_features"));
    EXPECT_FALSE(json.contains("label_column"));
    EXPECT_FALSE(json.at("event_features").at(0).contains("value_column"));
    EXPECT_FALSE(json.at("event_features").at(0).contains("window_start"));
    EXPECT_FALSE(json.at("event_features").at(0).contains("type_column"));
    const auto parsed = FeaturizeSpec::from_json(json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->account_features.empty());
    EXPECT_TRUE(parsed->label_column.empty());
}

// A static carry-through spec (#532): engineered columns carried through, no event
// features, no time dimension. It round-trips and omits time_column / as_of /
// event_features in the canonical form.
TEST(DdfFeaturizeJson, StaticSpecRoundTrips) {
    FeaturizeSpec spec;
    spec.entity_column = "conversation_id";
    spec.account_features = {"customer_engagement", "sales_effectiveness",
                             "conversation_length"};
    spec.label_column = "outcome";

    const Json json = spec.to_json();
    EXPECT_FALSE(json.contains("time_column"));
    EXPECT_FALSE(json.contains("as_of"));
    EXPECT_FALSE(json.contains("event_features"));

    const auto parsed = FeaturizeSpec::from_json(json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->entity_column, "conversation_id");
    EXPECT_TRUE(parsed->time_column.empty());
    EXPECT_TRUE(parsed->event_features.empty());
    EXPECT_EQ(parsed->account_features, spec.account_features);
    EXPECT_EQ(parsed->label_column, "outcome");
}

// A static spec MAY carry a point-in-time cutoff (time_column + as_of) for the row
// filter "where applicable" — that round-trips too.
TEST(DdfFeaturizeJson, StaticSpecWithCutoffRoundTrips) {
    FeaturizeSpec spec;
    spec.entity_column = "conversation_id";
    spec.time_column = "event_ts";
    spec.as_of = 1000.0;
    spec.account_features = {"customer_engagement"};
    spec.label_column = "outcome";

    const auto parsed = FeaturizeSpec::from_json(spec.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->time_column, "event_ts");
    EXPECT_DOUBLE_EQ(parsed->as_of, 1000.0);
    EXPECT_TRUE(parsed->event_features.empty());
    EXPECT_EQ(parsed->account_features, spec.account_features);
}

TEST(DdfFeaturizeJson, RejectsBadInput) {
    const Json good = sample_spec().to_json();
    ASSERT_TRUE(FeaturizeSpec::from_json(good).has_value());

    EXPECT_FALSE(FeaturizeSpec::from_json(Json::array()).has_value());  // not an object
    {  // missing entity_column
        Json bad = good;
        bad.erase("entity_column");
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // no feature columns at all (empty event_features AND no account_features)
        Json bad = good;
        bad["event_features"] = Json::array();
        bad.erase("account_features");
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // windowed event features without a time_column
        Json bad = good;
        bad.erase("time_column");
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // a time_column without an as_of cutoff
        Json bad = good;
        bad.erase("as_of");
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // sum feature without a value_column
        Json bad = good;
        bad["event_features"] = Json::array(
            {{{"name", "x"}, {"agg", "sum"}}});
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // unknown agg
        Json bad = good;
        bad["event_features"][0]["agg"] = "median";
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // window_start above as_of (empty window)
        Json bad = good;
        bad["event_features"][0]["window_start"] = 1000.0;
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // type_column without type_value
        Json bad = good;
        bad["event_features"][0].erase("type_value");
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // duplicate output name (feature vs account column)
        Json bad = good;
        bad["account_features"] = Json::array({"failed_payments"});
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
    {  // unknown key
        Json bad = good;
        bad["surprise"] = 1;
        EXPECT_FALSE(FeaturizeSpec::from_json(bad).has_value());
    }
}

// =====================================================================
// Single-node featurize — a small synthetic frame
// =====================================================================

DataFrameSchema events_schema() {
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "entity", .dtype = DataType::Int64},
                      ColumnSchema{.name = "ts", .dtype = DataType::Int64},
                      ColumnSchema{.name = "value", .dtype = DataType::Float64},
                      ColumnSchema{.name = "type", .dtype = DataType::Int64}};
    return schema;
}
DataFrameSchema accounts_schema() {
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "entity", .dtype = DataType::Int64},
                      ColumnSchema{.name = "mrr", .dtype = DataType::Float64},
                      ColumnSchema{.name = "seats", .dtype = DataType::Int64},
                      ColumnSchema{.name = "tenure_days", .dtype = DataType::Int64},
                      ColumnSchema{.name = "churned", .dtype = DataType::Int64}};
    return schema;
}

TEST(DdfFeaturize, WindowedFeaturesExcludeFutureRows) {
    // type 3 = payment_failed. Entity 1 has two payments before t0 (ts 10, 50) and a
    // third AFTER t0 (ts 99 — the leak). Entity 2 has one payment before t0.
    const auto events = make_store({
        {{"entity", 1}, {"ts", 10}, {"value", 100.0}, {"type", 3}},
        {{"entity", 1}, {"ts", 50}, {"value", 200.0}, {"type", 3}},
        {{"entity", 1}, {"ts", 99}, {"value", 999.0}, {"type", 3}},  // future (dropped)
        {{"entity", 2}, {"ts", 20}, {"value", 50.0}, {"type", 3}},
    });
    const auto accounts = make_store({
        {{"entity", 1}, {"mrr", 1000.0}, {"seats", 10}, {"tenure_days", 100}, {"churned", 1}},
        {{"entity", 2}, {"mrr", 2000.0}, {"seats", 20}, {"tenure_days", 200}, {"churned", 0}},
        {{"entity", 3}, {"mrr", 3000.0}, {"seats", 30}, {"tenure_days", 300}, {"churned", 0}},
    });

    CpuDevice device;
    auto events_frame = load_data_frame(events, events_schema(), device);
    ASSERT_TRUE(events_frame.ok()) << events_frame.status.message;
    auto accounts_frame = load_data_frame(accounts, accounts_schema(), device);
    ASSERT_TRUE(accounts_frame.ok()) << accounts_frame.status.message;

    FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = 60.0;
    spec.event_features = {
        WindowedFeature{.name = "payments_count",
                        .agg = AggregateOp::Count,
                        .type_column = "type",
                        .type_value = 3.0},
        WindowedFeature{.name = "payments_sum",
                        .agg = AggregateOp::Sum,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 3.0},
        WindowedFeature{.name = "max_payment",
                        .agg = AggregateOp::Max,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 3.0},
    };
    spec.account_features = {"mrr", "seats"};
    spec.label_column = "churned";

    const auto result = point_in_time_featurize(events_frame.frame, accounts_frame.frame, spec);
    ASSERT_TRUE(result.ok()) << result.status.message;
    const FeatureTable& table = result.table;

    // One row per account (the entity universe), ascending — including account 3, which
    // has no events at all (left join, zero-filled).
    EXPECT_EQ(table.entities, (std::vector<std::int64_t>{1, 2, 3}));
    EXPECT_EQ(table.feature_names,
              (std::vector<std::string>{"mrr", "seats", "payments_count", "payments_sum",
                                        "max_payment"}));

    // Entity 1: 2 in-window payments (the ts=99 one excluded), sum 300 (not 1299), max 200.
    EXPECT_EQ(*table.feature("payments_count"), (std::vector<double>{2.0, 1.0, 0.0}));
    EXPECT_EQ(*table.feature("payments_sum"), (std::vector<double>{300.0, 50.0, 0.0}));
    EXPECT_EQ(*table.feature("max_payment"), (std::vector<double>{200.0, 50.0, 0.0}));
    // Static account columns ride through; the label is carried separately as y.
    EXPECT_EQ(*table.feature("mrr"), (std::vector<double>{1000.0, 2000.0, 3000.0}));
    EXPECT_EQ(table.label_name, "churned");
    EXPECT_EQ(table.labels, (std::vector<double>{1.0, 0.0, 0.0}));
}

TEST(DdfFeaturize, ToDataFrameMaterializesColumns) {
    const auto events = make_store({
        {{"entity", 1}, {"ts", 10}, {"value", 5.0}, {"type", 3}},
    });
    const auto accounts = make_store({
        {{"entity", 1}, {"mrr", 9.0}, {"seats", 2}, {"tenure_days", 3}, {"churned", 1}},
    });
    CpuDevice device;
    auto events_frame = load_data_frame(events, events_schema(), device);
    auto accounts_frame = load_data_frame(accounts, accounts_schema(), device);
    ASSERT_TRUE(events_frame.ok());
    ASSERT_TRUE(accounts_frame.ok());

    FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = 60.0;
    spec.event_features = {WindowedFeature{
        .name = "payments", .agg = AggregateOp::Count, .type_column = "type", .type_value = 3.0}};
    spec.account_features = {"mrr"};
    spec.label_column = "churned";

    const auto result = point_in_time_featurize(events_frame.frame, accounts_frame.frame, spec);
    ASSERT_TRUE(result.ok()) << result.status.message;

    const auto frame = result.table.to_data_frame(device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;
    EXPECT_EQ(frame.frame.row_count(), 1u);
    EXPECT_NE(frame.frame.column("entity"), nullptr);
    EXPECT_NE(frame.frame.column("mrr"), nullptr);
    EXPECT_NE(frame.frame.column("payments"), nullptr);
    EXPECT_NE(frame.frame.column("churned"), nullptr);  // the label rides into the frame
}

// =====================================================================
// Static featurize — engineered numeric columns, no windowed events (#532)
// =====================================================================

// The saas-sales-conversations shape: each row is one conversation (entity) carrying
// already-engineered numeric columns + an outcome label. A schema with an integer
// entity key, three engineered columns, an event timestamp, and the label.
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

std::vector<Json> engineered_rows() {
    return {
        {{"conversation_id", 1}, {"customer_engagement", 0.80}, {"sales_effectiveness", 0.50},
         {"conversation_length", 12}, {"event_ts", 10}, {"outcome", 1}},
        {{"conversation_id", 2}, {"customer_engagement", 0.30}, {"sales_effectiveness", 0.90},
         {"conversation_length", 4}, {"event_ts", 20}, {"outcome", 0}},
        // event_ts 99 is AFTER the as_of cutoff (60) — the leakage trap (dropped).
        {{"conversation_id", 3}, {"customer_engagement", 0.60}, {"sales_effectiveness", 0.20},
         {"conversation_length", 31}, {"event_ts", 99}, {"outcome", 1}},
        {{"conversation_id", 4}, {"customer_engagement", 0.95}, {"sales_effectiveness", 0.77},
         {"conversation_length", 7}, {"event_ts", 50}, {"outcome", 0}},
    };
}

FeaturizeSpec engineered_spec(bool with_cutoff) {
    FeaturizeSpec spec;
    spec.entity_column = "conversation_id";
    if (with_cutoff) {
        spec.time_column = "event_ts";
        spec.as_of = 60.0;
    }
    spec.account_features = {"customer_engagement", "sales_effectiveness",
                             "conversation_length"};
    spec.label_column = "outcome";
    return spec;
}

// A static spec with no time dimension carries every engineered column through, one
// row per entity (ascending), with the outcome as the label.
TEST(DdfFeaturize, StaticCarriesEngineeredColumns) {
    CpuDevice device;
    auto frame = load_data_frame(make_store(engineered_rows()), engineered_schema(), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;

    // events frame is ignored in static mode — pass the same frame for both.
    const auto result =
        point_in_time_featurize(frame.frame, frame.frame, engineered_spec(false));
    ASSERT_TRUE(result.ok()) << result.status.message;
    const FeatureTable& table = result.table;

    EXPECT_EQ(table.entities, (std::vector<std::int64_t>{1, 2, 3, 4}));
    EXPECT_EQ(table.feature_names,
              (std::vector<std::string>{"customer_engagement", "sales_effectiveness",
                                        "conversation_length"}));
    EXPECT_EQ(*table.feature("customer_engagement"),
              (std::vector<double>{0.80, 0.30, 0.60, 0.95}));
    EXPECT_EQ(*table.feature("conversation_length"),
              (std::vector<double>{12.0, 4.0, 31.0, 7.0}));
    EXPECT_EQ(table.label_name, "outcome");
    EXPECT_EQ(table.labels, (std::vector<double>{1.0, 0.0, 1.0, 0.0}));
}

// "Point-in-time, no leakage where applicable": with a time_column the row universe is
// filtered to event_ts <= as_of, so the post-cutoff conversation is excluded entirely.
TEST(DdfFeaturize, StaticPointInTimeFilterDropsFutureRows) {
    CpuDevice device;
    auto frame = load_data_frame(make_store(engineered_rows()), engineered_schema(), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;

    const auto result =
        point_in_time_featurize(frame.frame, frame.frame, engineered_spec(true));
    ASSERT_TRUE(result.ok()) << result.status.message;
    const FeatureTable& table = result.table;

    // Conversation 3 (event_ts 99 > as_of 60) is dropped — no leakage.
    EXPECT_EQ(table.entities, (std::vector<std::int64_t>{1, 2, 4}));
    EXPECT_EQ(*table.feature("customer_engagement"),
              (std::vector<double>{0.80, 0.30, 0.95}));
    EXPECT_EQ(*table.feature("sales_effectiveness"),
              (std::vector<double>{0.50, 0.90, 0.77}));
    EXPECT_EQ(table.labels, (std::vector<double>{1.0, 0.0, 0.0}));
}

// =====================================================================
// The real churn fixture — point-in-time featurize + the leakage assertion
// =====================================================================

// A linear day index within 2026 (the only year the fixture uses): days-of-year so a
// trailing window can be expressed (unlike a YYYYMMDD ordinal). Strictly monotonic.
std::int64_t day_of_year_2026(const std::string& iso) {
    // ISO "2026-MM-DD[Thh:mm:ssZ]".
    const int month = std::stoi(iso.substr(5, 2));
    const int day = std::stoi(iso.substr(8, 2));
    static const int before[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    return before[month] + day;
}

// Numeric event-type codes (the upstream featurize encoding of the string types).
std::int64_t event_type_code(const std::string& type) {
    if (type == "login") return 1;
    if (type == "feature_use") return 2;
    if (type == "payment_failed") return 3;
    if (type == "support_ticket") return 4;
    if (type == "csm_contact") return 5;
    if (type == "comms") return 6;
    return 0;
}

// Encode a loaded ChurnDataset into the numeric (events, accounts) stores the DDF
// featurize operates over, plus the stable account_id -> entity-key index.
struct ChurnNumeric {
    std::vector<Json> events;
    std::vector<Json> accounts;
    std::map<std::string, std::int64_t> entity_index;
    std::int64_t as_of = 0;
};

ChurnNumeric encode_churn(const zetta::usecases::churn::ChurnDataset& dataset) {
    ChurnNumeric out;
    for (const zetta::usecases::churn::ChurnAccount& account : dataset.accounts) {
        const std::int64_t key =
            static_cast<std::int64_t>(out.entity_index.size());
        out.entity_index.emplace(account.account_id, key);
        out.accounts.push_back({{"entity", key},
                                {"mrr", account.mrr},
                                {"seats", account.seats},
                                {"tenure_days", account.tenure_days},
                                {"churned", account.churned ? 1 : 0}});
    }
    for (const zetta::usecases::churn::ChurnEvent& event : dataset.events) {
        out.events.push_back({{"entity", out.entity_index.at(event.account_id)},
                              {"ts", day_of_year_2026(event.event_ts)},
                              {"value", event.value},
                              {"type", event_type_code(event.event_type)}});
    }
    out.as_of = day_of_year_2026(dataset.as_of_date);
    return out;
}

// The churn point-in-time feature spec: the shape of churn_features.sql.
FeaturizeSpec churn_spec(std::int64_t as_of) {
    FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = static_cast<double>(as_of);
    spec.event_features = {
        // Billing health — involuntary churn signal.
        WindowedFeature{.name = "failed_payments",
                        .agg = AggregateOp::Count,
                        .type_column = "type",
                        .type_value = 3.0},
        // Support health — total severity before t0.
        WindowedFeature{.name = "support_severity",
                        .agg = AggregateOp::Sum,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 4.0},
        // Engagement — total events before t0 (no type filter).
        WindowedFeature{.name = "total_events", .agg = AggregateOp::Count},
        // Adoption — mean usage value over feature-use events.
        WindowedFeature{.name = "mean_usage",
                        .agg = AggregateOp::Mean,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 2.0},
    };
    spec.account_features = {"mrr", "seats", "tenure_days"};
    spec.label_column = "churned";
    return spec;
}

// Independent ground truth computed straight from the dataset: windowed (event_ts <=
// as_of) and leaky (no cutoff) per-entity aggregates, for the leakage assertion.
struct GroundTruth {
    std::map<std::int64_t, double> failed_payments, support_severity, total_events;
    std::map<std::int64_t, double> leaky_failed_payments, leaky_support, leaky_total;
};

GroundTruth ground_truth(const ChurnNumeric& numeric) {
    GroundTruth truth;
    for (const Json& event : numeric.events) {
        const auto key = event.at("entity").get<std::int64_t>();
        const auto day = event.at("ts").get<std::int64_t>();
        const auto type = event.at("type").get<std::int64_t>();
        const auto value = event.at("value").get<double>();
        const bool in_window = day <= numeric.as_of;
        truth.leaky_total[key] += 1.0;
        if (in_window) truth.total_events[key] += 1.0;
        if (type == 3) {  // payment_failed
            truth.leaky_failed_payments[key] += 1.0;
            if (in_window) truth.failed_payments[key] += 1.0;
        }
        if (type == 4) {  // support_ticket
            truth.leaky_support[key] += value;
            if (in_window) truth.support_severity[key] += value;
        }
    }
    return truth;
}

TEST(DdfFeaturize, ChurnFixturePointInTimeNoLeakage) {
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::usecases::churn::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    const ChurnNumeric numeric = encode_churn(loaded.dataset);
    GroundTruth truth = ground_truth(numeric);  // non-const: [] inserts 0 for a missing key

    CpuDevice device;
    auto events_frame =
        load_data_frame(make_store(numeric.events), events_schema(), device);
    ASSERT_TRUE(events_frame.ok()) << events_frame.status.message;
    auto accounts_frame =
        load_data_frame(make_store(numeric.accounts), accounts_schema(), device);
    ASSERT_TRUE(accounts_frame.ok()) << accounts_frame.status.message;

    const FeaturizeSpec spec = churn_spec(numeric.as_of);
    const auto result =
        point_in_time_featurize(events_frame.frame, accounts_frame.frame, spec);
    ASSERT_TRUE(result.ok()) << result.status.message;
    const FeatureTable& table = result.table;

    // One feature row per account (dense over the universe).
    EXPECT_EQ(table.row_count(), numeric.entity_index.size());
    const std::vector<double>* failed = table.feature("failed_payments");
    const std::vector<double>* support = table.feature("support_severity");
    const std::vector<double>* total = table.feature("total_events");
    ASSERT_NE(failed, nullptr);
    ASSERT_NE(support, nullptr);
    ASSERT_NE(total, nullptr);

    // Every feature equals the leakage-free ground truth, and the future events are
    // genuinely excluded: at least one entity's windowed value is strictly below its
    // leaky (no-cutoff) value (the deliberate post-t0 events the fixture carries).
    bool saw_dropped_event = false;
    double windowed_events = 0.0;
    double all_events = static_cast<double>(numeric.events.size());
    for (std::size_t row = 0; row < table.row_count(); ++row) {
        const std::int64_t key = table.entities[row];
        EXPECT_DOUBLE_EQ((*failed)[row], truth.failed_payments[key]) << "entity " << key;
        EXPECT_DOUBLE_EQ((*support)[row], truth.support_severity[key]) << "entity " << key;
        EXPECT_DOUBLE_EQ((*total)[row], truth.total_events[key]) << "entity " << key;
        windowed_events += (*total)[row];
        if (truth.leaky_total[key] != truth.total_events[key] ||
            truth.leaky_failed_payments[key] != truth.failed_payments[key] ||
            truth.leaky_support[key] != truth.support_severity[key]) {
            saw_dropped_event = true;
        }
    }
    // The leakage trap fired: fewer events counted than exist, and a windowed feature
    // is strictly below its leaky value for at least one account.
    EXPECT_LT(windowed_events, all_events);
    EXPECT_TRUE(saw_dropped_event);

    // Concretely: fedex_office's third payment_failed (2026-04-05) and globex's second
    // support ticket (2026-03-20) are both after t0 (2026-03-01) and must be dropped.
    const std::int64_t office = numeric.entity_index.at("acct_fedex_office");
    const std::int64_t globex = numeric.entity_index.at("acct_globex");
    EXPECT_DOUBLE_EQ(truth.leaky_failed_payments[office], 3.0);  // 3 in the raw data
    EXPECT_DOUBLE_EQ(truth.failed_payments[office], 2.0);        // 2 before t0
    EXPECT_GT(truth.leaky_support[globex], truth.support_severity[globex]);
}

// =====================================================================
// The distributed `featurize` ComputeJob — single-node == multi-node
// =====================================================================

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

struct ComputeNode {
    ComputeNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds, unsigned workers)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, workers) {
        node.start();
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
    TempDir temp_dir;  // declared first so it outlives `node`
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
};

std::vector<Json> slice(const std::vector<Json>& rows, std::size_t begin, std::size_t end) {
    return std::vector<Json>(rows.begin() + std::min(begin, rows.size()),
                             rows.begin() + std::min(end, rows.size()));
}

// Run the featurize ComputeJob across `node_count` loopback nodes, each owning a
// contiguous shard of events + accounts, and return node 0's global feature table.
// Asserts every node Succeeded, agrees, and recorded a Featurize job.
FeatureTable run_distributed_featurize(const ChurnNumeric& numeric,
                                       const FeaturizeSpec& spec,
                                       std::size_t node_count, std::uint16_t base_port) {
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < node_count; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        nodes.push_back(std::make_unique<ComputeNode>(
            ids[index], static_cast<std::uint16_t>(base_port + index), seeds, 2));
    }
    for (auto& node : nodes) node->cluster.start();
    EXPECT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != node_count - 1) return false;
        }
        return true;
    }));
    for (auto& node : nodes) node->coordinator.start();

    std::vector<std::unique_ptr<CpuDevice>> devices;
    std::vector<DataFrame> events_frames;
    std::vector<DataFrame> accounts_frames;
    const std::size_t evt_per = (numeric.events.size() + node_count - 1) / node_count;
    const std::size_t acc_per = (numeric.accounts.size() + node_count - 1) / node_count;
    for (std::size_t index = 0; index < node_count; ++index) {
        devices.push_back(std::make_unique<CpuDevice>());
        auto events = load_data_frame(
            make_store(slice(numeric.events, index * evt_per, (index + 1) * evt_per)),
            events_schema(), *devices[index]);
        auto accounts = load_data_frame(
            make_store(slice(numeric.accounts, index * acc_per, (index + 1) * acc_per)),
            accounts_schema(), *devices[index]);
        EXPECT_TRUE(events.ok()) << events.status.message;
        EXPECT_TRUE(accounts.ok()) << accounts.status.message;
        events_frames.push_back(std::move(events.frame));
        accounts_frames.push_back(std::move(accounts.frame));
    }

    std::vector<zetta::ddf::DistributedFeaturizeResult> results(node_count);
    std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < node_count; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Featurize);
        threads.emplace_back([&, index] {
            results[index] = run_featurize_job(nodes[index]->coordinator, *jobs[index],
                                               events_frames[index],
                                               accounts_frames[index], spec);
        });
    }
    for (auto& thread : threads) thread.join();

    for (std::size_t index = 0; index < node_count; ++index) {
        EXPECT_EQ(results[index].status, JobStatus::Succeeded)
            << "node " << index << ": " << jobs[index]->detail();
        EXPECT_EQ(jobs[index]->kind(), JobKind::Featurize) << "node " << index;
        EXPECT_TRUE(nodes[index]->node.find_job(jobs[index]->id())) << "node " << index;
        // Every node ends with the identical global feature table (broadcast).
        EXPECT_EQ(results[index].table.entities, results[0].table.entities)
            << "node " << index;
    }
    EXPECT_TRUE(results[0].is_root);

    FeatureTable table = std::move(results[0].table);
    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
    return table;
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

TEST(DdfFeaturize, DistributedMatchesSingleNode) {
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::usecases::churn::load_churn_dataset(churn_dir);
    ASSERT_TRUE(loaded.ok()) << loaded.error;
    const ChurnNumeric numeric = encode_churn(loaded.dataset);
    const FeaturizeSpec spec = churn_spec(numeric.as_of);

    // The single-node reference (as-of + group-by + hash-join).
    CpuDevice device;
    auto events_frame = load_data_frame(make_store(numeric.events), events_schema(), device);
    auto accounts_frame =
        load_data_frame(make_store(numeric.accounts), accounts_schema(), device);
    ASSERT_TRUE(events_frame.ok());
    ASSERT_TRUE(accounts_frame.ok());
    const auto reference =
        point_in_time_featurize(events_frame.frame, accounts_frame.frame, spec);
    ASSERT_TRUE(reference.ok()) << reference.status.message;

    // Single-node-via-job and 3-node-via-job both equal the reference.
    const FeatureTable single = run_distributed_featurize(numeric, spec, 1, 47500);
    expect_tables_equal(single, reference.table);
    const FeatureTable multi = run_distributed_featurize(numeric, spec, 3, 47510);
    expect_tables_equal(multi, reference.table);
}

}  // namespace
