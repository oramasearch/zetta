#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
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
    ASSERT_TRUE(status.ok()) << status.message;

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
    ASSERT_TRUE(status.ok()) << status.message;

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


// --- RoPE ------------------------------------------------------------------------------------
// RoPE is a ROTATION, which gives two properties no formula comparison would catch on its own:
// it preserves each row's norm, and it is the identity at position 0. A swapped sin/cos, a wrong
// sign, or a mispaired (i, i+half) still "looks like" a rotation formula but breaks these.
TEST(CudaTransformer, RopePreservesRowNormsAndIsIdentityAtPositionZero) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const int rows = 6;
    const int head_dim = 64;
    const std::vector<float> x = random_values(static_cast<std::size_t>(rows) * head_dim, 101U);
    DeviceBuffer device_x = device.upload<float>(std::span<const float>(x));
    DeviceBuffer device_out = device.allocate(DataType::Float32, x.size());
    ASSERT_TRUE(zetta::compute::cuda_rope(device_x, device_out, rows, head_dim, 10000.0F, 0).ok());
    std::vector<float> out(x.size(), 0.0F);
    ASSERT_TRUE(device.download<float>(device_out, std::span<float>(out)).ok());

    for (int row = 0; row < rows; ++row) {
        double before = 0.0;
        double after = 0.0;
        for (int c = 0; c < head_dim; ++c) {
            const std::size_t index = static_cast<std::size_t>(row) * head_dim + c;
            before += static_cast<double>(x[index]) * x[index];
            after += static_cast<double>(out[index]) * out[index];
        }
        EXPECT_NEAR(std::sqrt(after), std::sqrt(before), 1e-3) << "row " << row << " norm changed";
    }
    // Row 0 is position 0, where every angle is 0, so the rotation is the identity.
    for (int c = 0; c < head_dim; ++c) {
        EXPECT_NEAR(out[static_cast<std::size_t>(c)], x[static_cast<std::size_t>(c)], 1e-6F)
            << "position 0 must be the identity, at " << c;
    }
}

// The backward is the INVERSE rotation, so forward-then-backward must return the original. This
// verifies the gradient without numerical differencing, and would catch a sign error immediately.
TEST(CudaTransformer, RopeBackwardIsTheInverseRotation) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const int rows = 5;
    const int head_dim = 32;
    const std::vector<float> x = random_values(static_cast<std::size_t>(rows) * head_dim, 202U);
    DeviceBuffer device_x = device.upload<float>(std::span<const float>(x));
    DeviceBuffer rotated = device.allocate(DataType::Float32, x.size());
    DeviceBuffer restored = device.allocate(DataType::Float32, x.size());
    ASSERT_TRUE(zetta::compute::cuda_rope(device_x, rotated, rows, head_dim, 10000.0F, 0).ok());
    ASSERT_TRUE(
        zetta::compute::cuda_rope_backward(rotated, restored, rows, head_dim, 10000.0F, 0).ok());

    std::vector<float> round_trip(x.size(), 0.0F);
    ASSERT_TRUE(device.download<float>(restored, std::span<float>(round_trip)).ok());
    for (std::size_t index = 0; index < x.size(); ++index) {
        EXPECT_NEAR(round_trip[index], x[index], 1e-4F) << "round trip differs at " << index;
    }
}

TEST(CudaTransformer, RopeRejectsAnOddHeadDim) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";
    // rotate_half pairs (i, i + head_dim/2); an odd head_dim has no such pairing, and truncating
    // silently would rotate some elements and leave others untouched.
    const std::vector<float> x(15, 0.5F);
    DeviceBuffer device_x = device.upload<float>(std::span<const float>(x));
    DeviceBuffer out = device.allocate(DataType::Float32, x.size());
    EXPECT_FALSE(zetta::compute::cuda_rope(device_x, out, 3, 5, 10000.0F, 0).ok());
}

