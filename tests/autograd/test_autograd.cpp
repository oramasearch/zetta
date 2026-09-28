#include <gtest/gtest.h>

#include <cmath>  // std::log -- libc++ provides it transitively, libstdc++ does not
#include <cstddef>
#include <functional>
#include <vector>

#include "zetta/autograd/tensor.hpp"
#include "zetta/compute/cpu_device.hpp"

namespace {

using zetta::autograd::add;
using zetta::autograd::add_bias;
using zetta::autograd::col_slice;
using zetta::autograd::concat_cols;
using zetta::autograd::cross_entropy;
using zetta::autograd::embedding;
using zetta::autograd::gelu;
using zetta::autograd::layer_norm;
using zetta::autograd::matmul;
using zetta::autograd::matmul_transposed_b;
using zetta::autograd::mul;
using zetta::autograd::patch_embedding;
using zetta::autograd::policy_gradient_loss;
using zetta::autograd::rms_norm;
using zetta::autograd::sequence_policy_gradient_loss;
using zetta::autograd::rope;
using zetta::autograd::row_softmax;
using zetta::autograd::scale;
using zetta::autograd::set_compute_device;
using zetta::autograd::silu;
using zetta::autograd::sum;
using zetta::autograd::Tensor;
using zetta::autograd::transpose;
using zetta::compute::CpuDevice;

// Check the analytic gradient (from backward() on the parameter) against a
// finite-difference estimate. `build(param)` constructs the scalar loss from a
// parameter leaf; it is called on the real leaf and on perturbed copies.
void check_gradient(const std::vector<float>& param,
                    const std::vector<int>& shape,
                    const std::function<Tensor(const Tensor&)>& build,
                    float tolerance = 2e-2F) {
    Tensor parameter = Tensor::leaf(param, shape, true);
    Tensor loss = build(parameter);
    loss.backward();
    const std::vector<float> analytic = parameter.grad();
    ASSERT_EQ(analytic.size(), param.size());

    const float epsilon = 1e-2F;
    for (std::size_t i = 0; i < param.size(); ++i) {
        std::vector<float> plus = param;
        std::vector<float> minus = param;
        plus[i] += epsilon;
        minus[i] -= epsilon;
        const float loss_plus = build(Tensor::leaf(plus, shape, false)).data()[0];
        const float loss_minus =
            build(Tensor::leaf(minus, shape, false)).data()[0];
        EXPECT_NEAR(analytic[i], (loss_plus - loss_minus) / (2 * epsilon),
                    tolerance)
            << "parameter " << i;
    }
}

// Forward-only loss = sum(X * W), for finite-difference gradient checking.
float forward_loss(const std::vector<float>& x_data,
                   const std::vector<float>& w_data) {
    const Tensor x = Tensor::leaf(x_data, {2, 3}, false);
    const Tensor w = Tensor::leaf(w_data, {3, 2}, false);
    return sum(matmul(x, w)).data()[0];
}

TEST(Autograd, MatmulForwardIsCorrect) {
    const Tensor x = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    const Tensor w = Tensor::leaf({1, 0, 0, 1, 1, 1}, {3, 2}, false);
    const Tensor y = matmul(x, w);
    ASSERT_EQ(y.shape(), (std::vector<int>{2, 2}));
    // [[1+0+3, 0+2+3], [4+0+6, 0+5+6]] = [[4,5],[10,11]]
    EXPECT_EQ(y.data(), (std::vector<float>{4, 5, 10, 11}));
}

// The gold standard: analytic gradients (from backward) match finite differences.
TEST(Autograd, MatmulGradientMatchesFiniteDifference) {
    const std::vector<float> x_data = {1, 2, 3, 4, 5, 6};
    const std::vector<float> w_data = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F};

    const Tensor x = Tensor::leaf(x_data, {2, 3}, false);
    Tensor w = Tensor::leaf(w_data, {3, 2}, true);
    Tensor loss = sum(matmul(x, w));
    loss.backward();
    const std::vector<float> analytic = w.grad();
    ASSERT_EQ(analytic.size(), w_data.size());

