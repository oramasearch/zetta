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
#include <random>
#include <vector>

#include <mlx/mlx.h>

#include "zetta/agent/advantage.hpp"
#include "zetta/compute/mlx_transformer.hpp"
#include "zetta/compute/safetensors.hpp"

namespace {

namespace mx = mlx::core;
using zetta::agent::group_advantages;
using zetta::compute::model::deserialize_safetensors;
using zetta::compute::model::serialize_safetensors;
using zetta::compute::model::AdamW;
using zetta::compute::model::causal_self_attention;
using zetta::compute::model::cross_entropy_next_token;
using zetta::compute::model::feed_forward;
using zetta::compute::model::MlxTransformer;
using zetta::compute::model::policy_gradient_loss;
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

// Softmax of the logits at the last position (read to host).
std::vector<double> last_position_softmax(const mx::array& logits, int vocab) {
    mx::array evaluated = logits;
    mx::eval(evaluated);
    const int seq = evaluated.shape(0);
    const float* row = evaluated.data<float>() + (seq - 1) * vocab;
    double max_logit = row[0];
    for (int c = 1; c < vocab; ++c) max_logit = std::max(max_logit, double(row[c]));
    std::vector<double> probs(vocab, 0.0);
    double sum = 0.0;
    for (int c = 0; c < vocab; ++c) {
        probs[c] = std::exp(row[c] - max_logit);
        sum += probs[c];
    }
    for (double& p : probs) p /= sum;
    return probs;
}

int sample_from(const std::vector<double>& probs, double uniform_draw) {
    double cumulative = 0.0;
    for (std::size_t i = 0; i < probs.size(); ++i) {
        cumulative += probs[i];
        if (uniform_draw < cumulative) return static_cast<int>(i);
    }
    return static_cast<int>(probs.size()) - 1;
}

// Overfit `model` to `sequence` with next-token cross-entropy (shared by the
// autodiff and generation tests).
void overfit_to_sequence(MlxTransformer& model, const TransformerConfig& config,
                         const std::vector<int>& sequence, int steps) {
    const mx::array ids = token_ids(sequence);
    std::vector<int> argnums(model.parameters().size());
    std::iota(argnums.begin(), argnums.end(), 0);
    const std::function<mx::array(const std::vector<mx::array>&)> loss_fn =
        [&](const std::vector<mx::array>& parameters) {
            return cross_entropy_next_token(config, parameters, ids);
        };
    auto grad_fn = mx::value_and_grad(loss_fn, argnums);
    AdamW optimizer(/*learning_rate=*/1e-2F);
    for (int step = 0; step < steps; ++step) {
        auto [loss, gradients] = grad_fn(model.parameters());
        optimizer.update(model.parameters(), gradients);
    }
}

int argmax_last_position(const mx::array& logits, int vocab) {
    mx::array evaluated = logits;
    mx::eval(evaluated);
    const int seq = evaluated.shape(0);
    const float* row = evaluated.data<float>() + (seq - 1) * vocab;
    int best = 0;
    for (int c = 1; c < vocab; ++c) {
        if (row[c] > row[best]) best = c;
    }
    return best;
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

// The #555 objective, in miniature: the transformer learns a verifiable game PURELY
// FROM REWARD (RLVR) — never shown the answer. For prompt [0, q] it must emit
// answer(q); the reward is 1 iff the sampled answer token is correct. Policy
// gradient (through the whole transformer) + GRPO advantages + AdamW drive the
// output distribution to the right token for each question.
TEST(MlxTransformer, LearnsAVerifiableGameByPolicyGradient) {
    const TransformerConfig config = small_config();  // vocab 16
    MlxTransformer model(config, 5);
    AdamW optimizer(/*learning_rate=*/1e-2F);

    const std::vector<int> questions = {2, 5, 8};
    const auto answer = [](int q) { return q + 4; };  // 6, 9, 12 — distinct, in vocab

    std::mt19937 rng(3);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const int group = 24;

    std::vector<int> argnums(model.parameters().size());
    std::iota(argnums.begin(), argnums.end(), 0);

    for (int step = 0; step < 600; ++step) {
        const int q = questions[step % questions.size()];
        const mx::array prompt = token_ids({0, q});

        // Sample a group of answers from the current policy; reward = correct?
        const std::vector<double> probs =
            last_position_softmax(model.forward(prompt), config.vocab_size);
        std::vector<int> actions(group);
        std::vector<double> rewards(group);
        for (int i = 0; i < group; ++i) {
            actions[i] = sample_from(probs, uniform(rng));
            rewards[i] = actions[i] == answer(q) ? 1.0 : 0.0;
        }
        const std::vector<double> advantages = group_advantages(rewards);

        // Policy-gradient step through the transformer.
        const std::function<mx::array(const std::vector<mx::array>&)> loss_fn =
            [&](const std::vector<mx::array>& parameters) {
                return policy_gradient_loss(config, parameters, prompt, actions,
                                            advantages);
            };
        auto grad_fn = mx::value_and_grad(loss_fn, argnums);
        auto [loss, gradients] = grad_fn(model.parameters());
        optimizer.update(model.parameters(), gradients);
    }

    // After RLVR training the greedy answer for each question is correct — learned
    // from reward alone.
    for (int q : questions) {
        EXPECT_EQ(argmax_last_position(model.forward(token_ids({0, q})),
                                       config.vocab_size),
                  answer(q))
            << "question " << q;
    }
}

// The agent emits an action as text: after overfitting a sequence, greedy
// generation continues it. Causality guarantees a prefix's last-position logits
// equal the full sequence's at that position, so generation reproduces what was
// learned — and it is deterministic.
TEST(MlxTransformer, GeneratesTheLearnedContinuation) {
    const TransformerConfig config = small_config();
    MlxTransformer model(config, 3);
    const std::vector<int> sequence = {2, 5, 7, 3, 9, 1, 4};
    overfit_to_sequence(model, config, sequence, 250);

    const std::vector<int> generated = model.generate({2, 5}, /*max_new_tokens=*/5);
    EXPECT_EQ(generated, (std::vector<int>{7, 3, 9, 1, 4}));
    EXPECT_EQ(model.generate({2, 5}, 5), generated);  // deterministic
}

using zetta::compute::model::distributed_adamw_step;
using zetta::compute::model::flatten_arrays;
using zetta::compute::model::unflatten_like;

// The gradients of a fixed loss for a model (used to drive the distributed step).
std::vector<mx::array> loss_gradients(const MlxTransformer& model,
                                      const TransformerConfig& config,
                                      const std::vector<int>& sequence) {
    const mx::array ids = token_ids(sequence);
    std::vector<int> argnums(model.parameters().size());
    std::iota(argnums.begin(), argnums.end(), 0);
    const std::function<mx::array(const std::vector<mx::array>&)> loss_fn =
        [&](const std::vector<mx::array>& parameters) {
            return cross_entropy_next_token(config, parameters, ids);
        };
    return mx::value_and_grad(loss_fn, argnums)(model.parameters()).second;
}

double max_abs_difference(const std::vector<mx::array>& a,
                          const std::vector<mx::array>& b) {
    const std::vector<double> flat_a = flatten_arrays(a);
    const std::vector<double> flat_b = flatten_arrays(b);
    double worst = 0.0;
    for (std::size_t i = 0; i < flat_a.size(); ++i) {
        worst = std::max(worst, std::abs(flat_a[i] - flat_b[i]));
    }
    return worst;
}

TEST(MlxTransformer, FlattenUnflattenRoundTrips) {
    const MlxTransformer model(small_config(), 1);
    const std::vector<double> flat = flatten_arrays(model.parameters());
    const std::vector<mx::array> restored =
        unflatten_like(flat, model.parameters());
    ASSERT_EQ(restored.size(), model.parameters().size());
    EXPECT_LT(max_abs_difference(restored, model.parameters()), 1e-6);
}

// With an identity allreduce the distributed step is a plain AdamW step.
TEST(MlxTransformer, DistributedStepWithIdentityEqualsPlainAdamW) {
    const TransformerConfig config = small_config();
    MlxTransformer plain(config, 2);
    MlxTransformer distributed(config, 2);  // identical init
    const std::vector<int> sequence = {1, 2, 3, 4};
    const std::vector<mx::array> gradients =
        loss_gradients(plain, config, sequence);

    AdamW optimizer_plain;
    AdamW optimizer_distributed;
    optimizer_plain.update(plain.parameters(), gradients);
    distributed_adamw_step(distributed.parameters(), gradients,
                           optimizer_distributed,
                           [](const std::vector<double>& g) { return g; });

    EXPECT_LT(max_abs_difference(plain.parameters(), distributed.parameters()),
              1e-6);
}

// The distributed invariant: a step whose allreduce SUMS this node's gradient with a
// peer's equals a single step with the summed gradient — data-parallel training of
// the transformer is single-node == multi-node by construction.
TEST(MlxTransformer, DistributedStepSumsGradientsAcrossNodes) {
    const TransformerConfig config = small_config();
    MlxTransformer combined(config, 4);
    MlxTransformer sharded(config, 4);  // identical init

    // Two nodes' gradients (from two different sequences).
    const std::vector<mx::array> grad_a = loss_gradients(combined, config, {1, 2, 3});
    const std::vector<mx::array> grad_b = loss_gradients(combined, config, {4, 5, 6});
    const std::vector<double> peer = flatten_arrays(grad_b);

    // Combined: one step with grad_a + grad_b.
    const std::vector<double> summed_flat = [&] {
        std::vector<double> local = flatten_arrays(grad_a);
        for (std::size_t i = 0; i < local.size(); ++i) local[i] += peer[i];
        return local;
    }();
    AdamW optimizer_combined;
    optimizer_combined.update(combined.parameters(),
                              unflatten_like(summed_flat, grad_a));

    // Sharded: node A applies grad_a with an allreduce that adds peer B's gradient.
    AdamW optimizer_sharded;
    distributed_adamw_step(
        sharded.parameters(), grad_a, optimizer_sharded,
        [&](const std::vector<double>& local) {
            std::vector<double> global(local.size(), 0.0);
            for (std::size_t i = 0; i < local.size(); ++i) {
                global[i] = local[i] + peer[i];
            }
            return global;
        });

    EXPECT_LT(max_abs_difference(combined.parameters(), sharded.parameters()),
              1e-6);
}

// A trained model saved to safetensors and loaded into a freshly (differently)
// initialized model reproduces its outputs exactly — the checkpoint round-trips
// through the real format.
TEST(MlxTransformer, CheckpointRoundTripsThroughSafetensors) {
    const TransformerConfig config = small_config();
    MlxTransformer trained(config, 3);
    overfit_to_sequence(trained, config, {2, 5, 7, 3, 9, 1, 4}, 200);

    const std::string checkpoint = serialize_safetensors(trained.named_parameters());

    MlxTransformer restored(config, 99);  // different init
    ASSERT_TRUE(
        restored.load_named_parameters(deserialize_safetensors(checkpoint)));

    // Identical forward outputs and identical generation after loading.
    const mx::array ids = token_ids({2, 5, 7});
    EXPECT_LT(max_abs_difference({trained.forward(ids)}, {restored.forward(ids)}),
              1e-6);
    EXPECT_EQ(restored.generate({2, 5}, 5), trained.generate({2, 5}, 5));
}

TEST(MlxTransformer, LoadRejectsMissingOrMismatchedTensors) {
    const TransformerConfig config = small_config();
    MlxTransformer model(config, 4);
    const std::vector<double> before = flatten_arrays(model.parameters());

    // Missing tensors.
    EXPECT_FALSE(model.load_named_parameters({}));

    // A present but wrong-shaped tensor.
    std::map<std::string, mx::array> named = model.named_parameters();
    named.at("token_embedding") = mx::zeros(mx::Shape{3, 3}, mx::float32);
    EXPECT_FALSE(model.load_named_parameters(named));

    // The model is untouched by a rejected load.
    const std::vector<double> after = flatten_arrays(model.parameters());
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_DOUBLE_EQ(before[i], after[i]);
    }
}

}  // namespace

#endif  // ZETTA_WITH_MLX