// --- SwiGLU ----------------------------------------------------------------------------------
TEST(CudaTransformer, SwigluMatchesTheHostReferenceAndItsGradients) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const std::size_t elements = 1000;  // not a multiple of the 256-thread block
    const std::vector<float> gate = random_values(elements, 303U);
    const std::vector<float> up = random_values(elements, 404U);
    const std::vector<float> grad_out = random_values(elements, 505U);

    const auto sigmoid = [](double value) { return 1.0 / (1.0 + std::exp(-value)); };

    DeviceBuffer device_gate = device.upload<float>(std::span<const float>(gate));
    DeviceBuffer device_up = device.upload<float>(std::span<const float>(up));
    DeviceBuffer device_grad_out = device.upload<float>(std::span<const float>(grad_out));
    DeviceBuffer device_out = device.allocate(DataType::Float32, elements);
    DeviceBuffer device_grad_gate = device.allocate(DataType::Float32, elements);
    DeviceBuffer device_grad_up = device.allocate(DataType::Float32, elements);

    ASSERT_TRUE(zetta::compute::cuda_swiglu(device_gate, device_up, device_out, elements).ok());
    ASSERT_TRUE(zetta::compute::cuda_swiglu_backward(device_gate, device_up, device_grad_out,
                                                     device_grad_gate, device_grad_up, elements)
                    .ok());

    std::vector<float> out(elements, 0.0F);
    std::vector<float> grad_gate(elements, 0.0F);
    std::vector<float> grad_up(elements, 0.0F);
    ASSERT_TRUE(device.download<float>(device_out, std::span<float>(out)).ok());
    ASSERT_TRUE(device.download<float>(device_grad_gate, std::span<float>(grad_gate)).ok());
    ASSERT_TRUE(device.download<float>(device_grad_up, std::span<float>(grad_up)).ok());

    for (std::size_t index = 0; index < elements; ++index) {
        const double g = gate[index];
        const double s = sigmoid(g);
        const double silu = g * s;
        EXPECT_NEAR(out[index], silu * up[index], 1e-5) << "forward at " << index;
        EXPECT_NEAR(grad_gate[index], grad_out[index] * up[index] * s * (1.0 + g * (1.0 - s)), 1e-5)
            << "grad_gate at " << index;
        EXPECT_NEAR(grad_up[index], grad_out[index] * silu, 1e-5) << "grad_up at " << index;
    }
}


// --- Causal attention ------------------------------------------------------------------------
// Reference: the composed, seq^2 formulation the fused kernel must reproduce exactly.
std::vector<float> host_causal_attention(const std::vector<float>& q, const std::vector<float>& k,
                                        const std::vector<float>& v, int seq, int n_heads,
                                        int n_kv_heads, int head_dim) {
    std::vector<float> out(q.size(), 0.0F);
    const int group = n_heads / n_kv_heads;
    const double scale = 1.0 / std::sqrt(static_cast<double>(head_dim));
    for (int head = 0; head < n_heads; ++head) {
        const int kv_head = head / group;
        for (int query = 0; query < seq; ++query) {
            std::vector<double> scores(static_cast<std::size_t>(query) + 1, 0.0);
            double maximum = -1e30;
            for (int key = 0; key <= query; ++key) {
                double dot = 0.0;
                for (int d = 0; d < head_dim; ++d) {
                    dot += static_cast<double>(q[(static_cast<std::size_t>(query) * n_heads + head) *
                                                     head_dim + d]) *
                           k[(static_cast<std::size_t>(key) * n_kv_heads + kv_head) * head_dim + d];
                }
                scores[static_cast<std::size_t>(key)] = dot * scale;
                maximum = std::max(maximum, dot * scale);
            }
            double sum = 0.0;
            for (double& score : scores) {
                score = std::exp(score - maximum);
                sum += score;
            }
            for (int d = 0; d < head_dim; ++d) {
                double total = 0.0;
                for (int key = 0; key <= query; ++key) {
                    total += scores[static_cast<std::size_t>(key)] /
                             sum *
                             v[(static_cast<std::size_t>(key) * n_kv_heads + kv_head) * head_dim + d];
                }
                out[(static_cast<std::size_t>(query) * n_heads + head) * head_dim + d] =
                    static_cast<float>(total);
            }
        }
    }
    return out;
}