    const float epsilon = 1e-2F;
    for (std::size_t i = 0; i < w_data.size(); ++i) {
        std::vector<float> plus = w_data;
        std::vector<float> minus = w_data;
        plus[i] += epsilon;
        minus[i] -= epsilon;
        const float numeric =
            (forward_loss(x_data, plus) - forward_loss(x_data, minus)) /
            (2 * epsilon);
        EXPECT_NEAR(analytic[i], numeric, 1e-2F) << "parameter " << i;
    }
}

TEST(Autograd, PatchEmbeddingForwardAndGradientsAreCorrect) {
    const std::vector<float> image_values = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
    };
    const std::vector<float> weights = {
        1, 0, 0, 1,
        0, 1, 1, 0,
    };
    const std::vector<float> biases = {0.5F, -0.5F};
    const Tensor image = Tensor::leaf(image_values, {4, 4, 1}, false);
    const Tensor weight = Tensor::leaf(weights, {2, 4}, false);
    const Tensor bias = Tensor::leaf(biases, {2}, false);
    const Tensor patches = patch_embedding(image, weight, bias, 2);
    EXPECT_EQ(patches.shape(), (std::vector<int>{4, 2}));
    EXPECT_EQ(patches.data(),
              (std::vector<float>{7.5F, 6.5F, 11.5F, 10.5F, 23.5F,
                                  22.5F, 27.5F, 26.5F}));

    check_gradient(weights, {2, 4}, [&](const Tensor& parameter) {
        return sum(patch_embedding(image, parameter, bias, 2));
    });
    check_gradient(biases, {2}, [&](const Tensor& parameter) {
        return sum(patch_embedding(image, weight, parameter, 2));
    });
    check_gradient(image_values, {4, 4, 1}, [&](const Tensor& parameter) {
        return sum(patch_embedding(parameter, weight, bias, 2));
    });
}

// Gradient flows to both operands of a matmul.
TEST(Autograd, GradientReachesBothOperands) {
    Tensor a = Tensor::leaf({1, 2, 3, 4}, {2, 2}, true);
    Tensor b = Tensor::leaf({5, 6, 7, 8}, {2, 2}, true);
    Tensor loss = sum(matmul(a, b));
    loss.backward();
    // dA[i,l] = sum over row l of B; dB[l,j] = sum over col l of A.
    EXPECT_EQ(a.grad(), (std::vector<float>{11, 15, 11, 15}));
    EXPECT_EQ(b.grad(), (std::vector<float>{4, 4, 6, 6}));
}

TEST(Autograd, AddGradient) {
    const Tensor other = Tensor::leaf({0.5F, -1.0F, 2.0F, 0.3F}, {2, 2}, false);
    check_gradient({1, 2, 3, 4}, {2, 2},
                   [&](const Tensor& p) { return sum(add(p, other)); });
}

TEST(Autograd, MulGradient) {
    const Tensor other = Tensor::leaf({0.5F, -1.0F, 2.0F, 0.3F}, {2, 2}, false);
    check_gradient({1, 2, 3, 4}, {2, 2},
                   [&](const Tensor& p) { return sum(mul(p, other)); });
}

TEST(Autograd, ScaleGradient) {
    check_gradient({1, 2, 3, 4}, {2, 2},
                   [](const Tensor& p) { return sum(scale(p, 2.5F)); });
}

TEST(Autograd, AddBiasGradient) {
    const Tensor x = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    check_gradient({0.1F, 0.2F, 0.3F}, {3},
                   [&](const Tensor& bias) { return sum(add_bias(x, bias)); });
}

TEST(Autograd, GeluGradient) {
    check_gradient({-2.0F, -0.5F, 0.0F, 0.5F, 1.5F, 3.0F}, {2, 3},
                   [](const Tensor& p) { return sum(gelu(p)); });
}

