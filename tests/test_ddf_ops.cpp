#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_status.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/table.hpp"
#include "temp_dir.hpp"

// DDF scan / filter / project ops (#381, ARC-12): the first operators over the
// DDF DataFrame substrate. These tests cover the acceptance (scan -> filter ->
// project on a fixture frame matches expected rows/cols), each op in isolation
// (including multi-chunk gather and the float-only-op rejection), and the
// FilterPredicate / Projection JSON boundaries. The compute runs on CpuDevice (the
// numerical reference); a guarded block at the bottom asserts CUDA parity where
// built.
namespace {

using zetta::compute::BinaryOp;
using zetta::compute::ComputeStatusCode;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::UnaryOp;
using zetta::ddf::ColumnSchema;
using zetta::ddf::Comparison;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::filter;
using zetta::ddf::FilterPredicate;
using zetta::ddf::project;
using zetta::ddf::ProjectColumn;
using zetta::ddf::Projection;
using zetta::ddf::scan;

// Build an in-memory store from a list of row docs.
zetta::JsonStore make_store(const std::vector<nlohmann::json>& rows) {
    zetta::JsonStore store;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        store.add("row-" + std::to_string(i), rows[i]);
    }
    return store;
}

DataFrameSchema int_schema(const std::vector<std::string>& names) {
    DataFrameSchema schema;
    for (const std::string& name : names) {
        schema.columns.push_back(ColumnSchema{.name = name, .dtype = DataType::Int32});
    }
    return schema;
}

// --- scan --------------------------------------------------------------------

