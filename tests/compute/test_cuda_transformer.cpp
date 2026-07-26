#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <random>
#include <span>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/data_type.hpp"

#if defined(ZETTA_WITH_CUDA)
#include "zetta/compute/cuda_device.hpp"
#include "zetta/compute/cuda_transformer.hpp"
#endif

// Fused transformer kernels (#922). Every kernel is checked two ways, because the two catch
// different mistakes:
//
//   * against a straightforward HOST implementation of the same formula — catches a wrong formula;
//   * by GRADIENT CHECK against numerical differences — catches a backward pass that is internally
//     consistent but does not actually differentiate the forward.
//
// A kernel that only passes the first is a kernel whose gradient can be silently wrong, which in
// training shows up as "it learns, just worse" — the hardest class of bug to attribute.
namespace {

#if defined(ZETTA_WITH_CUDA)

using zetta::compute::CudaDevice;
using zetta::compute::DataType;
using zetta::compute::DeviceBuffer;

constexpr float kEpsilon = 1e-6F;

bool cuda_present(CudaDevice& device) {
    const auto info = device.info();
    return !(info.name.empty() || info.name == "cuda");
}

// The reference: RMSNorm exactly as the header documents it, in plain host code.
std::vector<float> host_rms_norm(const std::vector<float>& x, const std::vector<float>& weight,
                                int rows, int cols) {
    std::vector<float> out(x.size(), 0.0F);
    for (int row = 0; row < rows; ++row) {
        double sum_squares = 0.0;
        for (int c = 0; c < cols; ++c) {
            const double value = x[static_cast<std::size_t>(row) * cols + c];
            sum_squares += value * value;
        }
        const double scale = 1.0 / std::sqrt(sum_squares / cols + kEpsilon);
        for (int c = 0; c < cols; ++c) {
            const std::size_t index = static_cast<std::size_t>(row) * cols + c;
            out[index] = static_cast<float>(x[index] * scale * weight[c]);
        }
    }
    return out;
}

std::vector<float> random_values(std::size_t count, std::uint32_t seed) {
    // A fixed engine with an explicitly-specified reduction, NOT uniform_real_distribution: that
    // distribution is unspecified across standard libraries, so using it here would make this test
    // produce different inputs on macOS and Linux (see #927).
    std::mt19937 rng(seed);
    std::vector<float> values(count);
    for (float& value : values) {
        value = static_cast<float>(rng() % 2000) / 1000.0F - 1.0F;  // [-1, 1)
    }
    return values;
}

TEST(CudaTransformer, RmsNormMatchesTheHostReference) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    // Cols deliberately NOT a multiple of the 256-thread block, so the strided loops and the block
    // reduction are exercised on a ragged row rather than only on the easy aligned case.
    const int rows = 7;
    const int cols = 300;
    const std::vector<float> x = random_values(static_cast<std::size_t>(rows) * cols, 11U);
    const std::vector<float> weight = random_values(static_cast<std::size_t>(cols), 22U);

    DeviceBuffer device_x = device.upload<float>(std::span<const float>(x));
    DeviceBuffer device_weight = device.upload<float>(std::span<const float>(weight));
    DeviceBuffer device_out = device.allocate(DataType::Float32, x.size());
    ASSERT_FALSE(device_x.empty() || device_weight.empty() || device_out.empty());

    const auto status =
        zetta::compute::cuda_rms_norm(device_x, device_weight, device_out, rows, cols, kEpsilon);
    ASSERT_TRUE(status.ok()) << status.reason();

    std::vector<float> actual(x.size(), 0.0F);
    ASSERT_TRUE(device.download<float>(device_out, std::span<float>(actual)).ok());

    const std::vector<float> expected = host_rms_norm(x, weight, rows, cols);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_NEAR(actual[index], expected[index], 1e-5F) << "at " << index;
    }
}

TEST(CudaTransformer, RmsNormRejectsMismatchedShapes) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";
    // A too-small buffer must be refused at the boundary. Launched anyway it reads out of bounds,
    // which surfaces much later as corrupt numbers instead of an error.
    const std::vector<float> small(4, 1.0F);
    DeviceBuffer device_x = device.upload<float>(std::span<const float>(small));
    DeviceBuffer device_weight = device.upload<float>(std::span<const float>(small));
    DeviceBuffer device_out = device.allocate(DataType::Float32, small.size());
    EXPECT_FALSE(
        zetta::compute::cuda_rms_norm(device_x, device_weight, device_out, 8, 8, kEpsilon).ok());
    EXPECT_FALSE(
        zetta::compute::cuda_rms_norm(device_x, device_weight, device_out, 0, 4, kEpsilon).ok());
}