TEST(Autograd, SiluGradient) {
    check_gradient({-2.0F, -0.5F, 0.0F, 0.5F, 1.5F, 3.0F}, {2, 3},
                   [](const Tensor& p) { return sum(silu(p)); });
}

TEST(Autograd, RowSoftmaxRowsSumToOne) {
    const Tensor logits = Tensor::leaf({1, 2, 3, -1, 0, 4}, {2, 3}, false);
    const Tensor probs = row_softmax(logits);
    for (int row = 0; row < 2; ++row) {
        float total = 0.0F;
        for (int col = 0; col < 3; ++col) total += probs.data()[row * 3 + col];
        EXPECT_NEAR(total, 1.0F, 1e-6F);
    }
    // The equal-logits row [1,1,...] would be uniform; here larger logits get more.
    EXPECT_GT(probs.data()[2], probs.data()[0]);  // row 0: logit 3 > logit 1
}

TEST(Autograd, RowSoftmaxGradient) {
    // A non-trivial loss (sum of softmax alone is constant): weight the row
    // probabilities by a fixed target so the gradient is non-zero.
    const Tensor target =
        Tensor::leaf({0.2F, 0.5F, 0.3F, 0.1F, 0.7F, 0.2F}, {2, 3}, false);
    check_gradient({0.5F, -1.0F, 2.0F, 1.0F, 0.0F, -0.5F}, {2, 3},
                   [&](const Tensor& p) {
                       return sum(mul(row_softmax(p), target));
                   });
}

TEST(Autograd, RmsNormGivesUnitRmsRows) {
    const Tensor x = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    const Tensor weight = Tensor::leaf({1, 1, 1}, {3}, false);
    const Tensor y = rms_norm(x, weight);
    for (int row = 0; row < 2; ++row) {
        float mean_square = 0.0F;
        for (int col = 0; col < 3; ++col) {
            const float v = y.data()[row * 3 + col];
            mean_square += v * v;
        }
        EXPECT_NEAR(mean_square / 3.0F, 1.0F, 1e-3F);
    }
}

TEST(Autograd, RmsNormGradientWrtInput) {
    const Tensor weight = Tensor::leaf({0.5F, 1.2F, 0.8F}, {3}, false);
    const Tensor target =
        Tensor::leaf({0.2F, 0.5F, 0.3F, 0.1F, 0.7F, 0.2F}, {2, 3}, false);
    check_gradient({1.0F, -2.0F, 3.0F, 0.5F, 1.5F, -1.0F}, {2, 3},
                   [&](const Tensor& x) {
                       return sum(mul(rms_norm(x, weight), target));
                   });
}

TEST(Autograd, RmsNormGradientWrtWeight) {
    const Tensor x =
        Tensor::leaf({1.0F, -2.0F, 3.0F, 0.5F, 1.5F, -1.0F}, {2, 3}, false);
    const Tensor target =
        Tensor::leaf({0.2F, 0.5F, 0.3F, 0.1F, 0.7F, 0.2F}, {2, 3}, false);
    check_gradient({0.5F, 1.2F, 0.8F}, {3}, [&](const Tensor& weight) {
        return sum(mul(rms_norm(x, weight), target));
    });
}

TEST(Autograd, LayerNormCentersAndScalesRows) {
    const Tensor x = Tensor::leaf({1, 2, 3, 4, 6, 8}, {2, 3}, false);
    const Tensor weight = Tensor::leaf({1, 1, 1}, {3}, false);
    const Tensor bias = Tensor::leaf({0, 0, 0}, {3}, false);
    const Tensor y = layer_norm(x, weight, bias);
    for (int row = 0; row < 2; ++row) {
        float mean = 0.0F;
        float variance = 0.0F;
        for (int col = 0; col < 3; ++col) {
            mean += y.data()[row * 3 + col];
            variance += y.data()[row * 3 + col] * y.data()[row * 3 + col];
        }
        EXPECT_NEAR(mean / 3.0F, 0.0F, 1e-5F);
        EXPECT_NEAR(variance / 3.0F, 1.0F, 1e-5F);
    }
}

