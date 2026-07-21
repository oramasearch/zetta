#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "zetta/autograd/tensor.hpp"

namespace {

using zetta::autograd::matmul;
using zetta::autograd::sum;
using zetta::autograd::Tensor;

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

}  // namespace