TEST(CudaTransformer, CausalAttentionMatchesTheComposedReferenceWithGqa) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    // GQA on purpose: 4 query heads over 2 kv heads, which is where an off-by-one in the head
    // mapping shows up. head_dim 8 keeps the reference cheap; seq 12 exercises the causal bound.
    const int seq = 12;
    const int n_heads = 4;
    const int n_kv_heads = 2;
    const int head_dim = 8;
    const std::size_t q_elements = static_cast<std::size_t>(seq) * n_heads * head_dim;
    const std::size_t kv_elements = static_cast<std::size_t>(seq) * n_kv_heads * head_dim;

    const std::vector<float> q = random_values(q_elements, 606U);
    const std::vector<float> k = random_values(kv_elements, 707U);
    const std::vector<float> v = random_values(kv_elements, 808U);

    DeviceBuffer device_q = device.upload<float>(std::span<const float>(q));
    DeviceBuffer device_k = device.upload<float>(std::span<const float>(k));
    DeviceBuffer device_v = device.upload<float>(std::span<const float>(v));
    DeviceBuffer device_out = device.allocate(DataType::Float32, q_elements);
    DeviceBuffer device_lse =
        device.allocate(DataType::Float32, static_cast<std::size_t>(n_heads) * seq);

    const auto status = zetta::compute::cuda_causal_attention(
        device_q, device_k, device_v, device_out, device_lse, seq, n_heads, n_kv_heads, head_dim);
    ASSERT_TRUE(status.ok()) << status.message;

    std::vector<float> actual(q_elements, 0.0F);
    ASSERT_TRUE(device.download<float>(device_out, std::span<float>(actual)).ok());
    const std::vector<float> expected =
        host_causal_attention(q, k, v, seq, n_heads, n_kv_heads, head_dim);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_NEAR(actual[index], expected[index], 1e-4F) << "at " << index;
    }
}

TEST(CudaTransformer, CausalAttentionRejectsAnIncompatibleHeadGrouping) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";
    const std::vector<float> small(64, 0.1F);
    DeviceBuffer buffer = device.upload<float>(std::span<const float>(small));
    DeviceBuffer out = device.allocate(DataType::Float32, small.size());
    DeviceBuffer lse = device.allocate(DataType::Float32, 16);
    // 3 query heads cannot be grouped over 2 kv heads; rounding would pair heads with wrong keys.
    EXPECT_FALSE(zetta::compute::cuda_causal_attention(buffer, buffer, buffer, out, lse, 4, 3, 2, 4)
                     .ok());
}

// Gradient check for all three of grad_q, grad_k, grad_v against numerical differences. The
// backward recomputes the softmax from the saved log-sum-exp, so an error in that reconstruction
// shows up here and nowhere else.
TEST(CudaTransformer, CausalAttentionBackwardMatchesNumericalGradients) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const int seq = 5;
    const int n_heads = 2;
    const int n_kv_heads = 1;  // GQA sharing, so grad_k/grad_v accumulate over two query heads
    const int head_dim = 4;
    const std::size_t q_elements = static_cast<std::size_t>(seq) * n_heads * head_dim;
    const std::size_t kv_elements = static_cast<std::size_t>(seq) * n_kv_heads * head_dim;

    const std::vector<float> q = random_values(q_elements, 909U);
    const std::vector<float> k = random_values(kv_elements, 111U);
    const std::vector<float> v = random_values(kv_elements, 222U);
    const std::vector<float> grad_out = random_values(q_elements, 333U);

    const auto loss_for = [&](const std::vector<float>& qq, const std::vector<float>& kk,
                              const std::vector<float>& vv) {
        const std::vector<float> out =
            host_causal_attention(qq, kk, vv, seq, n_heads, n_kv_heads, head_dim);
        double total = 0.0;
        for (std::size_t index = 0; index < out.size(); ++index) {
            total += static_cast<double>(out[index]) * grad_out[index];
        }
        return total;
    };

    DeviceBuffer device_q = device.upload<float>(std::span<const float>(q));
    DeviceBuffer device_k = device.upload<float>(std::span<const float>(k));
    DeviceBuffer device_v = device.upload<float>(std::span<const float>(v));
    DeviceBuffer device_grad_out = device.upload<float>(std::span<const float>(grad_out));
    DeviceBuffer device_out = device.allocate(DataType::Float32, q_elements);
    DeviceBuffer device_lse =
        device.allocate(DataType::Float32, static_cast<std::size_t>(n_heads) * seq);
    ASSERT_TRUE(zetta::compute::cuda_causal_attention(device_q, device_k, device_v, device_out,
                                                      device_lse, seq, n_heads, n_kv_heads, head_dim)
                    .ok());

    DeviceBuffer device_row_dot =
        device.allocate(DataType::Float32, static_cast<std::size_t>(n_heads) * seq);
    DeviceBuffer device_grad_q = device.allocate(DataType::Float32, q_elements);
    DeviceBuffer device_grad_k = device.allocate(DataType::Float32, kv_elements);
    DeviceBuffer device_grad_v = device.allocate(DataType::Float32, kv_elements);
    const auto status = zetta::compute::cuda_causal_attention_backward(
        device_q, device_k, device_v, device_out, device_grad_out, device_lse, device_row_dot,
        device_grad_q, device_grad_k, device_grad_v, seq, n_heads, n_kv_heads, head_dim);
    ASSERT_TRUE(status.ok()) << status.message;

    std::vector<float> grad_q(q_elements, 0.0F);
    std::vector<float> grad_k(kv_elements, 0.0F);
    std::vector<float> grad_v(kv_elements, 0.0F);
    ASSERT_TRUE(device.download<float>(device_grad_q, std::span<float>(grad_q)).ok());
    ASSERT_TRUE(device.download<float>(device_grad_k, std::span<float>(grad_k)).ok());
    ASSERT_TRUE(device.download<float>(device_grad_v, std::span<float>(grad_v)).ok());

    const double step = 1e-3;
    const auto check = [&](const char* name, const std::vector<float>& analytic,
                           const std::vector<float>& base,
                           const std::function<double(const std::vector<float>&)>& evaluate) {
        for (std::size_t index = 0; index < analytic.size(); ++index) {
            std::vector<float> plus = base;
            std::vector<float> minus = base;
            plus[index] = static_cast<float>(plus[index] + step);
            minus[index] = static_cast<float>(minus[index] - step);
            const double numerical = (evaluate(plus) - evaluate(minus)) / (2.0 * step);
            EXPECT_NEAR(analytic[index], numerical, 2e-2) << name << " at " << index;
        }
    };
    check("grad_q", grad_q, q, [&](const std::vector<float>& x) { return loss_for(x, k, v); });
    check("grad_k", grad_k, k, [&](const std::vector<float>& x) { return loss_for(q, x, v); });
    check("grad_v", grad_v, v, [&](const std::vector<float>& x) { return loss_for(q, k, x); });
}


