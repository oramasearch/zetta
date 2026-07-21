#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

#include "zetta/autograd/tensor.hpp"
#include "zetta/autograd/transformer.hpp"

namespace {

using zetta::autograd::causal_self_attention;
using zetta::autograd::feed_forward;
using zetta::autograd::sum;
using zetta::autograd::Tensor;

void check_gradient(const std::vector<float>& param,
                    const std::vector<int>& shape,
                    const std::function<Tensor(const Tensor&)>& build,
                    float tolerance = 5e-2F) {
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

std::vector<float> small_weights(int count) {
    std::vector<float> w(count, 0.0F);
    for (int i = 0; i < count; ++i) {
        w[i] = 0.05F * static_cast<float>((i % 7) - 3);  // small, varied, deterministic
    }
    return w;
}

TEST(AutogradTransformer, AttentionGradientFlows) {
    const int seq = 4;
    const int d = 4;
    const Tensor x = Tensor::leaf(small_weights(seq * d), {seq, d}, false);
    const Tensor wk = Tensor::leaf(small_weights(d * d), {d, d}, false);
    const Tensor wv = Tensor::leaf(small_weights(d * d), {d, d}, false);
    const Tensor wo = Tensor::leaf(small_weights(d * d), {d, d}, false);
    check_gradient(small_weights(d * d), {d, d}, [&](const Tensor& wq) {
        return sum(causal_self_attention(x, wq, wk, wv, wo));
    });
}

TEST(AutogradTransformer, FeedForwardGradientFlows) {
    const int seq = 3;
    const int d = 4;
    const int ff = 6;
    const Tensor x = Tensor::leaf(small_weights(seq * d), {seq, d}, false);
    const Tensor w2 = Tensor::leaf(small_weights(ff * d), {ff, d}, false);
    check_gradient(small_weights(d * ff), {d, ff}, [&](const Tensor& w1) {
        return sum(feed_forward(x, w1, w2));
    });
}

// Perturbing the last token must not change position 0's attention output (it
// cannot see the future), but must change the last position's.
TEST(AutogradTransformer, AttentionIsCausal) {
    const int seq = 4;
    const int d = 4;
    const Tensor wq = Tensor::leaf(small_weights(d * d), {d, d}, false);
    const Tensor wk = Tensor::leaf(small_weights(d * d), {d, d}, false);
    const Tensor wv = Tensor::leaf(small_weights(d * d), {d, d}, false);
    const Tensor wo = Tensor::leaf(small_weights(d * d), {d, d}, false);

    std::vector<float> x = small_weights(seq * d);
    std::vector<float> x_perturbed = x;
    for (int col = 0; col < d; ++col) {
        x_perturbed[(seq - 1) * d + col] += 0.5F;  // perturb the last row only
    }
    const Tensor out = causal_self_attention(Tensor::leaf(x, {seq, d}, false), wq,
                                             wk, wv, wo);
    const Tensor out_perturbed = causal_self_attention(
        Tensor::leaf(x_perturbed, {seq, d}, false), wq, wk, wv, wo);

    float first_diff = 0.0F;
    float last_diff = 0.0F;
    for (int col = 0; col < d; ++col) {
        first_diff = std::max(
            first_diff, std::abs(out.data()[col] - out_perturbed.data()[col]));
        last_diff = std::max(last_diff,
                             std::abs(out.data()[(seq - 1) * d + col] -
                                      out_perturbed.data()[(seq - 1) * d + col]));
    }
    EXPECT_LT(first_diff, 1e-5F);  // position 0 cannot see the future
    EXPECT_GT(last_diff, 1e-5F);   // the last position does
}

}  // namespace