// The gradient check. Perturb one input at a time and compare the analytic gradient to the
// numerical one — the only test that can tell a correct backward from a plausible one.
TEST(CudaTransformer, RmsNormBackwardMatchesNumericalGradients) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const int rows = 3;
    const int cols = 5;
    const std::size_t elements = static_cast<std::size_t>(rows) * cols;
    std::vector<float> x = random_values(elements, 33U);
    const std::vector<float> weight = random_values(static_cast<std::size_t>(cols), 44U);
    // Upstream gradient: use a non-uniform one, since an all-ones grad_out can hide a term that is
    // wrong only where the upstream gradient varies.
    const std::vector<float> grad_out = random_values(elements, 55U);

    // Scalar objective L = sum(rms_norm(x, w) * grad_out), whose dL/dx is what backward returns.
    const auto loss_for = [&](const std::vector<float>& inputs,
                              const std::vector<float>& weights) {
        const std::vector<float> out = host_rms_norm(inputs, weights, rows, cols);
        double total = 0.0;
        for (std::size_t index = 0; index < out.size(); ++index) {
            total += static_cast<double>(out[index]) * grad_out[index];
        }
        return total;
    };

    DeviceBuffer device_x = device.upload<float>(std::span<const float>(x));
    DeviceBuffer device_weight = device.upload<float>(std::span<const float>(weight));
    DeviceBuffer device_grad_out = device.upload<float>(std::span<const float>(grad_out));
    DeviceBuffer device_grad_x = device.allocate(DataType::Float32, elements);
    // grad_weight ACCUMULATES, so it must start zeroed: uploading a zero vector allocates and
    // fills in one step.
    const std::vector<float> zeros(static_cast<std::size_t>(cols), 0.0F);
    DeviceBuffer device_grad_weight = device.upload<float>(std::span<const float>(zeros));
    ASSERT_FALSE(device_grad_x.empty() || device_grad_weight.empty());

    const auto status = zetta::compute::cuda_rms_norm_backward(
        device_x, device_weight, device_grad_out, device_grad_x, device_grad_weight, rows, cols,
        kEpsilon);
    ASSERT_TRUE(status.ok()) << status.reason();

    std::vector<float> grad_x(elements, 0.0F);
    std::vector<float> grad_weight(static_cast<std::size_t>(cols), 0.0F);
    ASSERT_TRUE(device.download<float>(device_grad_x, std::span<float>(grad_x)).ok());
    ASSERT_TRUE(device.download<float>(device_grad_weight, std::span<float>(grad_weight)).ok());

    const double step = 1e-3;
    for (std::size_t index = 0; index < elements; ++index) {
        std::vector<float> plus = x;
        std::vector<float> minus = x;
        plus[index] = static_cast<float>(plus[index] + step);
        minus[index] = static_cast<float>(minus[index] - step);
        const double numerical =
            (loss_for(plus, weight) - loss_for(minus, weight)) / (2.0 * step);
        EXPECT_NEAR(grad_x[index], numerical, 2e-2) << "grad_x at " << index;
    }
    for (int col = 0; col < cols; ++col) {
        std::vector<float> plus = weight;
        std::vector<float> minus = weight;
        plus[static_cast<std::size_t>(col)] += static_cast<float>(step);
        minus[static_cast<std::size_t>(col)] -= static_cast<float>(step);
        const double numerical = (loss_for(x, plus) - loss_for(x, minus)) / (2.0 * step);
        EXPECT_NEAR(grad_weight[static_cast<std::size_t>(col)], numerical, 2e-2)
            << "grad_weight at " << col;
    }
}

// Determinism is a stated requirement, not a nicety: #926 asserts CUDA reproduces the CPU reference
// exactly for greedy decode, which is only meaningful if the kernels themselves are reproducible.
// This is what rules out an atomicAdd reduction for grad_weight.
TEST(CudaTransformer, RmsNormBackwardIsBitwiseReproducible) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const int rows = 64;  // enough rows that a nondeterministic row reduction would show up
    const int cols = 128;
    const std::size_t elements = static_cast<std::size_t>(rows) * cols;
    const std::vector<float> x = random_values(elements, 66U);
    const std::vector<float> weight = random_values(static_cast<std::size_t>(cols), 77U);
    const std::vector<float> grad_out = random_values(elements, 88U);
    const std::vector<float> zeros(static_cast<std::size_t>(cols), 0.0F);

    const auto run_once = [&]() {
        DeviceBuffer device_x = device.upload<float>(std::span<const float>(x));
        DeviceBuffer device_weight = device.upload<float>(std::span<const float>(weight));
        DeviceBuffer device_grad_out = device.upload<float>(std::span<const float>(grad_out));
        DeviceBuffer device_grad_x = device.allocate(DataType::Float32, elements);
        DeviceBuffer device_grad_weight = device.upload<float>(std::span<const float>(zeros));
        EXPECT_TRUE(zetta::compute::cuda_rms_norm_backward(device_x, device_weight, device_grad_out,
                                                           device_grad_x, device_grad_weight, rows,
                                                           cols, kEpsilon)
                        .ok());
        std::vector<float> out(static_cast<std::size_t>(cols), 0.0F);
        EXPECT_TRUE(device.download<float>(device_grad_weight, std::span<float>(out)).ok());
        return out;
    };

    const std::vector<float> first = run_once();
    const std::vector<float> second = run_once();
    for (std::size_t index = 0; index < first.size(); ++index) {
        // Bitwise equal, not approximately: any difference here means the reduction order varies.
        EXPECT_EQ(first[index], second[index]) << "grad_weight diverged at " << index;
    }
}

#endif  // ZETTA_WITH_CUDA

}  // namespace
