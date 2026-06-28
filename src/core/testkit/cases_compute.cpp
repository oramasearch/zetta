#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "cases.hpp"
#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/testkit/test_suite.hpp"

// Compute & device cases — exercise the ComputeDevice operator seam on the
// always-built CpuDevice reference backend (and report the CUDA build state). These
// are the leaf primitives every heavy op composes (map / elementwise / reduce /
// sort_by_key).
namespace zetta::testkit {
namespace {

using compute::BinaryOp;
using compute::CpuDevice;
using compute::DataType;
using compute::DeviceBuffer;
using compute::ReduceOp;
using compute::SortOrder;
using compute::UnaryOp;

std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

// map + elementwise on the CPU device: out = a + b, then square it, checked against
// the host computation.
TestResult run_elementwise(const TestOptions& options, TestContext& context) {
    const std::size_t count =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("count", 1024)));
    context.emit("uploading two vectors of " + std::to_string(count) + " floats");

    CpuDevice device;
    std::vector<float> a(count), b(count);
    for (std::size_t i = 0; i < count; ++i) {
        a[i] = static_cast<float>(i) * 0.5f;
        b[i] = static_cast<float>(count - i);
    }
    DeviceBuffer buffer_a = device.upload<float>(a);
    DeviceBuffer buffer_b = device.upload<float>(b);
    DeviceBuffer sum = device.allocate(DataType::Float32, count);
    if (!device.elementwise(BinaryOp::Add, buffer_a, buffer_b, sum).ok()) {
        return TestResult::fail("elementwise add returned a non-ok status");
    }
    DeviceBuffer squared = device.allocate(DataType::Float32, count);
    if (!device.map(UnaryOp::Square, sum, squared).ok()) {
        return TestResult::fail("map square returned a non-ok status");
    }

    std::vector<float> got(count);
    device.download<float>(squared, got);
    double max_error = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double expected = static_cast<double>(a[i] + b[i]) * (a[i] + b[i]);
        max_error = std::max(max_error, std::abs(static_cast<double>(got[i]) - expected));
    }
    if (max_error > 1e-2) {
        return TestResult::fail("device result diverged from host (max error " +
                                number(max_error) + ")");
    }
    TestResult result = TestResult::pass("(a + b)^2 matched the host on " +
                                         std::to_string(count) + " elements");
    result.metric("elements", std::to_string(count))
        .metric("max |error|", number(max_error));
    return result;
}

// reduce + sort_by_key on the CPU device: sum a vector (checked exactly), then sort
// (key, value) pairs ascending and confirm the keys come back ordered with their
// values carried along.
TestResult run_reduce_sort(const TestOptions& options, TestContext& context) {
    const std::size_t count =
        static_cast<std::size_t>(std::max<long>(2, options.get_int("count", 1024)));
    CpuDevice device;

    std::vector<double> values(count);
    double host_sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<double>(i % 17) - 4.0;
        host_sum += values[i];
    }
    context.emit("reducing " + std::to_string(count) + " doubles");
    DeviceBuffer input = device.upload<double>(values);
    DeviceBuffer total = device.allocate(DataType::Float64, 1);
    if (!device.reduce(ReduceOp::Sum, input, total).ok()) {
        return TestResult::fail("reduce sum returned a non-ok status");
    }
    std::vector<double> reduced(1);
    device.download<double>(total, reduced);
    if (std::abs(reduced[0] - host_sum) > 1e-6) {
        return TestResult::fail("reduce sum " + number(reduced[0]) + " != host " +
                                number(host_sum));
    }

    // sort_by_key: descending keys + their original index as the value.
    context.emit("sorting " + std::to_string(count) + " (key, value) pairs");
    std::vector<std::int64_t> keys(count), payload(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys[i] = static_cast<std::int64_t>(count - i);
        payload[i] = static_cast<std::int64_t>(i);
    }
    DeviceBuffer key_buffer = device.upload<std::int64_t>(keys);
    DeviceBuffer value_buffer = device.upload<std::int64_t>(payload);
    if (!device.sort_by_key(key_buffer, value_buffer, SortOrder::Ascending).ok()) {
        return TestResult::fail("sort_by_key returned a non-ok status");
    }
    std::vector<std::int64_t> sorted_keys(count), sorted_values(count);
    device.download<std::int64_t>(key_buffer, sorted_keys);
    device.download<std::int64_t>(value_buffer, sorted_values);
    for (std::size_t i = 1; i < count; ++i) {
        if (sorted_keys[i - 1] > sorted_keys[i]) {
            return TestResult::fail("keys not ascending after sort_by_key");
        }
    }
    // The value carried with key k (== count - original_index) must be count - k.
    for (std::size_t i = 0; i < count; ++i) {
        if (sorted_values[i] != static_cast<std::int64_t>(count) - sorted_keys[i]) {
            return TestResult::fail("sort_by_key did not carry values with keys");
        }
    }
    TestResult result = TestResult::pass("reduce + sort_by_key matched the host");
    result.metric("elements", std::to_string(count))
        .metric("reduce sum", number(reduced[0]));
    return result;
}

// Report the compute backends this build carries and whether the optional CUDA
// backend is compiled in. The CPU↔CUDA conformance suite runs in the gtest build;
// here we surface the build state (Skipped on a CPU-only build).
TestResult run_cuda_conformance(const TestOptions&, TestContext&) {
#ifdef ZETTA_WITH_CUDA
    TestResult result = TestResult::pass("CUDA backend is compiled in");
    result.metric("cuda", "available");
    result.note(
        "the CPU<->CUDA operator parity suite runs as tests/test_cuda_device.cpp");
    return result;
#else
    TestResult result = TestResult::skip(
        "CPU-only build: no CUDA backend (configure with -DZETTA_ENABLE_CUDA=ON on "
        "a CUDA host)");
    result.metric("cuda", "not built");
    return result;
#endif
}

}  // namespace

void register_compute_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "compute.cpu.elementwise",
        .category = TestCategory::Compute,
        .name = "CPU device: map + elementwise",
        .description = "Run elementwise add and a unary square on the CpuDevice "
                       "reference backend and check the result against the host.",
        .options = {TestOption::integer("count", "Element count", 1024, 1, 2000000,
                                        "how many floats to process")},
        .run = run_elementwise,
    });
    suite.add(TestCase{
        .id = "compute.cpu.reduce_sort",
        .category = TestCategory::Compute,
        .name = "CPU device: reduce + sort_by_key",
        .description = "Reduce a vector (checked exactly) and sort (key, value) "
                       "pairs on the device, confirming the values follow the keys.",
        .options = {TestOption::integer("count", "Element count", 1024, 2, 2000000,
                                        "how many elements to reduce / sort")},
        .run = run_reduce_sort,
    });
    suite.add(TestCase{
        .id = "compute.cuda.conformance",
        .category = TestCategory::Compute,
        .name = "CUDA backend: build state",
        .description = "Report whether the optional CUDA backend is compiled in "
                       "(the CPU<->CUDA operator parity suite runs in the gtest "
                       "build). Skipped on a CPU-only build.",
        .options = {},
        .run = run_cuda_conformance,
    });
}

}  // namespace zetta::testkit
