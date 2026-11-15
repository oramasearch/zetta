// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/data/table.hpp"
#include "temp_dir.hpp"

// The DDF DataFrame core (#380): a typed, columnar frame over the storage engine,
// chunked and backed by ComputeDevice buffers. These tests cover the load ->
// read-back round-trip (the acceptance), chunking, schema inference, the value
// boundary (missing / non-numeric / out-of-range), and the schema JSON boundary.
namespace {

using zetta::compute::ComputeStatusCode;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::infer_schema;
using zetta::ddf::load_data_frame;

// Build an in-memory store from a list of row docs (ids are not exercised here).
zetta::storage::JsonStore make_store(const std::vector<nlohmann::json>& rows) {
    zetta::storage::JsonStore store;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.add("row-" + std::to_string(i), rows[i]);
    }
    return store;
}

// --- round-trip --------------------------------------------------------------

TEST(DataFrame, LoadsTypedColumnsAndRoundTrips) {
    const zetta::storage::JsonStore store = make_store({
        {{"score", 0.5}, {"age", 30}, {"flag", 1}},
        {{"score", 1.5}, {"age", 41}, {"flag", 0}},
        {{"score", -2.0}, {"age", 19}, {"flag", 1}},
    });
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "score", .dtype = DataType::Float64},
        ColumnSchema{.name = "age", .dtype = DataType::Int32},
        ColumnSchema{.name = "flag", .dtype = DataType::UInt8},
    };

    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    const DataFrame& frame = result.frame;

    EXPECT_EQ(frame.row_count(), 3u);
    EXPECT_EQ(frame.column_count(), 3u);
    EXPECT_EQ(frame.column_names(),
              (std::vector<std::string>{"score", "age", "flag"}));

    std::vector<double> score;
    ASSERT_TRUE(frame.to_host<double>("score", score).ok());
    EXPECT_EQ(score, (std::vector<double>{0.5, 1.5, -2.0}));

    std::vector<std::int32_t> age;
    ASSERT_TRUE(frame.to_host<std::int32_t>("age", age).ok());
    EXPECT_EQ(age, (std::vector<std::int32_t>{30, 41, 19}));

    std::vector<std::uint8_t> flag;
    ASSERT_TRUE(frame.to_host<std::uint8_t>("flag", flag).ok());
    EXPECT_EQ(flag, (std::vector<std::uint8_t>{1, 0, 1}));
}

TEST(DataFrame, IntegerColumnAcceptsFloatFeatureValues) {
    // A Float column accepts integer JSON values (widened), not the reverse.
    const zetta::storage::JsonStore store = make_store({
        {{"v", 7}},
        {{"v", 8}},
    });
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "v", .dtype = DataType::Float32}};

    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;

    std::vector<float> values;
    ASSERT_TRUE(result.frame.to_host<float>("v", values).ok());
    EXPECT_EQ(values, (std::vector<float>{7.0f, 8.0f}));
}

TEST(DataFrame, BooleanColumnsLoadAsZeroAndOne) {
    const zetta::storage::JsonStore store = make_store({
        {{"label", false}, {"active", true}},
        {{"label", true}, {"active", false}},
    });
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "label", .dtype = DataType::Int64},
        ColumnSchema{.name = "active", .dtype = DataType::Float64},
    };
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<std::int64_t> labels;
    std::vector<double> active;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("label", labels).ok());
    ASSERT_TRUE(result.frame.to_host<double>("active", active).ok());
    EXPECT_EQ(labels, (std::vector<std::int64_t>{0, 1}));
    EXPECT_EQ(active, (std::vector<double>{1.0, 0.0}));
}

TEST(DataFrame, TextEntityColumnLoadsThroughItsResolvedMapping) {
    const zetta::storage::JsonStore store = make_store({
        {{"customer_id", "c00041"}},
        {{"customer_id", "c00042"}},
    });
    const auto mapped = zetta::data::derive_entity_key_mapping(
        "customer_id", {"c00041", "c00042"});
    ASSERT_TRUE(mapped.ok()) << mapped.error;
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "entity",
                                   .field = "customer_id",
                                   .dtype = DataType::Int64,
                                   .entity_keys = mapped.mapping}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<std::int64_t> entities;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("entity", entities).ok());
    ASSERT_EQ(entities.size(), 2u);
    EXPECT_EQ(mapped.mapping->original(entities[0]), "c00041");
    EXPECT_EQ(mapped.mapping->original(entities[1]), "c00042");
}

TEST(DataFrame, TimestampColumnNormalizesIsoAndEpochValues) {
    const zetta::storage::JsonStore store = make_store({
        {{"observed_at", "1970-01-01T00:00:01Z"}},
        {{"observed_at", 2.5}},
    });
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "observed_at",
                     .dtype = DataType::Float64,
                     .timestamp = true},
    };

    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<double> values;
    ASSERT_TRUE(result.frame.to_host<double>("observed_at", values).ok());
    EXPECT_EQ(values, (std::vector<double>{1.0, 2.5}));
}

// --- chunking ----------------------------------------------------------------

