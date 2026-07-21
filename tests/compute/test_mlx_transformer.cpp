#include <gtest/gtest.h>

// The transformer building blocks, exercised on MLX. Compiles only under
// ZETTA_WITH_MLX (a -DZETTA_ENABLE_MLX=ON build); otherwise an empty TU.
#if defined(ZETTA_WITH_MLX)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <mlx/mlx.h>

#include "zetta/compute/mlx_transformer.hpp"

namespace {

namespace mx = mlx::core;
using zetta::compute::model::causal_self_attention;
using zetta::compute::model::feed_forward;
using zetta::compute::model::rms_norm;

mx::array weight(int rows, int cols, std::uint64_t seed) {
    return mx::random::normal(mx::Shape{rows, cols}, mx::random::key(seed)) * 0.1F;
}

TEST(MlxTransformer, RmsNormGivesUnitRmsRows) {
    const mx::array x = mx::random::normal(mx::Shape{3, 8}, mx::random::key(1));
    const mx::array scale = mx::ones(mx::Shape{8}, mx::float32);
    const mx::array y = rms_norm(x, scale);

    mx::array mean_square = mx::mean(mx::multiply(y, y), std::vector<int>{-1});
    mx::eval(mean_square);
    for (int row = 0; row < 3; ++row) {
        EXPECT_NEAR(mean_square.data<float>()[row], 1.0F, 1e-2F);
    }
}

TEST(MlxTransformer, FeedForwardKeepsShape) {
    const int seq = 4;
    const int d_model = 8;
    const int d_ff = 16;
    const mx::array x = mx::random::normal(mx::Shape{seq, d_model}, mx::random::key(2));
    const mx::array y = feed_forward(x, weight(d_model, d_ff, 3), weight(d_ff, d_model, 4));
    ASSERT_EQ(y.shape(0), seq);
    ASSERT_EQ(y.shape(1), d_model);
}

TEST(MlxTransformer, AttentionKeepsShape) {
    const int seq = 5;
    const int d_model = 8;
    const mx::array x = mx::random::normal(mx::Shape{seq, d_model}, mx::random::key(5));
    const mx::array y = causal_self_attention(
        x, weight(d_model, d_model, 6), weight(d_model, d_model, 7),
        weight(d_model, d_model, 8), weight(d_model, d_model, 9), /*n_heads=*/2);
    ASSERT_EQ(y.shape(0), seq);
    ASSERT_EQ(y.shape(1), d_model);
}

// Causality: perturbing the LAST token must not change position 0's output (it
// cannot attend to the future), but it must change the last position's output (so
// the test is not vacuous).
TEST(MlxTransformer, AttentionIsCausal) {
    const int seq = 4;
    const int d_model = 8;
    const mx::array wq = weight(d_model, d_model, 10);
    const mx::array wk = weight(d_model, d_model, 11);
    const mx::array wv = weight(d_model, d_model, 12);
    const mx::array wo = weight(d_model, d_model, 13);

    const mx::array x = mx::random::normal(mx::Shape{seq, d_model}, mx::random::key(14));
    // A perturbation applied only to the last row.
    const std::vector<float> row_mask_data = {0.0F, 0.0F, 0.0F, 1.0F};
    const mx::array row_mask(row_mask_data.data(), mx::Shape{seq, 1}, mx::float32);
    const mx::array noise = mx::random::normal(mx::Shape{seq, d_model}, mx::random::key(15));
    const mx::array x_perturbed = x + mx::multiply(noise, row_mask);

    mx::array out = causal_self_attention(x, wq, wk, wv, wo, 2);
    mx::array out_perturbed = causal_self_attention(x_perturbed, wq, wk, wv, wo, 2);

    mx::array first = mx::take(out, 0, 0);
    mx::array first_perturbed = mx::take(out_perturbed, 0, 0);
    mx::array last = mx::take(out, seq - 1, 0);
    mx::array last_perturbed = mx::take(out_perturbed, seq - 1, 0);
    mx::eval(first, first_perturbed, last, last_perturbed);

    float first_diff = 0.0F;
    float last_diff = 0.0F;
    for (int i = 0; i < d_model; ++i) {
        first_diff = std::max(first_diff,
                              std::abs(first.data<float>()[i] -
                                       first_perturbed.data<float>()[i]));
        last_diff = std::max(
            last_diff,
            std::abs(last.data<float>()[i] - last_perturbed.data<float>()[i]));
    }
    EXPECT_LT(first_diff, 1e-4F);  // position 0 cannot see the future
    EXPECT_GT(last_diff, 1e-4F);   // the last position does see the change
}

}  // namespace

#endif  // ZETTA_WITH_MLX