TEST(Autograd, LayerNormGradientsMatchFiniteDifference) {
    const std::vector<float> inputs = {0.5F, -1.0F, 2.0F, 1.0F, 0.25F, -0.5F};
    const std::vector<float> target_values = {0.3F, -0.2F, 0.7F,
                                               -0.4F, 0.9F, 0.1F};
    const Tensor target = Tensor::leaf(target_values, {2, 3}, false);
    const Tensor fixed_weight = Tensor::leaf({0.8F, 1.2F, -0.7F}, {3}, false);
    const Tensor fixed_bias = Tensor::leaf({0.1F, -0.3F, 0.2F}, {3}, false);
    check_gradient(inputs, {2, 3}, [&](const Tensor& x) {
        return sum(mul(layer_norm(x, fixed_weight, fixed_bias), target));
    });

    const Tensor fixed_input = Tensor::leaf(inputs, {2, 3}, false);
    check_gradient({0.8F, 1.2F, -0.7F}, {3}, [&](const Tensor& weight) {
        return sum(mul(layer_norm(fixed_input, weight, fixed_bias), target));
    });
    check_gradient({0.1F, -0.3F, 0.2F}, {3}, [&](const Tensor& bias) {
        return sum(mul(layer_norm(fixed_input, fixed_weight, bias), target));
    });
}

TEST(Autograd, TransposeForwardAndGradient) {
    const Tensor a = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    const Tensor t = transpose(a);
    ASSERT_EQ(t.shape(), (std::vector<int>{3, 2}));
    EXPECT_EQ(t.data(), (std::vector<float>{1, 4, 2, 5, 3, 6}));

    const Tensor target =
        Tensor::leaf({0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}, {3, 2}, false);
    check_gradient({1, 2, 3, 4, 5, 6}, {2, 3}, [&](const Tensor& p) {
        return sum(mul(transpose(p), target));
    });
}

TEST(Autograd, EmbeddingGathersAndScatters) {
    // table [vocab=3, dim=2].
    const Tensor table =
        Tensor::leaf({10, 11, 20, 21, 30, 31}, {3, 2}, false);
    const std::vector<int> ids = {2, 0, 2};
    const Tensor gathered = embedding(table, ids);
    ASSERT_EQ(gathered.shape(), (std::vector<int>{3, 2}));
    EXPECT_EQ(gathered.data(), (std::vector<float>{30, 31, 10, 11, 30, 31}));

    // Gradient scatter-adds: id 2 is selected twice, id 0 once, id 1 never.
    const Tensor target =
        Tensor::leaf({0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}, {3, 2}, false);
    check_gradient({10, 11, 20, 21, 30, 31}, {3, 2}, [&](const Tensor& t) {
        return sum(mul(embedding(t, ids), target));
    });
}

TEST(Autograd, CrossEntropyForwardAndGradient) {
    // Uniform logits over 2 classes -> loss = log(2) regardless of target.
    const Tensor uniform = Tensor::leaf({0, 0, 0, 0}, {2, 2}, false);
    const Tensor loss = cross_entropy(uniform, {0, 1});
    EXPECT_NEAR(loss.data()[0], std::log(2.0F), 1e-5F);

    // Gradient check against finite differences.
    const std::vector<int> targets = {2, 0};
    check_gradient({0.5F, -1.0F, 2.0F, 1.0F, 0.0F, -0.5F}, {2, 3},
                   [&](const Tensor& logits) {
                       return cross_entropy(logits, targets);
                   });
}