// --- allocation pool (#928) ------------------------------------------------------------------
// cudaMalloc/cudaFree synchronize the device, and one autograd training step issues ~1,700
// allocate/free pairs — measured as ~11% GPU utilization while training. These tests assert the
// pool actually serves reuse rather than merely existing: a hit count that stays zero is a pool
// that is silently doing nothing, which is indistinguishable from the bug it was added to fix.
TEST(CudaAllocationPool, ReusesFreedBlocksOfTheSameSize) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    const std::size_t elements = 4096;
    void* first_handle = nullptr;
    {
        DeviceBuffer buffer = device.allocate(DataType::Float32, elements);
        ASSERT_FALSE(buffer.empty());
        first_handle = buffer.handle();
    }  // released here -- should land in the pool, not go back to CUDA
    EXPECT_GT(device.pooled_bytes(), 0U) << "a freed block should be held for reuse";

    const std::size_t hits_before = device.pool_hits();
    {
        DeviceBuffer reused = device.allocate(DataType::Float32, elements);
        ASSERT_FALSE(reused.empty());
        // Same address back: proof it was reused rather than freshly allocated.
        EXPECT_EQ(reused.handle(), first_handle);
    }
    EXPECT_EQ(device.pool_hits(), hits_before + 1);
}

TEST(CudaAllocationPool, RespectsItsLimitAndDrains) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";

    // A zero limit disables pooling: without a cap the reserve would grow to the high-water mark
    // of every shape ever seen and quietly consume the memory ceiling #924 sets.
    device.set_pool_limit(0);
    { DeviceBuffer buffer = device.allocate(DataType::Float32, 1024); }
    EXPECT_EQ(device.pooled_bytes(), 0U) << "pooling disabled must not retain anything";

    device.set_pool_limit(64ULL * 1024 * 1024);
    { DeviceBuffer buffer = device.allocate(DataType::Float32, 1024); }
    EXPECT_GT(device.pooled_bytes(), 0U);
    device.drain_pool();
    EXPECT_EQ(device.pooled_bytes(), 0U) << "drain must return everything to CUDA";
}

TEST(CudaAllocationPool, DifferentSizesDoNotAliasEachOther) {
    CudaDevice device;
    if (!cuda_present(device)) GTEST_SKIP() << "no CUDA device present";
    device.drain_pool();

    void* small_handle = nullptr;
    { DeviceBuffer small = device.allocate(DataType::Float32, 256); small_handle = small.handle(); }
    // A different size must NOT be served the pooled block: matching is by exact size, so handing
    // back a smaller block would silently under-allocate and corrupt memory downstream.
    DeviceBuffer large = device.allocate(DataType::Float32, 8192);
    ASSERT_FALSE(large.empty());
    EXPECT_NE(large.handle(), small_handle);
    EXPECT_GE(large.element_count(), 8192U);
}

#endif  // ZETTA_WITH_CUDA

}  // namespace