TEST(DataFrame, ChunksRowsAcrossBuffers) {
    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 7; ++i) rows.push_back({{"x", i}});
    const zetta::storage::JsonStore store = make_store(rows);

    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Int32}};

    CpuDevice device;
    auto result = load_data_frame(store, schema, device, /*chunk_rows=*/3);
    ASSERT_TRUE(result.ok()) << result.status.message;
    const DataFrame& frame = result.frame;

    EXPECT_EQ(frame.chunk_rows(), 3u);
    EXPECT_EQ(frame.chunk_count(), 3u);  // ceil(7 / 3)

    const auto* column = frame.column("x");
    ASSERT_NE(column, nullptr);
    ASSERT_EQ(column->chunk_count(), 3u);
    EXPECT_EQ(column->chunk(0).element_count(), 3u);
    EXPECT_EQ(column->chunk(1).element_count(), 3u);
    EXPECT_EQ(column->chunk(2).element_count(), 1u);  // remainder

    // The split is transparent on read-back: chunks reassemble in row order.
    std::vector<std::int32_t> values;
    ASSERT_TRUE(column->to_host<std::int32_t>(device, values).ok());
    EXPECT_EQ(values, (std::vector<std::int32_t>{0, 1, 2, 3, 4, 5, 6}));
}

// --- the literal acceptance: load a real Table -------------------------------

TEST(DataFrame, LoadsFromTable) {
    zetta::test::TempDir tmp;
    zetta::data::Table table("tbl-uuid", "events", tmp.path() / "tbl");
    table.add_row({{"amount", 10.0}, {"count", 2}});
    table.add_row({{"amount", 20.5}, {"count", 5}});

    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "amount", .dtype = DataType::Float64},
        ColumnSchema{.name = "count", .dtype = DataType::Int64},
    };

    CpuDevice device;
    auto result = load_data_frame(table.store(), schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;

    std::vector<double> amount;
    ASSERT_TRUE(result.frame.to_host<double>("amount", amount).ok());
    EXPECT_EQ(amount, (std::vector<double>{10.0, 20.5}));

    std::vector<std::int64_t> count;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("count", count).ok());
    EXPECT_EQ(count, (std::vector<std::int64_t>{2, 5}));
}

// --- field override ----------------------------------------------------------

TEST(DataFrame, ColumnFieldOverrideReadsAlternateKey) {
    const zetta::storage::JsonStore store = make_store({{{"raw_value", 11}}});
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "value", .field = "raw_value", .dtype = DataType::Int32}};

    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;

    ASSERT_NE(result.frame.column("value"), nullptr);
    std::vector<std::int32_t> values;
    ASSERT_TRUE(result.frame.to_host<std::int32_t>("value", values).ok());
    EXPECT_EQ(values, (std::vector<std::int32_t>{11}));
}

// --- schema inference --------------------------------------------------------

TEST(DataFrame, InferSchemaPicksNumericFieldsAndTypes) {
    const zetta::storage::JsonStore store = make_store({
        {{"f", 1.5}, {"n", 3}, {"label", "churn"}},
        {{"f", 2.5}, {"n", 4}, {"label", "stay"}},
    });
    const DataFrameSchema schema = infer_schema(store);

    // A string field ("label") is skipped; numeric fields become columns in key
    // order (nlohmann orders object keys lexicographically): f (real) then n (int).
    ASSERT_EQ(schema.column_count(), 2u);
    EXPECT_EQ(schema.columns[0].name, "f");
    EXPECT_EQ(schema.columns[0].dtype, DataType::Float64);
    EXPECT_EQ(schema.columns[1].name, "n");
    EXPECT_EQ(schema.columns[1].dtype, DataType::Int64);

    // The inferred schema loads + round-trips.
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<double> f;
    ASSERT_TRUE(result.frame.to_host<double>("f", f).ok());
    EXPECT_EQ(f, (std::vector<double>{1.5, 2.5}));
}

TEST(DataFrame, InferSkipsFieldsMissingFromSomeRows) {
    const zetta::storage::JsonStore store = make_store({
        {{"a", 1}, {"b", 2}},
        {{"a", 3}},  // b absent here -> b is not dense -> dropped
    });
    const DataFrameSchema schema = infer_schema(store);
    ASSERT_EQ(schema.column_count(), 1u);
    EXPECT_EQ(schema.columns[0].name, "a");
}

TEST(DataFrame, InferEmptyStoreYieldsEmptySchema) {
    const zetta::storage::JsonStore store;
    EXPECT_TRUE(infer_schema(store).empty());
}

// --- empty store -------------------------------------------------------------

TEST(DataFrame, EmptyStoreYieldsZeroRowColumns) {
    const zetta::storage::JsonStore store;
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Int32}};

    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.row_count(), 0u);
    EXPECT_EQ(result.frame.column_count(), 1u);
    EXPECT_EQ(result.frame.chunk_count(), 0u);
    ASSERT_NE(result.frame.column("x"), nullptr);
    EXPECT_EQ(result.frame.column("x")->chunk_count(), 0u);
}

