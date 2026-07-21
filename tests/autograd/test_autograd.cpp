#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <vector>

#include "zetta/autograd/tensor.hpp"

namespace {

using zetta::autograd::add;
using zetta::autograd::add_bias;
using zetta::autograd::cross_entropy;
using zetta::autograd::embedding;
using zetta::autograd::gelu;
using zetta::autograd::matmul;
using zetta::autograd::mul;
using zetta::autograd::policy_gradient_loss;
using zetta::autograd::rms_norm;
using zetta::autograd::row_softmax;
using zetta::autograd::scale;
using zetta::autograd::silu;
using zetta::autograd::sum;
using zetta::autograd::Tensor;
using zetta::autograd::transpose;

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

TEST(Autograd, PolicyGradientLossGradient) {
    const std::vector<int> actions = {1, 3, 0};
    const std::vector<double> advantages = {0.5, -0.3, 0.8};
    check_gradient({0.5F, -1.0F, 2.0F, 0.25F}, {1, 4}, [&](const Tensor& logits) {
        return policy_gradient_loss(logits, actions, advantages);
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