// Prompt masking (#932). In SFT the sequence is prompt + completion; the prompt is near-identical
// across the corpus, so the model memorizes it within a few steps and it then contributes ~0 error
// while still occupying most of the positions — 87% of the loss on the base-agent corpus. Scoring
// only the completion is what makes the reported loss mean "how well did it answer".
TEST(Autograd, CrossEntropyCanMaskThePrompt) {
    const std::vector<float> values = {0.5F, -1.0F, 2.0F, 1.0F, 0.0F, -0.5F, -2.0F, 0.25F, 1.5F};
    const std::vector<int> targets = {2, 0, 1};

    // Masking the first two rows must give exactly the loss of scoring the last row alone.
    const Tensor all = Tensor::leaf(values, {3, 3}, false);
    const Tensor masked = cross_entropy(all, targets, /*first_scored=*/2);
    const Tensor tail = Tensor::leaf({-2.0F, 0.25F, 1.5F}, {1, 3}, false);
    const Tensor tail_only = cross_entropy(tail, {1});
    EXPECT_NEAR(masked.data()[0], tail_only.data()[0], 1e-5F);

    // ...and it must NOT be the unmasked loss, or the mask is doing nothing.
    const Tensor unmasked = cross_entropy(all, targets);
    EXPECT_GT(std::abs(unmasked.data()[0] - masked.data()[0]), 1e-3F);

    // Masked rows receive EXACTLY zero gradient — the whole point is that the gradient goes to the
    // completion instead of being diluted across tokens already predicted perfectly.
    Tensor logits = Tensor::leaf(values, {3, 3}, true);
    Tensor loss = cross_entropy(logits, targets, /*first_scored=*/2);
    loss.backward();
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 3; ++column) {
            EXPECT_FLOAT_EQ(logits.node()->grad[row * 3 + column], 0.0F)
                << "masked row " << row << " column " << column << " received gradient";
        }
    }
    bool scored_row_has_gradient = false;
    for (int column = 0; column < 3; ++column) {
        if (std::abs(logits.node()->grad[2 * 3 + column]) > 1e-6F) scored_row_has_gradient = true;
    }
    EXPECT_TRUE(scored_row_has_gradient) << "the scored row must actually receive gradient";

    // The masked objective is still differentiable end to end.
    check_gradient(values, {3, 3}, [&](const Tensor& t) {
        return cross_entropy(t, targets, /*first_scored=*/1);
    });
}

// A boundary at or past the end must not divide by zero.
TEST(Autograd, CrossEntropyMaskClampsAtTheLastRow) {
    const Tensor logits = Tensor::leaf({0.5F, -1.0F, 2.0F, 1.0F}, {2, 2}, false);
    const Tensor loss = cross_entropy(logits, {1, 0}, /*first_scored=*/99);
    EXPECT_TRUE(std::isfinite(loss.data()[0]));
}

TEST(Autograd, PolicyGradientLossGradient) {
    const std::vector<int> actions = {1, 3, 0};
    const std::vector<double> advantages = {0.5, -0.3, 0.8};
    check_gradient({0.5F, -1.0F, 2.0F, 0.25F}, {1, 4}, [&](const Tensor& logits) {
        return policy_gradient_loss(logits, actions, advantages);
    });
}

TEST(Autograd, SequencePolicyGradientLossGradient) {
    const std::vector<int> actions = {2, 0, 3};  // one token per step (T=3)
    check_gradient({0.5F, -1.0F, 2.0F, 0.25F,   // step 0 logits
                    1.0F, 0.0F, -0.5F, 0.75F,   // step 1
                    -0.3F, 0.6F, 0.1F, -1.2F},  // step 2
                   {3, 4}, [&](const Tensor& logits) {
                       return sequence_policy_gradient_loss(logits, actions, 0.7);
                   });
}