// --- value boundary ----------------------------------------------------------

TEST(DataFrame, RejectsMissingField) {
    const zetta::storage::JsonStore store = make_store({{{"y", 1}}});  // no "x"
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Int32}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrame, RejectsNonNumberField) {
    const zetta::storage::JsonStore store = make_store({{{"x", "oops"}}});
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Float64}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrame, RejectsRealIntoIntegerColumn) {
    const zetta::storage::JsonStore store = make_store({{{"x", 3.5}}});
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Int32}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrame, RejectsNegativeIntoUnsignedColumn) {
    const zetta::storage::JsonStore store = make_store({{{"x", -1}}});
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::UInt32}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrame, RejectsOutOfRangeValue) {
    const zetta::storage::JsonStore store = make_store({{{"x", 300}}});  // > 255
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::UInt8}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrame, RejectsZeroChunkRows) {
    const zetta::storage::JsonStore store = make_store({{{"x", 1}}});
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Int32}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device, /*chunk_rows=*/0);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

// --- read-back boundary ------------------------------------------------------

TEST(DataFrame, ToHostRejectsDtypeMismatch) {
    const zetta::storage::JsonStore store = make_store({{{"x", 1}}});
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Int32}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok());

    std::vector<float> wrong;  // column is Int32, not Float32
    const auto status = result.frame.to_host<float>("x", wrong);
    EXPECT_EQ(status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrame, ToHostRejectsUnknownColumn) {
    const zetta::storage::JsonStore store = make_store({{{"x", 1}}});
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Int32}};
    CpuDevice device;
    auto result = load_data_frame(store, schema, device);
    ASSERT_TRUE(result.ok());

    std::vector<std::int32_t> out;
    const auto status = result.frame.to_host<std::int32_t>("nope", out);
    EXPECT_EQ(status.code, ComputeStatusCode::InvalidArgument);
}

// --- schema JSON boundary ----------------------------------------------------

TEST(DataFrameSchemaJson, RoundTrips) {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "score",
                     .dtype = DataType::Float64,
                     .timestamp = true},
        ColumnSchema{.name = "value", .field = "raw_value", .dtype = DataType::Int32},
    };
    const auto parsed = DataFrameSchema::from_json(schema.to_json());
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->column_count(), 2u);
    EXPECT_EQ(parsed->columns[0].name, "score");
    EXPECT_EQ(parsed->columns[0].dtype, DataType::Float64);
    EXPECT_TRUE(parsed->columns[0].timestamp);
    EXPECT_TRUE(parsed->columns[0].field.empty());
    EXPECT_EQ(parsed->columns[1].name, "value");
    EXPECT_EQ(parsed->columns[1].field, "raw_value");
    EXPECT_EQ(parsed->columns[1].dtype, DataType::Int32);
}

TEST(DataFrameSchemaJson, RejectsBadInput) {
    using J = nlohmann::json;
    // not an object
    EXPECT_FALSE(DataFrameSchema::from_json(J::array()).has_value());
    // missing columns
    EXPECT_FALSE(DataFrameSchema::from_json(J::object()).has_value());
    // empty columns (minItems: 1)
    EXPECT_FALSE(
        DataFrameSchema::from_json(J{{"columns", J::array()}}).has_value());
    // unknown dtype
    EXPECT_FALSE(DataFrameSchema::from_json(
                     J{{"columns", {{{"name", "x"}, {"dtype", "float16"}}}}})
                     .has_value());
    // missing dtype
    EXPECT_FALSE(
        DataFrameSchema::from_json(J{{"columns", {{{"name", "x"}}}}}).has_value());
    // empty name
    EXPECT_FALSE(DataFrameSchema::from_json(
                     J{{"columns", {{{"name", ""}, {"dtype", "int32"}}}}})
                     .has_value());
    // duplicate names
    EXPECT_FALSE(DataFrameSchema::from_json(
                     J{{"columns",
                        {{{"name", "x"}, {"dtype", "int32"}},
                         {{"name", "x"}, {"dtype", "float64"}}}}})
                     .has_value());
    EXPECT_FALSE(DataFrameSchema::from_json(
                     J{{"columns",
                        {{{"name", "x"},
                          {"dtype", "int64"},
                          {"timestamp", true}}}}})
                     .has_value());
    EXPECT_FALSE(DataFrameSchema::from_json(
                     J{{"columns",
                        {{{"name", "x"},
                          {"dtype", "float32"},
                          {"timestamp", true}}}}})
                     .has_value());
}

TEST(DataFrameSchemaJson, AcceptsValidDocument) {
    using J = nlohmann::json;
    const J document{
        {"columns",
         {{{"name", "score"}, {"dtype", "float64"}},
          {{"name", "value"}, {"field", "raw_value"}, {"dtype", "uint32"}}}}};
    const auto parsed = DataFrameSchema::from_json(document);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->column_count(), 2u);
    EXPECT_EQ(parsed->columns[1].field, "raw_value");
    EXPECT_EQ(parsed->columns[1].dtype, DataType::UInt32);
}

}  // namespace