TEST(DdfScan, InfersSchemaAndLoads) {
    const zetta::JsonStore store = make_store({
        {{"a", 1}, {"b", 2.5}, {"label", "x"}},
        {{"a", 3}, {"b", 4.5}, {"label", "y"}},
    });
    CpuDevice device;
    auto result = scan(store, device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    // Numeric fields become columns (a int, b real); the string is dropped.
    EXPECT_EQ(result.frame.column_count(), 2u);
    EXPECT_EQ(result.frame.row_count(), 2u);
    ASSERT_NE(result.frame.column("a"), nullptr);
    ASSERT_NE(result.frame.column("b"), nullptr);
    EXPECT_EQ(result.frame.column("a")->dtype(), DataType::Int64);
    EXPECT_EQ(result.frame.column("b")->dtype(), DataType::Float64);
}

TEST(DdfScan, WithExplicitSchemaLoadsFromTable) {
    zetta::test::TempDir tmp;
    zetta::Table table("tbl-uuid", "events", tmp.path() / "tbl");
    table.add_row({{"amount", 10}, {"count", 2}});
    table.add_row({{"amount", 20}, {"count", 5}});

    CpuDevice device;
    auto result = scan(table.store(), int_schema({"amount", "count"}), device);
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<std::int32_t> amount;
    ASSERT_TRUE(result.frame.to_host<std::int32_t>("amount", amount).ok());
    EXPECT_EQ(amount, (std::vector<std::int32_t>{10, 20}));
}

// --- filter ------------------------------------------------------------------

TEST(DdfFilter, KeepsMatchingRows) {
    const zetta::JsonStore store =
        make_store({{{"x", 1}}, {{"x", 2}}, {{"x", 3}}, {{"x", 4}}, {{"x", 5}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device);
    ASSERT_TRUE(scanned.ok());

    auto greater =
        filter(scanned.frame, {.column = "x", .comparison = Comparison::Greater,
                               .value = 3.0});
    ASSERT_TRUE(greater.ok()) << greater.status.message;
    EXPECT_EQ(greater.frame.row_count(), 2u);
    std::vector<std::int32_t> values;
    ASSERT_TRUE(greater.frame.to_host<std::int32_t>("x", values).ok());
    EXPECT_EQ(values, (std::vector<std::int32_t>{4, 5}));
}

TEST(DdfFilter, CoversEveryComparison) {
    const zetta::JsonStore store =
        make_store({{{"x", 1}}, {{"x", 2}}, {{"x", 3}}, {{"x", 4}}, {{"x", 5}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device);
    ASSERT_TRUE(scanned.ok());

    const auto pick = [&](Comparison comparison) {
        auto result = filter(scanned.frame,
                             {.column = "x", .comparison = comparison, .value = 3.0});
        EXPECT_TRUE(result.ok()) << result.status.message;
        std::vector<std::int32_t> out;
        EXPECT_TRUE(result.frame.to_host<std::int32_t>("x", out).ok());
        return out;
    };

    EXPECT_EQ(pick(Comparison::Equal), (std::vector<std::int32_t>{3}));
    EXPECT_EQ(pick(Comparison::NotEqual), (std::vector<std::int32_t>{1, 2, 4, 5}));
    EXPECT_EQ(pick(Comparison::Less), (std::vector<std::int32_t>{1, 2}));
    EXPECT_EQ(pick(Comparison::LessEqual), (std::vector<std::int32_t>{1, 2, 3}));
    EXPECT_EQ(pick(Comparison::Greater), (std::vector<std::int32_t>{4, 5}));
    EXPECT_EQ(pick(Comparison::GreaterEqual), (std::vector<std::int32_t>{3, 4, 5}));
}

TEST(DdfFilter, GathersAndRepacksAcrossChunks) {
    // 7 rows, chunk_rows=3 -> chunks [3,3,1]; keep evens spread across chunks.
    std::vector<nlohmann::json> rows;
    for (int i = 0; i < 7; ++i) rows.push_back({{"x", i}});
    const zetta::JsonStore store = make_store(rows);
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device, /*chunk_rows=*/3);
    ASSERT_TRUE(scanned.ok());

    // x in {0,2,4,6} pass "x ge 0 and even"? Use a threshold predicate instead:
    // keep x >= 3 -> {3,4,5,6}, spanning chunk boundaries.
    auto kept = filter(scanned.frame, {.column = "x",
                                       .comparison = Comparison::GreaterEqual,
                                       .value = 3.0});
    ASSERT_TRUE(kept.ok()) << kept.status.message;
    EXPECT_EQ(kept.frame.row_count(), 4u);
    EXPECT_EQ(kept.frame.chunk_rows(), 3u);
    EXPECT_EQ(kept.frame.chunk_count(), 2u);  // ceil(4 / 3)
    std::vector<std::int32_t> values;
    ASSERT_TRUE(kept.frame.to_host<std::int32_t>("x", values).ok());
    EXPECT_EQ(values, (std::vector<std::int32_t>{3, 4, 5, 6}));
}

TEST(DdfFilter, SelectingNoneYieldsEmptyFrame) {
    const zetta::JsonStore store = make_store({{{"x", 1}}, {{"x", 2}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device);
    ASSERT_TRUE(scanned.ok());

    auto none = filter(scanned.frame, {.column = "x",
                                       .comparison = Comparison::Greater,
                                       .value = 100.0});
    ASSERT_TRUE(none.ok()) << none.status.message;
    EXPECT_EQ(none.frame.row_count(), 0u);
    EXPECT_EQ(none.frame.column_count(), 1u);
    EXPECT_EQ(none.frame.chunk_count(), 0u);
    ASSERT_NE(none.frame.column("x"), nullptr);
    EXPECT_EQ(none.frame.column("x")->chunk_count(), 0u);
}

TEST(DdfFilter, RejectsUnknownColumn) {
    const zetta::JsonStore store = make_store({{{"x", 1}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device);
    ASSERT_TRUE(scanned.ok());
    auto result = filter(scanned.frame, {.column = "missing"});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DdfFilter, RejectsEmptyFrame) {
    const DataFrame empty;
    auto result = filter(empty, {.column = "x"});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

// --- project -----------------------------------------------------------------

TEST(DdfProject, SelectsAndReordersColumns) {
    const zetta::JsonStore store =
        make_store({{{"a", 1}, {"b", 2}, {"c", 3}}, {{"a", 4}, {"b", 5}, {"c", 6}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"a", "b", "c"}), device);
    ASSERT_TRUE(scanned.ok());

    Projection projection;
    projection.columns = {ProjectColumn::passthrough("c", "c"),
                          ProjectColumn::passthrough("a", "a")};
    auto result = project(scanned.frame, projection);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.frame.column_names(), (std::vector<std::string>{"c", "a"}));
    std::vector<std::int32_t> c;
    ASSERT_TRUE(result.frame.to_host<std::int32_t>("c", c).ok());
    EXPECT_EQ(c, (std::vector<std::int32_t>{3, 6}));
}

TEST(DdfProject, ComputesUnaryAndBinaryColumns) {
    const zetta::JsonStore store =
        make_store({{{"a", 1}, {"b", 10}}, {{"a", 3}, {"b", 30}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"a", "b"}), device);
    ASSERT_TRUE(scanned.ok());

    Projection projection;
    projection.columns = {
        ProjectColumn::unary("nega", UnaryOp::Negate, "a"),
        ProjectColumn::binary("sum", BinaryOp::Add, "a", "b"),
    };
    auto result = project(scanned.frame, projection);
    ASSERT_TRUE(result.ok()) << result.status.message;

    std::vector<std::int32_t> nega;
    ASSERT_TRUE(result.frame.to_host<std::int32_t>("nega", nega).ok());
    EXPECT_EQ(nega, (std::vector<std::int32_t>{-1, -3}));
    std::vector<std::int32_t> sum;
    ASSERT_TRUE(result.frame.to_host<std::int32_t>("sum", sum).ok());
    EXPECT_EQ(sum, (std::vector<std::int32_t>{11, 33}));
}

TEST(DdfProject, ComputesFloatUnary) {
    const zetta::JsonStore store = make_store({{{"x", 4.0}}, {{"x", 9.0}}});
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "x", .dtype = DataType::Float64}};
    CpuDevice device;
    auto scanned = scan(store, schema, device);
    ASSERT_TRUE(scanned.ok());

    Projection projection;
    projection.columns = {ProjectColumn::unary("root", UnaryOp::SquareRoot, "x")};
    auto result = project(scanned.frame, projection);
    ASSERT_TRUE(result.ok()) << result.status.message;
    std::vector<double> root;
    ASSERT_TRUE(result.frame.to_host<double>("root", root).ok());
    EXPECT_EQ(root, (std::vector<double>{2.0, 3.0}));
}

TEST(DdfProject, RejectsFloatOnlyOpOnIntegerColumn) {
    const zetta::JsonStore store = make_store({{{"x", 4}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device);
    ASSERT_TRUE(scanned.ok());

    Projection projection;
    projection.columns = {ProjectColumn::unary("root", UnaryOp::SquareRoot, "x")};
    auto result = project(scanned.frame, projection);
    EXPECT_FALSE(result.ok());  // sqrt requires a floating-point dtype
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DdfProject, RejectsUnknownColumn) {
    const zetta::JsonStore store = make_store({{{"x", 1}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device);
    ASSERT_TRUE(scanned.ok());

    Projection projection;
    projection.columns = {ProjectColumn::passthrough("y", "missing")};
    auto result = project(scanned.frame, projection);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

TEST(DdfProject, RejectsEmptyProjection) {
    const zetta::JsonStore store = make_store({{{"x", 1}}});
    CpuDevice device;
    auto scanned = scan(store, int_schema({"x"}), device);
    ASSERT_TRUE(scanned.ok());
    auto result = project(scanned.frame, Projection{});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status.code, ComputeStatusCode::InvalidArgument);
}

// --- the acceptance: scan -> filter -> project, multi-chunk ------------------

TEST(DdfOps, ScanFilterProjectMatchesExpectedRowsAndCols) {
    const zetta::JsonStore store = make_store({
        {{"a", 1}, {"b", 10}, {"keep", 1}},
        {{"a", 2}, {"b", 20}, {"keep", 0}},
        {{"a", 3}, {"b", 30}, {"keep", 1}},
        {{"a", 4}, {"b", 40}, {"keep", 0}},
        {{"a", 5}, {"b", 50}, {"keep", 1}},
    });
    CpuDevice device;
    // chunk_rows=2 so the kept rows (0, 2, 4) span all three chunks.
    auto scanned = scan(store, int_schema({"a", "b", "keep"}), device,
                        /*chunk_rows=*/2);
    ASSERT_TRUE(scanned.ok()) << scanned.status.message;

    auto kept = filter(scanned.frame,
                       {.column = "keep", .comparison = Comparison::Equal,
                        .value = 1.0});
    ASSERT_TRUE(kept.ok()) << kept.status.message;
    EXPECT_EQ(kept.frame.row_count(), 3u);

    Projection projection;
    projection.columns = {
        ProjectColumn::passthrough("a", "a"),
        ProjectColumn::binary("sum", BinaryOp::Add, "a", "b"),
        ProjectColumn::unary("nega", UnaryOp::Negate, "a"),
    };
    auto out = project(kept.frame, projection);
    ASSERT_TRUE(out.ok()) << out.status.message;

    EXPECT_EQ(out.frame.row_count(), 3u);
    EXPECT_EQ(out.frame.column_count(), 3u);
    EXPECT_EQ(out.frame.column_names(),
              (std::vector<std::string>{"a", "sum", "nega"}));

    std::vector<std::int32_t> a;
    ASSERT_TRUE(out.frame.to_host<std::int32_t>("a", a).ok());
    EXPECT_EQ(a, (std::vector<std::int32_t>{1, 3, 5}));
    std::vector<std::int32_t> sum;
    ASSERT_TRUE(out.frame.to_host<std::int32_t>("sum", sum).ok());
    EXPECT_EQ(sum, (std::vector<std::int32_t>{11, 33, 55}));
    std::vector<std::int32_t> nega;
    ASSERT_TRUE(out.frame.to_host<std::int32_t>("nega", nega).ok());
    EXPECT_EQ(nega, (std::vector<std::int32_t>{-1, -3, -5}));
}

// --- FilterPredicate JSON boundary -------------------------------------------

TEST(DdfFilterPredicateJson, RoundTrips) {
    const FilterPredicate predicate{
        .column = "score", .comparison = Comparison::GreaterEqual, .value = 0.5};
    const auto parsed = FilterPredicate::from_json(predicate.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->column, "score");
    EXPECT_EQ(parsed->comparison, Comparison::GreaterEqual);
    EXPECT_EQ(parsed->value, 0.5);
}

TEST(DdfFilterPredicateJson, RejectsBadInput) {
    using J = nlohmann::json;
    EXPECT_FALSE(FilterPredicate::from_json(J::array()).has_value());
    EXPECT_FALSE(FilterPredicate::from_json(J{{"comparison", "gt"}, {"value", 1}})
                     .has_value());  // missing column
    EXPECT_FALSE(FilterPredicate::from_json(
                     J{{"column", ""}, {"comparison", "gt"}, {"value", 1}})
                     .has_value());  // empty column
    EXPECT_FALSE(FilterPredicate::from_json(J{{"column", "x"}, {"value", 1}})
                     .has_value());  // missing comparison
    EXPECT_FALSE(FilterPredicate::from_json(
                     J{{"column", "x"}, {"comparison", "between"}, {"value", 1}})
                     .has_value());  // unknown comparison
    EXPECT_FALSE(
        FilterPredicate::from_json(J{{"column", "x"}, {"comparison", "gt"}})
            .has_value());  // missing value
    EXPECT_FALSE(FilterPredicate::from_json(
                     J{{"column", "x"}, {"comparison", "gt"}, {"value", "no"}})
                     .has_value());  // value not a number
}

// --- Projection JSON boundary ------------------------------------------------

TEST(DdfProjectionJson, RoundTripsEveryMode) {
    Projection projection;
    projection.columns = {
        ProjectColumn::passthrough("a", "a"),
        ProjectColumn::unary("nega", UnaryOp::Negate, "a"),
        ProjectColumn::binary("sum", BinaryOp::Add, "a", "b"),
    };
    const auto parsed = Projection::from_json(projection.to_json());
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->column_count(), 3u);

    EXPECT_EQ(parsed->columns[0].name, "a");
    EXPECT_EQ(parsed->columns[0].kind, ProjectColumn::Kind::Passthrough);
    EXPECT_EQ(parsed->columns[0].source, "a");

    EXPECT_EQ(parsed->columns[1].name, "nega");
    EXPECT_EQ(parsed->columns[1].kind, ProjectColumn::Kind::Unary);
    EXPECT_EQ(parsed->columns[1].unary_op, UnaryOp::Negate);
    EXPECT_EQ(parsed->columns[1].source, "a");

    EXPECT_EQ(parsed->columns[2].name, "sum");
    EXPECT_EQ(parsed->columns[2].kind, ProjectColumn::Kind::Binary);
    EXPECT_EQ(parsed->columns[2].binary_op, BinaryOp::Add);
    EXPECT_EQ(parsed->columns[2].source, "a");
    EXPECT_EQ(parsed->columns[2].right, "b");
}

TEST(DdfProjectionJson, AcceptsValidDocument) {
    using J = nlohmann::json;
    const J document{
        {"columns",
         {{{"name", "a"}, {"source", "a"}},
          {{"name", "root"}, {"source", "x"}, {"unary", "sqrt"}},
          {{"name", "prod"}, {"binary", "multiply"}, {"left", "a"}, {"right", "b"}}}}};
    const auto parsed = Projection::from_json(document);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->column_count(), 3u);
    EXPECT_EQ(parsed->columns[1].unary_op, UnaryOp::SquareRoot);
    EXPECT_EQ(parsed->columns[2].binary_op, BinaryOp::Multiply);
}

TEST(DdfProjectionJson, RejectsBadInput) {
    using J = nlohmann::json;
    // not an object / missing columns / empty columns
    EXPECT_FALSE(Projection::from_json(J::array()).has_value());
    EXPECT_FALSE(Projection::from_json(J::object()).has_value());
    EXPECT_FALSE(Projection::from_json(J{{"columns", J::array()}}).has_value());
    // missing name
    EXPECT_FALSE(
        Projection::from_json(J{{"columns", {{{"source", "a"}}}}}).has_value());
    // empty name
    EXPECT_FALSE(Projection::from_json(
                     J{{"columns", {{{"name", ""}, {"source", "a"}}}}})
                     .has_value());
    // passthrough missing source
    EXPECT_FALSE(
        Projection::from_json(J{{"columns", {{{"name", "a"}}}}}).has_value());
    // unknown unary op
    EXPECT_FALSE(Projection::from_json(
                     J{{"columns",
                        {{{"name", "a"}, {"source", "a"}, {"unary", "cube"}}}}})
                     .has_value());
    // binary missing right
    EXPECT_FALSE(Projection::from_json(
                     J{{"columns",
                        {{{"name", "a"}, {"binary", "add"}, {"left", "a"}}}}})
                     .has_value());
    // binary mixing in a source
    EXPECT_FALSE(
        Projection::from_json(J{{"columns",
                                 {{{"name", "a"},
                                   {"binary", "add"},
                                   {"left", "a"},
                                   {"right", "b"},
                                   {"source", "c"}}}}})
            .has_value());
    // duplicate output names
    EXPECT_FALSE(Projection::from_json(
                     J{{"columns",
                        {{{"name", "a"}, {"source", "a"}},
                         {{"name", "a"}, {"source", "b"}}}}})
                     .has_value());
}

// --- CUDA parity (where built) -----------------------------------------------
// The ops are generic over any ComputeDevice, so the same scan/filter/project run
// on a CudaDevice must produce the CPU reference's result. Compiled only under
// ZETTA_WITH_CUDA; skipped at runtime when no GPU is present.
#if defined(ZETTA_WITH_CUDA)
}  // namespace
#include "zetta/compute/cuda_device.hpp"
namespace {
using zetta::compute::CudaDevice;

TEST(DdfOpsCudaParity, FilterProjectMatchCpu) {
    CudaDevice cuda;
    const auto info = cuda.info();
    if (info.name == "cuda" || info.name.empty()) {
        GTEST_SKIP() << "no CUDA device present";
    }
    const zetta::JsonStore store = make_store({
        {{"a", 1}, {"b", 10}, {"keep", 1}},
        {{"a", 2}, {"b", 20}, {"keep", 0}},
        {{"a", 3}, {"b", 30}, {"keep", 1}},
    });
    Projection projection;
    projection.columns = {ProjectColumn::binary("sum", BinaryOp::Add, "a", "b")};

    const auto run = [&](zetta::compute::ComputeDevice& device) {
        auto scanned = scan(store, int_schema({"a", "b", "keep"}), device);
        auto kept = filter(scanned.frame, {.column = "keep",
                                           .comparison = Comparison::Equal,
                                           .value = 1.0});
        auto out = project(kept.frame, projection);
        std::vector<std::int32_t> sum;
        EXPECT_TRUE(out.frame.to_host<std::int32_t>("sum", sum).ok());
        return sum;
    };

    CpuDevice cpu;
    EXPECT_EQ(run(cpu), run(cuda));
}
#endif

}  // namespace
