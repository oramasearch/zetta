#include <gtest/gtest.h>

// The transformer building blocks, exercised on MLX. Compiles only under
// ZETTA_WITH_MLX (a -DZETTA_ENABLE_MLX=ON build); otherwise an empty TU.
#if defined(ZETTA_WITH_MLX)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <vector>

#include <mlx/mlx.h>

#include "zetta/compute/mlx_transformer.hpp"

namespace {

namespace mx = mlx::core;
using zetta::compute::model::AdamW;
using zetta::compute::model::causal_self_attention;
using zetta::compute::model::cross_entropy_next_token;
using zetta::compute::model::feed_forward;
using zetta::compute::model::MlxTransformer;
using zetta::compute::model::rms_norm;
using zetta::compute::model::TransformerConfig;

TransformerConfig small_config() {
    TransformerConfig config;
    config.vocab_size = 16;
    config.d_model = 16;
    config.n_heads = 2;
    config.n_layers = 2;
    config.d_ff = 32;
    config.max_seq = 8;
    return config;
}

mx::array token_ids(const std::vector<int>& ids) {
    return mx::array(ids.data(), mx::Shape{static_cast<int>(ids.size())},
                     mx::int32);
}

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

TEST(MlxTransformer, ParameterCountMatchesLayout) {
    TransformerConfig config = small_config();
    config.n_layers = 3;
    const MlxTransformer model(config, 1);
    // token + positional embedding, 8 tensors per layer, final norm + projection.
    EXPECT_EQ(model.parameters().size(),
              static_cast<std::size_t>(2 + 8 * 3 + 2));
}

TEST(MlxTransformer, ForwardProducesFiniteLogits) {
    const MlxTransformer model(small_config(), 42);
    const mx::array ids = token_ids({1, 2, 3, 4, 5});
    mx::array logits = model.forward(ids);
    ASSERT_EQ(logits.shape(0), 5);
    ASSERT_EQ(logits.shape(1), 16);
    mx::eval(logits);
    for (int i = 0; i < 5 * 16; ++i) {
        EXPECT_TRUE(std::isfinite(logits.data<float>()[i]));
    }
}

TEST(MlxTransformer, ForwardIsDeterministic) {
    const MlxTransformer model(small_config(), 7);
    const mx::array ids = token_ids({3, 1, 4, 1, 5});
    mx::array a = model.forward(ids);
    mx::array b = model.forward(ids);
    mx::eval(a, b);
    for (int i = 0; i < a.shape(0) * a.shape(1); ++i) {
        EXPECT_FLOAT_EQ(a.data<float>()[i], b.data<float>()[i]);
    }
}

// The linchpin: real backprop (mx::value_and_grad through attention + MLP) + AdamW
// must overfit a fixed sequence — the loss collapses and the model then predicts
// the sequence greedily. If autodiff or the optimizer were wrong, this fails.
TEST(MlxTransformer, OverfitsAShortSequenceWithAutodiffAndAdamW) {
    const TransformerConfig config = small_config();
    MlxTransformer model(config, 3);
    const std::vector<int> sequence = {2, 5, 7, 3, 9, 1, 4};
    const mx::array ids = token_ids(sequence);

    std::vector<int> argnums(model.parameters().size());
    std::iota(argnums.begin(), argnums.end(), 0);
    const std::function<mx::array(const std::vector<mx::array>&)> loss_fn =
        [&](const std::vector<mx::array>& parameters) {
            return cross_entropy_next_token(config, parameters, ids);
        };
    auto grad_fn = mx::value_and_grad(loss_fn, argnums);

    AdamW optimizer(/*learning_rate=*/1e-2F);
    float first_loss = 0.0F;
    float last_loss = 0.0F;
    for (int step = 0; step < 250; ++step) {
        auto [loss, gradients] = grad_fn(model.parameters());
        mx::eval(loss);
        const float value = loss.item<float>();
        if (step == 0) first_loss = value;
        last_loss = value;
        optimizer.update(model.parameters(), gradients);
    }
    EXPECT_LT(last_loss, first_loss * 0.2F);  // the loss collapsed (overfit)

    // The model now predicts the sequence greedily: argmax(logits[t]) == ids[t+1].
    mx::array logits = model.forward(ids);
    mx::eval(logits);
    const int vocab = config.vocab_size;
    for (std::size_t t = 0; t + 1 < sequence.size(); ++t) {
        const float* row = logits.data<float>() + t * vocab;
        int best = 0;
        for (int c = 1; c < vocab; ++c) {
            if (row[c] > row[best]) best = c;
        }
        EXPECT_EQ(best, sequence[t + 1]) << "position " << t;
    }
}

}  // namespace

#endif  // ZETTA_WITH_MLX
