#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/host_stub_device.hpp"

namespace {

using zetta::compute::ComputeStatusCode;
using zetta::compute::DataType;
using zetta::compute::DeviceBuffer;
using zetta::compute::DeviceKind;
using zetta::compute::HostStubDevice;

// --- DataType helpers --------------------------------------------------------

TEST(ComputeDataType, ByteSizeAndNames) {
    EXPECT_EQ(zetta::compute::byte_size(DataType::Float32), 4u);
    EXPECT_EQ(zetta::compute::byte_size(DataType::Float64), 8u);
    EXPECT_EQ(zetta::compute::byte_size(DataType::Int32), 4u);
    EXPECT_EQ(zetta::compute::byte_size(DataType::UInt8), 1u);

    EXPECT_EQ(zetta::compute::to_string(DataType::Float32), "float32");
    EXPECT_EQ(zetta::compute::to_string(DataType::Int64), "int64");

    // The C++-type -> tag bridge the typed helpers use.
    EXPECT_EQ(zetta::compute::data_type_of<float>, DataType::Float32);
    EXPECT_EQ(zetta::compute::data_type_of<double>, DataType::Float64);
    EXPECT_EQ(zetta::compute::data_type_of<std::int32_t>, DataType::Int32);
    EXPECT_EQ(zetta::compute::data_type_of<std::uint8_t>, DataType::UInt8);
}

// --- The host<->device round-trip (the acceptance test) ----------------------

TEST(ComputeDevice, TypedHostDeviceRoundTrip) {
    HostStubDevice device;

    const std::vector<float> source = {1.0f, -2.5f, 3.25f, 42.0f, 0.0f};
    DeviceBuffer buffer = device.upload<float>(source);

    ASSERT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.dtype(), DataType::Float32);
    EXPECT_EQ(buffer.element_count(), source.size());
    EXPECT_EQ(buffer.size_bytes(), source.size() * sizeof(float));
    EXPECT_EQ(buffer.device(), &device);

    std::vector<float> destination(source.size(), 0.0f);
    const auto status = device.download<float>(buffer, destination);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(destination, source);
}

TEST(ComputeDevice, IntegerRoundTrip) {
    HostStubDevice device;

    const std::vector<std::int64_t> source = {-5, 0, 7, 1'000'000, 42};
    DeviceBuffer buffer = device.upload<std::int64_t>(source);
    ASSERT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.dtype(), DataType::Int64);

    std::vector<std::int64_t> destination(source.size());
    ASSERT_TRUE(device.download<std::int64_t>(buffer, destination).ok());
    EXPECT_EQ(destination, source);
}

// --- Byte-oriented transfer validation ---------------------------------------

TEST(ComputeDevice, ByteTransferRejectsOversizedCopy) {
    HostStubDevice device;
    DeviceBuffer buffer = device.allocate(DataType::Float32, 4);
    ASSERT_FALSE(buffer.empty());

    const std::vector<float> source(8, 1.0f);  // twice the buffer's capacity
    const auto status = device.copy_from_host(source.data(), buffer,
                                              source.size() * sizeof(float));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, ComputeStatusCode::InvalidArgument);
}

TEST(ComputeDevice, DownloadRejectsDtypeMismatch) {
    HostStubDevice device;
    const std::vector<float> source = {1.0f, 2.0f, 3.0f};
    DeviceBuffer buffer = device.upload<float>(source);
    ASSERT_FALSE(buffer.empty());

    // The buffer holds float32; downloading as int32 must be rejected, not
    // silently reinterpreted.
    std::vector<std::int32_t> wrong(source.size());
    const auto status = device.download<std::int32_t>(buffer, wrong);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, ComputeStatusCode::InvalidArgument);
}

TEST(ComputeDevice, DownloadRejectsSizeMismatch) {
    HostStubDevice device;
    const std::vector<float> source = {1.0f, 2.0f, 3.0f};
    DeviceBuffer buffer = device.upload<float>(source);
    ASSERT_FALSE(buffer.empty());

    std::vector<float> too_small(source.size() - 1);
    const auto status = device.download<float>(buffer, too_small);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code, ComputeStatusCode::InvalidArgument);
}

// --- Buffer ownership (RAII, move-only) --------------------------------------

TEST(DeviceBuffer, DefaultConstructedIsEmpty) {
    DeviceBuffer buffer;
    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(static_cast<bool>(buffer));
    EXPECT_EQ(buffer.element_count(), 0u);
    EXPECT_EQ(buffer.size_bytes(), 0u);
    EXPECT_EQ(buffer.device(), nullptr);
    EXPECT_EQ(buffer.handle(), nullptr);
}

TEST(DeviceBuffer, MoveTransfersOwnership) {
    HostStubDevice device;
    DeviceBuffer source = device.allocate(DataType::Float64, 16);
    ASSERT_FALSE(source.empty());
    void* original_handle = source.handle();

    DeviceBuffer moved = std::move(source);
    EXPECT_TRUE(source.empty());                 // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(source.device(), nullptr);
    EXPECT_FALSE(moved.empty());
    EXPECT_EQ(moved.handle(), original_handle);
    EXPECT_EQ(moved.element_count(), 16u);

    // Move-assign frees the destination's current storage, then takes source's.
    DeviceBuffer other = device.allocate(DataType::Float64, 1);
    other = std::move(moved);
    EXPECT_TRUE(moved.empty());                  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(other.handle(), original_handle);
}

// --- DeviceInfo --------------------------------------------------------------

TEST(ComputeDevice, StubReportsHostInfo) {
    HostStubDevice device;
    const auto info = device.info();
    EXPECT_EQ(info.kind, DeviceKind::Cpu);
    EXPECT_EQ(info.name, "host-stub");
    EXPECT_TRUE(info.unified_memory);
}

// --- Operators are declared but unsupported on the stub ----------------------
// This documents the seam: a transfer-only device reports Unsupported per op so
// the coordinator can route the work elsewhere, rather than crashing.

TEST(ComputeDevice, StubReportsOperatorsUnsupported) {
    HostStubDevice device;
    DeviceBuffer a = device.allocate(DataType::Float32, 4);
    DeviceBuffer b = device.allocate(DataType::Float32, 4);
    DeviceBuffer out = device.allocate(DataType::Float32, 4);

    EXPECT_EQ(device.map(zetta::compute::UnaryOp::Square, a, out).code,
              ComputeStatusCode::Unsupported);
    EXPECT_EQ(device.elementwise(zetta::compute::BinaryOp::Add, a, b, out).code,
              ComputeStatusCode::Unsupported);
    EXPECT_EQ(device.reduce(zetta::compute::ReduceOp::Sum, a, out).code,
              ComputeStatusCode::Unsupported);
    EXPECT_EQ(device.axpy(2.0, a, out).code, ComputeStatusCode::Unsupported);
    EXPECT_EQ(device.sort(a, zetta::compute::SortOrder::Ascending).code,
              ComputeStatusCode::Unsupported);

    // The reason is carried, not a bare code.
    EXPECT_FALSE(device.reduce(zetta::compute::ReduceOp::Sum, a, out).ok());
    EXPECT_NE(device.reduce(zetta::compute::ReduceOp::Sum, a, out).message, "");
}

}  // namespace