// With advantage 1 the sequence PG loss is exactly the summed negative
// log-likelihood — rows * cross_entropy — and the advantage scales it linearly.
TEST(Autograd, SequencePolicyGradientMatchesScaledCrossEntropy) {
    const std::vector<int> actions = {2, 0, 3};
    const std::vector<float> data = {0.5F, -1.0F, 2.0F, 0.25F, 1.0F, 0.0F,
                                     -0.5F, 0.75F, -0.3F, 0.6F, 0.1F, -1.2F};
    const Tensor logits = Tensor::leaf(data, {3, 4}, false);

    const float ce = cross_entropy(logits, actions).data()[0];
    const float pg_one = sequence_policy_gradient_loss(logits, actions, 1.0).data()[0];
    EXPECT_NEAR(pg_one, 3.0F * ce, 1e-4F);  // -sum log p == rows * mean(-log p)

    const float pg_neg = sequence_policy_gradient_loss(logits, actions, -2.0).data()[0];
    EXPECT_NEAR(pg_neg, -2.0F * pg_one, 1e-4F);  // advantage scales linearly
}

TEST(Autograd, SequencePolicyGradientZeroAdvantageIsZero) {
    const std::vector<int> actions = {1, 2};
    const Tensor logits =
        Tensor::leaf({0.5F, -1.0F, 2.0F, 0.25F, 1.0F, 0.0F, -0.5F, 0.75F}, {2, 4}, true);
    Tensor loss = sequence_policy_gradient_loss(logits, actions, 0.0);
    EXPECT_FLOAT_EQ(loss.data()[0], 0.0F);
    loss.backward();
    for (float gradient : logits.grad()) {
        EXPECT_FLOAT_EQ(gradient, 0.0F);
    }
}

// matmul routed through a ComputeDevice (CpuDevice) matches the host kernel — the
// same graph, on a device.
TEST(Autograd, MatmulOnComputeDeviceMatchesHost) {
    const Tensor a = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    const Tensor b = Tensor::leaf({1, 0, 0, 1, 1, 1}, {3, 2}, false);
    const std::vector<float> host = matmul(a, b).data();

    CpuDevice device;
    set_compute_device(&device);
    const std::vector<float> on_device = matmul(a, b).data();
    set_compute_device(nullptr);

    ASSERT_EQ(host.size(), on_device.size());
    for (std::size_t i = 0; i < host.size(); ++i) {
        EXPECT_NEAR(host[i], on_device[i], 1e-4F);
    }
}

// The device-backed matmul's forward AND backward (gemm with transpose flags) are
// correct — gradient-checked with a CpuDevice active.
TEST(Autograd, MatmulGradientOnComputeDevice) {
    CpuDevice device;
    set_compute_device(&device);
    const Tensor x = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    check_gradient({0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}, {3, 2},
                   [&](const Tensor& w) { return sum(matmul(x, w)); });
    set_compute_device(nullptr);
}

TEST(Autograd, RopeIsIdentityAtPositionZeroAndPreservesNorm) {
    // 3 positions, d=4. Position 0 should be unchanged (angle 0); every position's
    // row norm is preserved (RoPE is a rotation).
    const std::vector<float> data = {1, 2, 3, 4, 0.5F, -1, 2, 1, -2, 3, 1, 0.5F};
    const Tensor x = Tensor::leaf(data, {3, 4}, false);
    const Tensor y = rope(x);

    for (int col = 0; col < 4; ++col) {
        EXPECT_NEAR(y.data()[col], data[col], 1e-6F);  // position 0 unchanged
    }
    for (int pos = 0; pos < 3; ++pos) {
        float before = 0.0F;
        float after = 0.0F;
        for (int col = 0; col < 4; ++col) {
            before += data[pos * 4 + col] * data[pos * 4 + col];
            after += y.data()[pos * 4 + col] * y.data()[pos * 4 + col];
        }
        EXPECT_NEAR(before, after, 1e-4F) << "position " << pos;
    }
}

TEST(Autograd, RopeGradient) {
    const Tensor target =
        Tensor::leaf({0.2F, 0.5F, 0.3F, 0.1F, 0.7F, 0.2F, 0.4F, 0.6F}, {2, 4},
                     false);
    check_gradient({1, 2, 3, 4, 0.5F, -1, 2, 1}, {2, 4},
                   [&](const Tensor& x) { return sum(mul(rope(x), target)); });
}

