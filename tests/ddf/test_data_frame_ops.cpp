#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/data_frame_ops.hpp"
#include "zetta/storage/json_store.hpp"

// DDF DataFrame ops (#385): the single-node building blocks — sort (order a frame
// by a key column, carrying every other column), concat (row-wise union), and
// slice (a contiguous row range). The distributed sort that composes these across
// the cluster is exercised in test_distributed_sort.cpp.
namespace {

using zetta::compute::ComputeStatusCode;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::SortOrder;
using zetta::ddf::ColumnSchema;
using zetta::ddf::concat;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::load_data_frame;
using zetta::ddf::slice;
using zetta::ddf::sort;

zetta::storage::JsonStore make_store(const std::vector<nlohmann::json>& rows) {
    zetta::storage::JsonStore store;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.add("row-" + std::to_string(i), rows[i]);
    }
    return store;
}

// Load a {key:float64, id:int64} frame from the given rows.
DataFrame load_key_id(CpuDevice& device, const zetta::storage::JsonStore& store,
                      std::size_t chunk_rows = zetta::ddf::kDefaultChunkRows) {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "key", .dtype = DataType::Float64},
        ColumnSchema{.name = "id", .dtype = DataType::Int64},
    };
    auto result = load_data_frame(store, schema, device, chunk_rows);
    EXPECT_TRUE(result.ok()) << result.status.message;
    return std::move(result.frame);
}

// --- sort --------------------------------------------------------------------

TEST(DataFrameSort, OrdersByKeyAndCarriesPayload) {
    CpuDevice device;
    const auto store = make_store({
        {{"key", 3.0}, {"id", 100}},
        {{"key", 1.0}, {"id", 101}},
        {{"key", 2.0}, {"id", 102}},
        {{"key", -5.0}, {"id", 103}},
    });
    const DataFrame frame = load_key_id(device, store);

    auto result = sort(frame, "key");
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.row_count(), 4u);

    std::vector<double> key;
    ASSERT_TRUE(result.frame.to_host<double>("key", key).ok());
    EXPECT_EQ(key, (std::vector<double>{-5.0, 1.0, 2.0, 3.0}));

    // The id column reorders with the key — the payload follows its row.
    std::vector<std::int64_t> id;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("id", id).ok());
    EXPECT_EQ(id, (std::vector<std::int64_t>{103, 101, 102, 100}));
}

TEST(DataFrameSort, Descending) {
    CpuDevice device;
    const auto store = make_store({
        {{"key", 3.0}, {"id", 1}},
        {{"key", 1.0}, {"id", 2}},
        {{"key", 2.0}, {"id", 3}},
    });
    const DataFrame frame = load_key_id(device, store);

    auto result = sort(frame, "key", SortOrder::Descending);
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<double> key;
    ASSERT_TRUE(result.frame.to_host<double>("key", key).ok());
    EXPECT_EQ(key, (std::vector<double>{3.0, 2.0, 1.0}));
}

TEST(DataFrameSort, IsStableForEqualKeys) {
    CpuDevice device;
    // Three rows share key 1.0; their ids must keep input order after the sort.
    const auto store = make_store({
        {{"key", 1.0}, {"id", 10}},
        {{"key", 0.0}, {"id", 11}},
        {{"key", 1.0}, {"id", 12}},
        {{"key", 1.0}, {"id", 13}},
    });
    const DataFrame frame = load_key_id(device, store);

    auto result = sort(frame, "key");
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<std::int64_t> id;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("id", id).ok());
    EXPECT_EQ(id, (std::vector<std::int64_t>{11, 10, 12, 13}));
}

TEST(DataFrameSort, SpansMultipleChunks) {
    CpuDevice device;
    std::vector<nlohmann::json> rows;
    for (int value = 20; value > 0; --value) {  // 20..1 descending input
        rows.push_back({{"key", static_cast<double>(value)}, {"id", value}});
    }
    const DataFrame frame = load_key_id(device, make_store(rows), /*chunk_rows=*/3);
    ASSERT_GT(frame.chunk_count(), 1u);

    auto result = sort(frame, "key");
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<double> key;
    ASSERT_TRUE(result.frame.to_host<double>("key", key).ok());
    ASSERT_EQ(key.size(), 20u);
    for (std::size_t i = 0; i < key.size(); ++i) {
        EXPECT_EQ(key[i], static_cast<double>(i + 1));
    }
    // The sorted frame keeps the source chunk layout.
    EXPECT_EQ(result.frame.chunk_rows(), 3u);
}

