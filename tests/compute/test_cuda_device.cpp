#include <gtest/gtest.h>

// CPU<->CUDA conformance suite — the acceptance test of #361 ("each operator
// matches the CPU reference within tolerance"). For every operator it runs the
// same inputs through both CpuDevice (the numerical reference, #360) and
// CudaDevice (the GPU backend) and asserts they agree: bit-identical for integer
// ops, within a small tolerance for floating-point ones. It also pins the
// edge-case parity that distinguishes a correct backend from one that merely
// computes the happy path — integer-dtype rejection, integer-divide-by-zero
// safety, out-of-range gather/scatter rejection, and deterministic duplicate-index
// scatter.
//
// The whole body compiles only under ZETTA_WITH_CUDA (the define CMake sets for a
// -DZETTA_ENABLE_CUDA=ON build), so a CPU-only build (dev Macs, CI) sees an empty
// translation unit. When CUDA is compiled in but no GPU is present, every case
// skips at runtime — so a toolkit-but-no-GPU host still passes. Run it on a Linux
// CUDA host to actually exercise the GPU kernels.
#if defined(ZETTA_WITH_CUDA)

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/cuda_device.hpp"
#include "zetta/compute/data_type.hpp"

namespace {

using zetta::compute::BinaryOp;
using zetta::compute::ComputeDevice;
using zetta::compute::ComputeStatusCode;
using zetta::compute::CpuDevice;
using zetta::compute::CudaDevice;
using zetta::compute::data_type_of;
using zetta::compute::DataType;
using zetta::compute::DeviceBuffer;
using zetta::compute::GemmParams;
using zetta::compute::ReduceOp;
using zetta::compute::ScanInclusivity;
using zetta::compute::SortOrder;
using zetta::compute::UnaryOp;

// --- per-operator runners over the shared ComputeDevice seam ------------------
// Each uploads host data, runs one operator, downloads, and returns the result —
// so the same call site drives a CpuDevice and a CudaDevice identically.

template <class Element>
std::vector<Element> map_on(ComputeDevice& device, UnaryOp op,
                            const std::vector<Element>& input) {
    DeviceBuffer in = device.upload<Element>(input);
    DeviceBuffer out = device.allocate(data_type_of<Element>, input.size());
    EXPECT_TRUE(device.map(op, in, out).ok());
    std::vector<Element> result(input.size());
    EXPECT_TRUE(device.download<Element>(out, result).ok());
    return result;
}

template <class Element>
std::vector<Element> elementwise_on(ComputeDevice& device, BinaryOp op,
                                    const std::vector<Element>& lhs,
                                    const std::vector<Element>& rhs) {
    DeviceBuffer a = device.upload<Element>(lhs);
    DeviceBuffer b = device.upload<Element>(rhs);
    DeviceBuffer out = device.allocate(data_type_of<Element>, lhs.size());
    EXPECT_TRUE(device.elementwise(op, a, b, out).ok());
    std::vector<Element> result(lhs.size());
    EXPECT_TRUE(device.download<Element>(out, result).ok());
    return result;
}

template <class Element>
Element reduce_on(ComputeDevice& device, ReduceOp op,
                  const std::vector<Element>& input) {
    DeviceBuffer in = device.upload<Element>(input);
    DeviceBuffer out = device.allocate(data_type_of<Element>, 1);
    EXPECT_TRUE(device.reduce(op, in, out).ok());
    std::vector<Element> result(1);
    EXPECT_TRUE(device.download<Element>(out, result).ok());
    return result[0];
}

template <class Element>
std::vector<Element> scan_on(ComputeDevice& device, ReduceOp op,
                             ScanInclusivity inclusivity,
                             const std::vector<Element>& input) {
    DeviceBuffer in = device.upload<Element>(input);
    DeviceBuffer out = device.allocate(data_type_of<Element>, input.size());
    EXPECT_TRUE(device.scan(op, inclusivity, in, out).ok());
    std::vector<Element> result(input.size());
    EXPECT_TRUE(device.download<Element>(out, result).ok());
    return result;
}

template <class Count>
std::vector<Count> histogram_on(ComputeDevice& device,
                                const std::vector<float>& input,
                                std::size_t bins, double range_min,
                                double range_max) {
    DeviceBuffer in = device.upload<float>(input);
    DeviceBuffer out = device.allocate(data_type_of<Count>, bins);
    EXPECT_TRUE(device.histogram(in, bins, range_min, range_max, out).ok());
    std::vector<Count> result(bins);
    EXPECT_TRUE(device.download<Count>(out, result).ok());
    return result;
}

std::vector<float> gemm_on(ComputeDevice& device, const GemmParams& params,
                           const std::vector<float>& a,
                           const std::vector<float>& b,
                           const std::vector<float>& c0) {
    DeviceBuffer da = device.upload<float>(a);
    DeviceBuffer db = device.upload<float>(b);
    DeviceBuffer dc = device.upload<float>(c0);
    EXPECT_TRUE(device.gemm(params, da, db, dc).ok());
    std::vector<float> result(c0.size());
    EXPECT_TRUE(device.download<float>(dc, result).ok());
    return result;
}

std::vector<float> axpy_on(ComputeDevice& device, double alpha,
                           const std::vector<float>& x,
                           const std::vector<float>& y0) {
    DeviceBuffer dx = device.upload<float>(x);
    DeviceBuffer dy = device.upload<float>(y0);
    EXPECT_TRUE(device.axpy(alpha, dx, dy).ok());
    std::vector<float> result(y0.size());
    EXPECT_TRUE(device.download<float>(dy, result).ok());
    return result;
}

template <class Element>
std::vector<Element> sort_on(ComputeDevice& device,
                             const std::vector<Element>& input,
                             SortOrder order) {
    DeviceBuffer keys = device.upload<Element>(input);
    EXPECT_TRUE(device.sort(keys, order).ok());
    std::vector<Element> result(input.size());
    EXPECT_TRUE(device.download<Element>(keys, result).ok());
    return result;
}

template <class Key, class Value>
std::pair<std::vector<Key>, std::vector<Value>> sort_by_key_on(
    ComputeDevice& device, const std::vector<Key>& keys,
    const std::vector<Value>& values, SortOrder order) {
    DeviceBuffer key_buf = device.upload<Key>(keys);
    DeviceBuffer value_buf = device.upload<Value>(values);
    EXPECT_TRUE(device.sort_by_key(key_buf, value_buf, order).ok());
    std::pair<std::vector<Key>, std::vector<Value>> result{
        std::vector<Key>(keys.size()), std::vector<Value>(values.size())};
    EXPECT_TRUE(device.download<Key>(key_buf, result.first).ok());
    EXPECT_TRUE(device.download<Value>(value_buf, result.second).ok());
    return result;
}

template <class Element, class Index>
std::vector<Element> gather_on(ComputeDevice& device,
                               const std::vector<Element>& input,
                               const std::vector<Index>& indices) {
    DeviceBuffer in = device.upload<Element>(input);
    DeviceBuffer idx = device.upload<Index>(indices);
    DeviceBuffer out = device.allocate(data_type_of<Element>, indices.size());
    EXPECT_TRUE(device.gather(in, idx, out).ok());
    std::vector<Element> result(indices.size());
    EXPECT_TRUE(device.download<Element>(out, result).ok());
    return result;
}

template <class Element, class Index>
std::vector<Element> scatter_on(ComputeDevice& device,
                                const std::vector<Element>& input,
                                const std::vector<Index>& indices,
                                std::size_t output_size) {
    DeviceBuffer in = device.upload<Element>(input);
    DeviceBuffer idx = device.upload<Index>(indices);
    DeviceBuffer out = device.allocate(data_type_of<Element>, output_size);
    // Zero the output first so slots no index targets are a defined value on both
    // backends (the operator leaves unscattered positions untouched).
    const std::vector<Element> zeros(output_size, Element{0});
    EXPECT_TRUE(
        device.copy_from_host(zeros.data(), out, output_size * sizeof(Element))
            .ok());
    EXPECT_TRUE(device.scatter(in, idx, out).ok());
    std::vector<Element> result(output_size);
    EXPECT_TRUE(device.download<Element>(out, result).ok());
    return result;
}

// --- comparison helpers -------------------------------------------------------

void expect_close(const std::vector<float>& reference,
                  const std::vector<float>& candidate, float tolerance = 1e-4f) {
    ASSERT_EQ(reference.size(), candidate.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
        EXPECT_NEAR(reference[i], candidate[i], tolerance) << "at " << i;
}

void expect_close(const std::vector<double>& reference,
                  const std::vector<double>& candidate,
                  double tolerance = 1e-9) {
    ASSERT_EQ(reference.size(), candidate.size());
    for (std::size_t i = 0; i < reference.size(); ++i)
        EXPECT_NEAR(reference[i], candidate[i], tolerance) << "at " << i;
}

// Both devices live for the suite; SetUp skips every case when no GPU is present
// so a toolkit-but-no-GPU host still passes.
class CudaConformance : public ::testing::Test {
protected:
    CpuDevice cpu;
    CudaDevice cuda;

    void SetUp() override {
        const auto info = cuda.info();
        if (info.name == "cuda" || info.name.empty()) {
            GTEST_SKIP() << "no CUDA device present";
        }
    }
};

// --- operator conformance (CUDA == CPU on identical inputs) -------------------

TEST_F(CudaConformance, MapMatchesFloat) {
    const std::vector<float> source = {0.25f, 1.0f, 2.0f, 3.5f, 0.5f};
    for (UnaryOp op : {UnaryOp::Negate, UnaryOp::Absolute, UnaryOp::Square,
                       UnaryOp::SquareRoot, UnaryOp::Exponential,
                       UnaryOp::Logarithm}) {
        expect_close(map_on<float>(cpu, op, source),
                     map_on<float>(cuda, op, source), 1e-3f);
    }
}

TEST_F(CudaConformance, MapMatchesInt32) {
    const std::vector<std::int32_t> source = {1, -2, 3, -4, 0};
    for (UnaryOp op : {UnaryOp::Negate, UnaryOp::Absolute, UnaryOp::Square}) {
        EXPECT_EQ(map_on<std::int32_t>(cpu, op, source),
                  map_on<std::int32_t>(cuda, op, source));
    }
}

TEST_F(CudaConformance, ElementwiseMatchesFloat) {
    const std::vector<float> a = {1.0f, 5.0f, 3.0f, 8.0f};
    const std::vector<float> b = {4.0f, 2.0f, 6.0f, 2.0f};
    for (BinaryOp op : {BinaryOp::Add, BinaryOp::Subtract, BinaryOp::Multiply,
                        BinaryOp::Divide, BinaryOp::Maximum, BinaryOp::Minimum}) {
        expect_close(elementwise_on<float>(cpu, op, a, b),
                     elementwise_on<float>(cuda, op, a, b));
    }
}

TEST_F(CudaConformance, ReduceMatches) {
    const std::vector<double> source = {1.0, 2.0, 3.0, 4.0, 5.0};
    for (ReduceOp op : {ReduceOp::Sum, ReduceOp::Product, ReduceOp::Minimum,
                        ReduceOp::Maximum}) {
        EXPECT_NEAR(reduce_on<double>(cpu, op, source),
                    reduce_on<double>(cuda, op, source), 1e-9);
    }
    const std::vector<std::int64_t> ints = {5, 1, 4, 2, 3};
    EXPECT_EQ(reduce_on<std::int64_t>(cpu, ReduceOp::Sum, ints),
              reduce_on<std::int64_t>(cuda, ReduceOp::Sum, ints));
}

TEST_F(CudaConformance, ScanMatches) {
    const std::vector<std::int32_t> source = {1, 2, 3, 4, 5};
    for (ScanInclusivity inclusivity :
         {ScanInclusivity::Inclusive, ScanInclusivity::Exclusive}) {
        EXPECT_EQ(scan_on<std::int32_t>(cpu, ReduceOp::Sum, inclusivity, source),
                  scan_on<std::int32_t>(cuda, ReduceOp::Sum, inclusivity, source));
    }
}

TEST_F(CudaConformance, HistogramMatches) {
    const std::vector<float> source = {0.0f, 1.5f, 2.0f, 3.9f, 4.0f,
                                       7.0f, 9.99f, 10.0f, -1.0f};
    EXPECT_EQ(histogram_on<std::uint32_t>(cpu, source, 5, 0.0, 10.0),
              histogram_on<std::uint32_t>(cuda, source, 5, 0.0, 10.0));
}

TEST_F(CudaConformance, GemmMatches) {
    const std::vector<float> a = {1, 2, 3, 4, 5, 6};      // 2x3 row-major
    const std::vector<float> b = {7, 8, 9, 10, 11, 12};   // 3x2 row-major
    const std::vector<float> c0 = {0, 0, 0, 0};
    GemmParams params;
    params.rows = 2;
    params.inner = 3;
    params.cols = 2;
    expect_close(gemm_on(cpu, params, a, b, c0),
                 gemm_on(cuda, params, a, b, c0));

    // Transpose + beta-accumulate path.
    const std::vector<float> ta = {1, 2, 3, 4};  // stored 2x2
    const std::vector<float> tb = {1, 0, 0, 1};  // identity 2x2
    const std::vector<float> tc = {10, 20, 30, 40};
    GemmParams tp;
    tp.rows = tp.inner = tp.cols = 2;
    tp.beta = 1.0;
    tp.transpose_a = true;
    expect_close(gemm_on(cpu, tp, ta, tb, tc), gemm_on(cuda, tp, ta, tb, tc));
}

TEST_F(CudaConformance, AxpyMatches) {
    const std::vector<float> x = {1.0f, 2.0f, 3.0f, 4.0f};
    const std::vector<float> y = {10.0f, 20.0f, 30.0f, 40.0f};
    expect_close(axpy_on(cpu, 2.5, x, y), axpy_on(cuda, 2.5, x, y));
}

TEST_F(CudaConformance, SortMatches) {
    const std::vector<std::int32_t> source = {5, 1, 4, 2, 3, 0, 9, 7};
    for (SortOrder order : {SortOrder::Ascending, SortOrder::Descending}) {
        EXPECT_EQ(sort_on<std::int32_t>(cpu, source, order),
                  sort_on<std::int32_t>(cuda, source, order));
    }
}

TEST_F(CudaConformance, SortByKeyMatches) {
    const std::vector<float> scores = {0.4f, 0.9f, 0.1f, 0.7f};
    const std::vector<std::int64_t> ids = {10, 11, 12, 13};
    const auto cpu_result =
        sort_by_key_on<float, std::int64_t>(cpu, scores, ids, SortOrder::Descending);
    const auto cuda_result =
        sort_by_key_on<float, std::int64_t>(cuda, scores, ids, SortOrder::Descending);
    expect_close(cpu_result.first, cuda_result.first);
    EXPECT_EQ(cpu_result.second, cuda_result.second);
}

TEST_F(CudaConformance, GatherMatches) {
    const std::vector<float> source = {10.0f, 20.0f, 30.0f, 40.0f};
    const std::vector<std::int32_t> indices = {3, 1, 0, 2, 1};
    expect_close(gather_on<float, std::int32_t>(cpu, source, indices),
                 gather_on<float, std::int32_t>(cuda, source, indices));
}

TEST_F(CudaConformance, ScatterMatches) {
    const std::vector<float> source = {10.0f, 20.0f, 30.0f};
    const std::vector<std::int32_t> indices = {2, 0, 1};
    expect_close(scatter_on<float, std::int32_t>(cpu, source, indices, 3),
                 scatter_on<float, std::int32_t>(cuda, source, indices, 3));
}

// --- edge-case parity ---------------------------------------------------------

TEST_F(CudaConformance, MapSqrtIntegerRejectedOnBoth) {
    DeviceBuffer cpu_in = cpu.allocate(DataType::Int32, 4);
    DeviceBuffer cpu_out = cpu.allocate(DataType::Int32, 4);
    DeviceBuffer cuda_in = cuda.allocate(DataType::Int32, 4);
    DeviceBuffer cuda_out = cuda.allocate(DataType::Int32, 4);
    EXPECT_EQ(cpu.map(UnaryOp::SquareRoot, cpu_in, cpu_out).code,
              ComputeStatusCode::InvalidArgument);
    EXPECT_EQ(cuda.map(UnaryOp::SquareRoot, cuda_in, cuda_out).code,
              ComputeStatusCode::InvalidArgument);
}

TEST_F(CudaConformance, ElementwiseIntDivideByZeroSafeOnBoth) {
    const std::vector<std::int32_t> a = {10, 20, 30};
    const std::vector<std::int32_t> b = {2, 0, 5};  // a stray zero divisor
    const auto cpu_result = elementwise_on<std::int32_t>(cpu, BinaryOp::Divide, a, b);
    const auto cuda_result = elementwise_on<std::int32_t>(cuda, BinaryOp::Divide, a, b);
    EXPECT_EQ(cpu_result, (std::vector<std::int32_t>{5, 0, 6}));  // 20/0 -> 0
    EXPECT_EQ(cuda_result, cpu_result);
}

TEST_F(CudaConformance, GemmIntegerRejectedOnBoth) {
    DeviceBuffer cpu_a = cpu.allocate(DataType::Int32, 4);
    DeviceBuffer cpu_b = cpu.allocate(DataType::Int32, 4);
    DeviceBuffer cpu_c = cpu.allocate(DataType::Int32, 4);
    DeviceBuffer cuda_a = cuda.allocate(DataType::Int32, 4);
    DeviceBuffer cuda_b = cuda.allocate(DataType::Int32, 4);
    DeviceBuffer cuda_c = cuda.allocate(DataType::Int32, 4);
    GemmParams params;
    params.rows = params.inner = params.cols = 2;
    EXPECT_EQ(cpu.gemm(params, cpu_a, cpu_b, cpu_c).code,
              ComputeStatusCode::InvalidArgument);
    EXPECT_EQ(cuda.gemm(params, cuda_a, cuda_b, cuda_c).code,
              ComputeStatusCode::InvalidArgument);
}

TEST_F(CudaConformance, AxpyIntegerRejectedOnBoth) {
    DeviceBuffer cpu_x = cpu.allocate(DataType::Int32, 4);
    DeviceBuffer cpu_y = cpu.allocate(DataType::Int32, 4);
    DeviceBuffer cuda_x = cuda.allocate(DataType::Int32, 4);
    DeviceBuffer cuda_y = cuda.allocate(DataType::Int32, 4);
    EXPECT_EQ(cpu.axpy(1.0, cpu_x, cpu_y).code,
              ComputeStatusCode::InvalidArgument);
    EXPECT_EQ(cuda.axpy(1.0, cuda_x, cuda_y).code,
              ComputeStatusCode::InvalidArgument);
}

TEST_F(CudaConformance, GatherOutOfRangeRejectedOnBoth) {
    const std::vector<float> source = {1.0f, 2.0f};
    const std::vector<std::int32_t> indices = {0, 5};  // 5 is out of range
    DeviceBuffer cpu_in = cpu.upload<float>(source);
    DeviceBuffer cpu_idx = cpu.upload<std::int32_t>(indices);
    DeviceBuffer cpu_out = cpu.allocate(DataType::Float32, indices.size());
    DeviceBuffer cuda_in = cuda.upload<float>(source);
    DeviceBuffer cuda_idx = cuda.upload<std::int32_t>(indices);
    DeviceBuffer cuda_out = cuda.allocate(DataType::Float32, indices.size());
    EXPECT_EQ(cpu.gather(cpu_in, cpu_idx, cpu_out).code,
              ComputeStatusCode::InvalidArgument);
    EXPECT_EQ(cuda.gather(cuda_in, cuda_idx, cuda_out).code,
              ComputeStatusCode::InvalidArgument);
}

TEST_F(CudaConformance, ScatterOutOfRangeRejectedOnBoth) {
    const std::vector<float> source = {10.0f, 20.0f};
    const std::vector<std::int32_t> indices = {0, 7};  // 7 is out of range
    DeviceBuffer cpu_in = cpu.upload<float>(source);
    DeviceBuffer cpu_idx = cpu.upload<std::int32_t>(indices);
    DeviceBuffer cpu_out = cpu.allocate(DataType::Float32, 3);
    DeviceBuffer cuda_in = cuda.upload<float>(source);
    DeviceBuffer cuda_idx = cuda.upload<std::int32_t>(indices);
    DeviceBuffer cuda_out = cuda.allocate(DataType::Float32, 3);
    EXPECT_EQ(cpu.scatter(cpu_in, cpu_idx, cpu_out).code,
              ComputeStatusCode::InvalidArgument);
    EXPECT_EQ(cuda.scatter(cuda_in, cuda_idx, cuda_out).code,
              ComputeStatusCode::InvalidArgument);
}

TEST_F(CudaConformance, ScatterDuplicateIndicesLastWriterWins) {
    // Slot 1 is targeted by inputs 0, 1, and 3; the last writer (largest input
    // index, 3) must win on both backends, so the duplicate scatter is
    // deterministic rather than a race.
    const std::vector<float> source = {10.0f, 20.0f, 30.0f, 40.0f};
    const std::vector<std::int32_t> indices = {1, 1, 2, 1};
    const auto cpu_result = scatter_on<float, std::int32_t>(cpu, source, indices, 3);
    const auto cuda_result = scatter_on<float, std::int32_t>(cuda, source, indices, 3);
    EXPECT_EQ(cpu_result, (std::vector<float>{0.0f, 40.0f, 30.0f}));
    EXPECT_EQ(cuda_result, cpu_result);
}

}  // namespace

#endif  // ZETTA_WITH_CUDA
