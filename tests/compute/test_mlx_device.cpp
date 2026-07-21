#include <gtest/gtest.h>

// The MLX backend conformance test. The whole body compiles only under
// ZETTA_WITH_MLX (the define CMake sets for a -DZETTA_ENABLE_MLX=ON build), so a
// build without MLX sees an empty translation unit. Run it on macOS with libmlx to
// exercise the Metal GPU: it round-trips the unified-memory model and checks gemm
// (and axpy) against a hand-computed reference.
#if defined(ZETTA_WITH_MLX)

#include <array>
#include <cstddef>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/device_registry.hpp"
#include "zetta/compute/mlx_device.hpp"

namespace {

using zetta::compute::DataType;
using zetta::compute::DeviceBuffer;
using zetta::compute::DeviceKind;
using zetta::compute::DeviceRegistry;
using zetta::compute::DeviceRequest;
using zetta::compute::GemmParams;
using zetta::compute::MlxDevice;

TEST(MlxDevice, ReportsMlxKindWithUnifiedMemory) {
    MlxDevice device;
    const auto info = device.info();
    EXPECT_EQ(info.kind, DeviceKind::Mlx);
    EXPECT_TRUE(info.unified_memory);
}

TEST(MlxDevice, HostRoundTripThroughUnifiedMemory) {
    MlxDevice device;
    const std::vector<float> host = {1.5F, -2.0F, 3.25F, 4.0F};
    DeviceBuffer buffer = device.upload<float>(host);
    ASSERT_FALSE(buffer.empty());

    std::vector<float> back(host.size(), 0.0F);
    ASSERT_TRUE(device.download<float>(buffer, back).ok());
    EXPECT_EQ(back, host);
}

// 2x3 * 3x2 = 2x2, computed on the GPU and checked against the hand result.
TEST(MlxDevice, GemmMatchesHandComputedResult) {
    MlxDevice device;
    // A = [[1,2,3],[4,5,6]] (row-major), B = [[7,8],[9,10],[11,12]].
    const std::vector<float> a = {1, 2, 3, 4, 5, 6};
    const std::vector<float> b = {7, 8, 9, 10, 11, 12};
    DeviceBuffer da = device.upload<float>(a);
    DeviceBuffer db = device.upload<float>(b);
    DeviceBuffer dc = device.allocate(DataType::Float32, 4);
    ASSERT_FALSE(da.empty());
    ASSERT_FALSE(db.empty());
    ASSERT_FALSE(dc.empty());

    GemmParams params;
    params.rows = 2;
    params.inner = 3;
    params.cols = 2;
    params.alpha = 1.0;
    params.beta = 0.0;
    ASSERT_TRUE(device.gemm(params, da, db, dc).ok());

    std::vector<float> c(4, 0.0F);
    ASSERT_TRUE(device.download<float>(dc, c).ok());
    // [[1*7+2*9+3*11, 1*8+2*10+3*12], [4*7+5*9+6*11, 4*8+5*10+6*12]]
    //  = [[58, 64], [139, 154]]
    EXPECT_FLOAT_EQ(c[0], 58.0F);
    EXPECT_FLOAT_EQ(c[1], 64.0F);
    EXPECT_FLOAT_EQ(c[2], 139.0F);
    EXPECT_FLOAT_EQ(c[3], 154.0F);
}

TEST(MlxDevice, AxpyMatchesHandComputedResult) {
    MlxDevice device;
    const std::vector<float> x = {1.0F, 2.0F, 3.0F};
    const std::vector<float> y = {10.0F, 20.0F, 30.0F};
    DeviceBuffer dx = device.upload<float>(x);
    DeviceBuffer dy = device.upload<float>(y);
    ASSERT_TRUE(device.axpy(2.0, dx, dy).ok());  // y := 2x + y

    std::vector<float> out(3, 0.0F);
    ASSERT_TRUE(device.download<float>(dy, out).ok());
    EXPECT_FLOAT_EQ(out[0], 12.0F);
    EXPECT_FLOAT_EQ(out[1], 24.0F);
    EXPECT_FLOAT_EQ(out[2], 36.0F);
}

// The registry picks the MLX device as the accelerator over the CPU.
TEST(MlxDevice, RegistrySelectsMlxAsAccelerator) {
    DeviceRegistry registry;
    registry.add(std::make_shared<MlxDevice>());
    EXPECT_TRUE(registry.has(DeviceKind::Mlx));
    const auto selected = registry.select(DeviceRequest{});  // prefer accelerator
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->info().kind, DeviceKind::Mlx);
}

}  // namespace

#endif  // ZETTA_WITH_MLX
