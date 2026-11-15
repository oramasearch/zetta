// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"

// The CPU reference backend (#360). These tests are the numerical reference the
// CUDA backend (#361) is later checked against, so they assert exact results for
// every operator, plus argument-validation behavior and a large-input case that
// exercises the multi-threaded (multi-chunk) path of each data-parallel op.
namespace {

using zetta::compute::BinaryOp;
using zetta::compute::ComputeStatusCode;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::DeviceBuffer;
using zetta::compute::DeviceKind;
using zetta::compute::GemmParams;
using zetta::compute::ReduceOp;
using zetta::compute::ScanInclusivity;
using zetta::compute::SortOrder;
using zetta::compute::UnaryOp;

// Round-trip a host vector through a device op: upload `input`, run `op`, return
// the downloaded result. Templated on element type for reuse across dtypes.
template <class Element>
std::vector<Element> download(CpuDevice& device, const DeviceBuffer& buffer) {
    std::vector<Element> out(buffer.element_count());
    EXPECT_TRUE(device.download<Element>(buffer, out).ok());
    return out;
}

// --- device info -------------------------------------------------------------

TEST(CpuDevice, ReportsCpuInfo) {
    CpuDevice device;
    const auto info = device.info();
    EXPECT_EQ(info.kind, DeviceKind::Cpu);
    EXPECT_EQ(info.name, "cpu");
    EXPECT_TRUE(info.unified_memory);
}

// --- map ---------------------------------------------------------------------

TEST(CpuDevice, MapSquareFloat) {
    CpuDevice device;
    const std::vector<float> source = {1.0f, -2.0f, 3.0f, 0.5f};
    DeviceBuffer in = device.upload<float>(source);
    DeviceBuffer out = device.allocate(DataType::Float32, source.size());

    ASSERT_TRUE(device.map(UnaryOp::Square, in, out).ok());
    const std::vector<float> result = download<float>(device, out);
    EXPECT_FLOAT_EQ(result[0], 1.0f);
    EXPECT_FLOAT_EQ(result[1], 4.0f);
    EXPECT_FLOAT_EQ(result[2], 9.0f);
    EXPECT_FLOAT_EQ(result[3], 0.25f);
}

TEST(CpuDevice, MapNegateInt32) {
    CpuDevice device;
    const std::vector<std::int32_t> source = {1, -2, 3, 0};
    DeviceBuffer in = device.upload<std::int32_t>(source);
    DeviceBuffer out = device.allocate(DataType::Int32, source.size());

    ASSERT_TRUE(device.map(UnaryOp::Negate, in, out).ok());
    const std::vector<std::int32_t> result = download<std::int32_t>(device, out);
    EXPECT_EQ(result, (std::vector<std::int32_t>{-1, 2, -3, 0}));
}

TEST(CpuDevice, MapSqrtRejectsIntegerDtype) {
    CpuDevice device;
    DeviceBuffer in = device.allocate(DataType::Int32, 4);
    DeviceBuffer out = device.allocate(DataType::Int32, 4);
    const auto status = device.map(UnaryOp::SquareRoot, in, out);
    EXPECT_EQ(status.code, ComputeStatusCode::InvalidArgument);
}

TEST(CpuDevice, MapRejectsCountMismatch) {
    CpuDevice device;
    DeviceBuffer in = device.allocate(DataType::Float32, 4);
    DeviceBuffer out = device.allocate(DataType::Float32, 3);
    EXPECT_EQ(device.map(UnaryOp::Square, in, out).code,
              ComputeStatusCode::InvalidArgument);
}

// --- elementwise -------------------------------------------------------------

TEST(CpuDevice, ElementwiseAddAndMaxFloat) {
    CpuDevice device;
    const std::vector<float> a = {1.0f, 5.0f, 3.0f};
    const std::vector<float> b = {4.0f, 2.0f, 6.0f};
    DeviceBuffer lhs = device.upload<float>(a);
    DeviceBuffer rhs = device.upload<float>(b);
    DeviceBuffer out = device.allocate(DataType::Float32, a.size());

    ASSERT_TRUE(device.elementwise(BinaryOp::Add, lhs, rhs, out).ok());
    EXPECT_EQ(download<float>(device, out),
              (std::vector<float>{5.0f, 7.0f, 9.0f}));

    ASSERT_TRUE(device.elementwise(BinaryOp::Maximum, lhs, rhs, out).ok());
    EXPECT_EQ(download<float>(device, out),
              (std::vector<float>{4.0f, 5.0f, 6.0f}));
}

TEST(CpuDevice, ElementwiseIntegerDivideByZeroIsSafe) {
    CpuDevice device;
    const std::vector<std::int32_t> a = {10, 20, 30};
    const std::vector<std::int32_t> b = {2, 0, 5};
    DeviceBuffer lhs = device.upload<std::int32_t>(a);
    DeviceBuffer rhs = device.upload<std::int32_t>(b);
    DeviceBuffer out = device.allocate(DataType::Int32, a.size());

    ASSERT_TRUE(device.elementwise(BinaryOp::Divide, lhs, rhs, out).ok());
    EXPECT_EQ(download<std::int32_t>(device, out),
              (std::vector<std::int32_t>{5, 0, 6}));  // 20/0 -> 0, not a crash
}

// --- reduce ------------------------------------------------------------------

TEST(CpuDevice, ReduceSumAndMax) {
    CpuDevice device;
    const std::vector<double> source = {1.0, 2.0, 3.0, 4.0, 5.0};
    DeviceBuffer in = device.upload<double>(source);
    DeviceBuffer out = device.allocate(DataType::Float64, 1);

    ASSERT_TRUE(device.reduce(ReduceOp::Sum, in, out).ok());
    EXPECT_DOUBLE_EQ(download<double>(device, out)[0], 15.0);

    ASSERT_TRUE(device.reduce(ReduceOp::Maximum, in, out).ok());
    EXPECT_DOUBLE_EQ(download<double>(device, out)[0], 5.0);

    ASSERT_TRUE(device.reduce(ReduceOp::Product, in, out).ok());
    EXPECT_DOUBLE_EQ(download<double>(device, out)[0], 120.0);
}

TEST(CpuDevice, ReduceSumLargeInputMultiThreaded) {
    CpuDevice device;
    constexpr std::size_t count = 1'000'000;
    std::vector<std::int64_t> source(count, 1);
    DeviceBuffer in = device.upload<std::int64_t>(source);
    DeviceBuffer out = device.allocate(DataType::Int64, 1);

    ASSERT_TRUE(device.reduce(ReduceOp::Sum, in, out).ok());
    EXPECT_EQ(download<std::int64_t>(device, out)[0],
              static_cast<std::int64_t>(count));
}

// --- scan --------------------------------------------------------------------

TEST(CpuDevice, ScanInclusiveAndExclusive) {
    CpuDevice device;
    const std::vector<std::int32_t> source = {1, 2, 3, 4};
    DeviceBuffer in = device.upload<std::int32_t>(source);
    DeviceBuffer out = device.allocate(DataType::Int32, source.size());

    ASSERT_TRUE(
        device.scan(ReduceOp::Sum, ScanInclusivity::Inclusive, in, out).ok());
    EXPECT_EQ(download<std::int32_t>(device, out),
              (std::vector<std::int32_t>{1, 3, 6, 10}));

    ASSERT_TRUE(
        device.scan(ReduceOp::Sum, ScanInclusivity::Exclusive, in, out).ok());
    EXPECT_EQ(download<std::int32_t>(device, out),
              (std::vector<std::int32_t>{0, 1, 3, 6}));
}

TEST(CpuDevice, ScanLargeInputMatchesSequential) {
    CpuDevice device;
    constexpr std::size_t count = 500'000;
    std::vector<std::int64_t> source(count, 1);
    DeviceBuffer in = device.upload<std::int64_t>(source);
    DeviceBuffer out = device.allocate(DataType::Int64, count);

    ASSERT_TRUE(
        device.scan(ReduceOp::Sum, ScanInclusivity::Inclusive, in, out).ok());
    const std::vector<std::int64_t> result = download<std::int64_t>(device, out);
    // Inclusive prefix sum of all-ones is 1, 2, 3, ... so the multi-chunk
    // three-phase scan must line its phases up exactly.
    for (std::size_t i = 0; i < count; ++i)
        ASSERT_EQ(result[i], static_cast<std::int64_t>(i + 1)) << "at " << i;
}

// --- histogram ---------------------------------------------------------------

TEST(CpuDevice, HistogramCountsIntoBins) {
    CpuDevice device;
    // Values in [0, 10) into 5 bins of width 2: bins are [0,2) [2,4) ... [8,10).
    const std::vector<float> source = {0.0f, 1.5f, 2.0f, 3.9f, 4.0f,
                                       7.0f, 9.99f, 10.0f, -1.0f};
    DeviceBuffer in = device.upload<float>(source);
    DeviceBuffer out = device.allocate(DataType::UInt32, 5);

    ASSERT_TRUE(device.histogram(in, 5, 0.0, 10.0, out).ok());
    // Bins [0,2) [2,4) [4,6) [6,8) [8,10): 0.0,1.5 -> b0; 2.0,3.9 -> b1;
    // 4.0 -> b2; 7.0 -> b3; 9.99 -> b4. 10.0 and -1.0 fall outside and drop.
    EXPECT_EQ(download<std::uint32_t>(device, out),
              (std::vector<std::uint32_t>{2, 2, 1, 1, 1}));
}

TEST(CpuDevice, HistogramRejectsBadRange) {
    CpuDevice device;
    DeviceBuffer in = device.allocate(DataType::Float32, 4);
    DeviceBuffer out = device.allocate(DataType::UInt32, 4);
    EXPECT_EQ(device.histogram(in, 4, 5.0, 5.0, out).code,
              ComputeStatusCode::InvalidArgument);
    EXPECT_EQ(device.histogram(in, 3, 0.0, 1.0, out).code,
              ComputeStatusCode::InvalidArgument);  // bin_count != out size
}

// --- gemm --------------------------------------------------------------------

TEST(CpuDevice, GemmBasicProduct) {
    CpuDevice device;
    // A (2x3) * B (3x2) = C (2x2), alpha=1, beta=0.
    const std::vector<float> a = {1, 2, 3, 4, 5, 6};       // row-major 2x3
    const std::vector<float> b = {7, 8, 9, 10, 11, 12};    // row-major 3x2
    DeviceBuffer da = device.upload<float>(a);
    DeviceBuffer db = device.upload<float>(b);
    DeviceBuffer dc = device.allocate(DataType::Float32, 4);

    GemmParams params;
    params.rows = 2;
    params.inner = 3;
    params.cols = 2;
    params.alpha = 1.0;
    params.beta = 0.0;
    ASSERT_TRUE(device.gemm(params, da, db, dc).ok());

    // [1 2 3; 4 5 6] * [7 8; 9 10; 11 12] = [58 64; 139 154]
    EXPECT_EQ(download<float>(device, dc),
              (std::vector<float>{58, 64, 139, 154}));
}

TEST(CpuDevice, GemmTransposeAndBetaAccumulate) {
    CpuDevice device;
    // op(A) = A^T where A is stored 2x2; with beta=1 it accumulates into C.
    const std::vector<double> a = {1, 2, 3, 4};  // stored 2x2 row-major
    const std::vector<double> b = {1, 0, 0, 1};  // identity 2x2
    const std::vector<double> c0 = {10, 20, 30, 40};
    DeviceBuffer da = device.upload<double>(a);
    DeviceBuffer db = device.upload<double>(b);
    DeviceBuffer dc = device.upload<double>(c0);

    GemmParams params;
    params.rows = 2;
    params.inner = 2;
    params.cols = 2;
    params.alpha = 1.0;
    params.beta = 1.0;
    params.transpose_a = true;
    ASSERT_TRUE(device.gemm(params, da, db, dc).ok());

    // A^T = [1 3; 2 4]; times identity = [1 3; 2 4]; plus C0 = [11 23; 32 44].
    EXPECT_EQ(download<double>(device, dc),
              (std::vector<double>{11, 23, 32, 44}));
}

TEST(CpuDevice, GemmRejectsIntegerDtype) {
    CpuDevice device;
    DeviceBuffer da = device.allocate(DataType::Int32, 4);
    DeviceBuffer db = device.allocate(DataType::Int32, 4);
    DeviceBuffer dc = device.allocate(DataType::Int32, 4);
    GemmParams params;
    params.rows = params.inner = params.cols = 2;
    EXPECT_EQ(device.gemm(params, da, db, dc).code,
              ComputeStatusCode::InvalidArgument);
}

// --- axpy --------------------------------------------------------------------

TEST(CpuDevice, AxpyUpdatesYInPlace) {
    CpuDevice device;
    const std::vector<float> x = {1.0f, 2.0f, 3.0f};
    const std::vector<float> y0 = {10.0f, 20.0f, 30.0f};
    DeviceBuffer x_buf = device.upload<float>(x);
    DeviceBuffer y_buf = device.upload<float>(y0);

    ASSERT_TRUE(device.axpy(2.0, x_buf, y_buf).ok());  // y = 2x + y
    EXPECT_EQ(download<float>(device, y_buf),
              (std::vector<float>{12.0f, 24.0f, 36.0f}));
}

// --- sort --------------------------------------------------------------------

TEST(CpuDevice, SortAscendingAndDescending) {
    CpuDevice device;
    const std::vector<std::int32_t> source = {5, 1, 4, 2, 3};
    DeviceBuffer keys = device.upload<std::int32_t>(source);

    ASSERT_TRUE(device.sort(keys, SortOrder::Ascending).ok());
    EXPECT_EQ(download<std::int32_t>(device, keys),
              (std::vector<std::int32_t>{1, 2, 3, 4, 5}));

    ASSERT_TRUE(device.sort(keys, SortOrder::Descending).ok());
    EXPECT_EQ(download<std::int32_t>(device, keys),
              (std::vector<std::int32_t>{5, 4, 3, 2, 1}));
}

TEST(CpuDevice, SortByKeyCarriesValues) {
    CpuDevice device;
    // Rank entities by risk score, carrying their ids (the documented use case).
    const std::vector<float> scores = {0.4f, 0.9f, 0.1f, 0.7f};
    const std::vector<std::int64_t> ids = {10, 11, 12, 13};
    DeviceBuffer key_buf = device.upload<float>(scores);
    DeviceBuffer val_buf = device.upload<std::int64_t>(ids);

    ASSERT_TRUE(device.sort_by_key(key_buf, val_buf, SortOrder::Descending).ok());
    EXPECT_EQ(download<float>(device, key_buf),
              (std::vector<float>{0.9f, 0.7f, 0.4f, 0.1f}));
    EXPECT_EQ(download<std::int64_t>(device, val_buf),
              (std::vector<std::int64_t>{11, 13, 10, 12}));
}

// --- gather / scatter --------------------------------------------------------

TEST(CpuDevice, GatherSelectsByIndex) {
    CpuDevice device;
    const std::vector<float> source = {10.0f, 20.0f, 30.0f, 40.0f};
    const std::vector<std::int32_t> indices = {3, 1, 0, 2, 1};
    DeviceBuffer in = device.upload<float>(source);
    DeviceBuffer idx = device.upload<std::int32_t>(indices);
    DeviceBuffer out = device.allocate(DataType::Float32, indices.size());

    ASSERT_TRUE(device.gather(in, idx, out).ok());
    EXPECT_EQ(download<float>(device, out),
              (std::vector<float>{40.0f, 20.0f, 10.0f, 30.0f, 20.0f}));
}

TEST(CpuDevice, GatherRejectsOutOfRangeIndex) {
    CpuDevice device;
    const std::vector<float> source = {1.0f, 2.0f};
    const std::vector<std::int32_t> indices = {0, 5};  // 5 is out of range
    DeviceBuffer in = device.upload<float>(source);
    DeviceBuffer idx = device.upload<std::int32_t>(indices);
    DeviceBuffer out = device.allocate(DataType::Float32, indices.size());

    EXPECT_EQ(device.gather(in, idx, out).code,
              ComputeStatusCode::InvalidArgument);
}

TEST(CpuDevice, ScatterPlacesByIndex) {
    CpuDevice device;
    const std::vector<float> source = {10.0f, 20.0f, 30.0f};
    const std::vector<std::int32_t> indices = {2, 0, 1};
    DeviceBuffer in = device.upload<float>(source);
    DeviceBuffer idx = device.upload<std::int32_t>(indices);
    DeviceBuffer out = device.allocate(DataType::Float32, 3);

    ASSERT_TRUE(device.scatter(in, idx, out).ok());
    EXPECT_EQ(download<float>(device, out),
              (std::vector<float>{20.0f, 30.0f, 10.0f}));
}

// --- large-input data-parallel sanity ---------------------------------------

TEST(CpuDevice, MapLargeInputMultiThreaded) {
    CpuDevice device;
    constexpr std::size_t count = 1'000'000;
    std::vector<float> source(count);
    for (std::size_t i = 0; i < count; ++i)
        source[i] = static_cast<float>(i);
    DeviceBuffer in = device.upload<float>(source);
    DeviceBuffer out = device.allocate(DataType::Float32, count);

    ASSERT_TRUE(device.map(UnaryOp::Square, in, out).ok());
    const std::vector<float> result = download<float>(device, out);
    for (std::size_t i = 0; i < count; i += 100'000)
        ASSERT_FLOAT_EQ(result[i], source[i] * source[i]) << "at " << i;
}

}  // namespace