TEST(DataFrameSort, RejectsUnknownKeyColumn) {
    CpuDevice device;
    const DataFrame frame = load_key_id(device, make_store({{{"key", 1.0}, {"id", 1}}}));
    auto result = sort(frame, "nope");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrameSort, SingleRowRoundTrips) {
    CpuDevice device;
    const DataFrame frame = load_key_id(device, make_store({{{"key", 7.0}, {"id", 9}}}));
    auto result = sort(frame, "key");
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.row_count(), 1u);
    std::vector<std::int64_t> id;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("id", id).ok());
    EXPECT_EQ(id, (std::vector<std::int64_t>{9}));
}

// --- concat ------------------------------------------------------------------

TEST(DataFrameConcat, AppendsRowsInOrder) {
    CpuDevice device;
    const DataFrame a = load_key_id(device, make_store({
        {{"key", 1.0}, {"id", 1}},
        {{"key", 2.0}, {"id", 2}},
    }));
    const DataFrame b = load_key_id(device, make_store({
        {{"key", 3.0}, {"id", 3}},
    }));

    auto result = concat({&a, &b});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.row_count(), 3u);

    std::vector<double> key;
    ASSERT_TRUE(result.frame.to_host<double>("key", key).ok());
    EXPECT_EQ(key, (std::vector<double>{1.0, 2.0, 3.0}));
    std::vector<std::int64_t> id;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("id", id).ok());
    EXPECT_EQ(id, (std::vector<std::int64_t>{1, 2, 3}));
}

TEST(DataFrameConcat, SingleFrameIsACopy) {
    CpuDevice device;
    const DataFrame a = load_key_id(device, make_store({{{"key", 5.0}, {"id", 5}}}));
    auto result = concat({&a});
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.row_count(), 1u);
}

TEST(DataFrameConcat, RejectsEmptyList) {
    auto result = concat({});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DataFrameConcat, RejectsSchemaMismatch) {
    CpuDevice device;
    const DataFrame a = load_key_id(device, make_store({{{"key", 1.0}, {"id", 1}}}));

    DataFrameSchema other;
    other.columns = {ColumnSchema{.name = "key", .dtype = DataType::Float64}};
    auto other_result =
        load_data_frame(make_store({{{"key", 2.0}}}), other, device);
    ASSERT_TRUE(other_result.ok());

    auto result = concat({&a, &other_result.frame});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

// --- slice -------------------------------------------------------------------

TEST(DataFrameSlice, CutsAContiguousRange) {
    CpuDevice device;
    std::vector<nlohmann::json> rows;
    for (int value = 0; value < 6; ++value) {
        rows.push_back({{"key", static_cast<double>(value)}, {"id", value}});
    }
    const DataFrame frame = load_key_id(device, make_store(rows));

    auto result = slice(frame, 2, 5);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.row_count(), 3u);
    std::vector<std::int64_t> id;
    ASSERT_TRUE(result.frame.to_host<std::int64_t>("id", id).ok());
    EXPECT_EQ(id, (std::vector<std::int64_t>{2, 3, 4}));
}

TEST(DataFrameSlice, ClampsOutOfRangeBounds) {
    CpuDevice device;
    const DataFrame frame = load_key_id(device, make_store({
        {{"key", 0.0}, {"id", 0}},
        {{"key", 1.0}, {"id", 1}},
    }));
    auto result = slice(frame, 1, 100);  // end past the row count
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.row_count(), 1u);

    auto empty = slice(frame, 2, 2);  // empty range
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(empty.frame.row_count(), 0u);
}

// --- sort + concat compose into a global order -------------------------------

TEST(DataFrameOps, SliceSortConcatReassemblesGlobalOrder) {
    CpuDevice device;
    // Sort one frame, slice it into two partitions, and concat them back: the
    // round-trip is the shape the distributed sort relies on.
    std::vector<nlohmann::json> rows;
    for (int value : {5, 2, 8, 1, 9, 3, 7, 4, 6, 0}) {
        rows.push_back({{"key", static_cast<double>(value)}, {"id", value}});
    }
    const DataFrame frame = load_key_id(device, make_store(rows));

    auto sorted = sort(frame, "key");
    ASSERT_TRUE(sorted.ok());
    auto lo = slice(sorted.frame, 0, 5);
    auto hi = slice(sorted.frame, 5, 10);
    ASSERT_TRUE(lo.ok() && hi.ok());

    auto merged = concat({&lo.frame, &hi.frame});
    ASSERT_TRUE(merged.ok());
    std::vector<double> key;
    ASSERT_TRUE(merged.frame.to_host<double>("key", key).ok());
    EXPECT_EQ(key, (std::vector<double>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

}  // namespace