TEST(Autograd, ColSliceAndConcatRoundTrip) {
    // Split a [2,4] into two [2,2] column halves and concat back -> identity.
    const Tensor x = Tensor::leaf({1, 2, 3, 4, 5, 6, 7, 8}, {2, 4}, false);
    const Tensor left = col_slice(x, 0, 2);
    const Tensor right = col_slice(x, 2, 2);
    ASSERT_EQ(left.data(), (std::vector<float>{1, 2, 5, 6}));
    ASSERT_EQ(right.data(), (std::vector<float>{3, 4, 7, 8}));
    const Tensor joined = concat_cols({left, right});
    EXPECT_EQ(joined.data(), x.data());
}

TEST(Autograd, ColSliceGradient) {
    const Tensor target = Tensor::leaf({0.1F, 0.2F, 0.3F, 0.4F}, {2, 2}, false);
    check_gradient({1, 2, 3, 4, 5, 6}, {2, 3}, [&](const Tensor& x) {
        return sum(mul(col_slice(x, 1, 2), target));
    });
}

TEST(Autograd, ConcatColsGradientReachesAllParts) {
    // Gradient flows to a parameter used in one concatenated part.
    const Tensor other = Tensor::leaf({9, 9, 9, 9}, {2, 2}, false);
    const Tensor weight = Tensor::leaf({0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F}, {2, 4}, false);
    check_gradient({1, 2, 3, 4}, {2, 2}, [&](const Tensor& p) {
        return sum(mul(concat_cols({p, other}), weight));
    });
}

// matmul_transposed_b(a, b) == matmul(a, transpose(b)) — same result, but without
// materializing the transpose (the real-checkpoint inference win).
TEST(Autograd, MatmulTransposedBMatchesMatmulOfTranspose) {
    const Tensor a = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);   // [m=2, k=3]
    const Tensor b = Tensor::leaf({1, 0, -1, 2, 1, 0, 0, 3, 1, -1, 2, 1}, {4, 3}, false);  // [n=4, k=3]
    const std::vector<float> direct = matmul_transposed_b(a, b).data();
    const std::vector<float> viaTranspose = matmul(a, transpose(b)).data();
    ASSERT_EQ(direct.size(), viaTranspose.size());
    for (std::size_t i = 0; i < direct.size(); ++i) {
        EXPECT_NEAR(direct[i], viaTranspose[i], 1e-4F) << "element " << i;
    }
}

// Gradient flows correctly to BOTH operands of matmul_transposed_b.
TEST(Autograd, MatmulTransposedBGradientToA) {
    const Tensor b = Tensor::leaf({0.5F, -1.0F, 2.0F, 0.25F, 1.0F, 0.0F}, {2, 3}, false);
    const Tensor weight = Tensor::leaf({0.3F, -0.7F, 1.1F, 0.2F}, {2, 2}, false);
    check_gradient({1, 2, 3, 4, 5, 6}, {2, 3}, [&](const Tensor& a) {
        return sum(mul(matmul_transposed_b(a, b), weight));  // a [2,3], b [2,3] -> [2,2]
    });
}

TEST(Autograd, MatmulTransposedBGradientToB) {
    const Tensor a = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    const Tensor weight = Tensor::leaf({0.3F, -0.7F, 1.1F, 0.2F}, {2, 2}, false);
    check_gradient({0.5F, -1.0F, 2.0F, 0.25F, 1.0F, 0.0F}, {2, 3}, [&](const Tensor& b) {
        return sum(mul(matmul_transposed_b(a, b), weight));
    });
}

// A composed, nonlinear-in-the-parameter graph: loss = sum((X·W) ⊙ (X·W)).
TEST(Autograd, ComposedGraphGradient) {
    const Tensor x = Tensor::leaf({1, 2, 3, 4, 5, 6}, {2, 3}, false);
    check_gradient({0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F}, {3, 2},
                   [&](const Tensor& w) {
                       const Tensor y = matmul(x, w);
                       return sum(mul(y, y));
                   });
}

}  // namespace
