#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <random>
#include <vector>

#include "zetta/agent/advantage.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/autograd/transformer.hpp"

namespace {

using zetta::agent::group_advantages;
using zetta::autograd::Adam;
using zetta::autograd::causal_self_attention;
using zetta::autograd::cross_entropy;
using zetta::autograd::feed_forward;
using zetta::autograd::grouped_query_attention;
using zetta::autograd::policy_gradient_loss;
using zetta::autograd::swiglu_feed_forward;
using zetta::autograd::row_slice;
using zetta::autograd::sum;
using zetta::autograd::Tensor;
using zetta::autograd::Transformer;
using zetta::autograd::TransformerConfig;
using zetta::autograd::zero_grad;

// Softmax of a single logits row (host), and inverse-CDF sampling.
std::vector<double> softmax_row(const std::vector<float>& row) {
    double max_value = row[0];
    for (float v : row) max_value = std::max(max_value, double(v));
    std::vector<double> probs(row.size(), 0.0);
    double sum_exp = 0.0;
    for (std::size_t i = 0; i < row.size(); ++i) {
        probs[i] = std::exp(row[i] - max_value);
        sum_exp += probs[i];
    }
    for (double& p : probs) p /= sum_exp;
    return probs;
}

int sample_from(const std::vector<double>& probs, double u) {
    double cumulative = 0.0;
    for (std::size_t i = 0; i < probs.size(); ++i) {
        cumulative += probs[i];
        if (u < cumulative) return static_cast<int>(i);
    }
    return static_cast<int>(probs.size()) - 1;
}

TransformerConfig small_config() {
    TransformerConfig config;
    config.vocab_size = 12;
    config.d_model = 16;
    config.n_layers = 2;
    config.d_ff = 32;
    config.max_seq = 8;
    return config;
}

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

TEST(AutogradTransformer, SwigluGradientFlows) {
    const int seq = 3;
    const int d = 4;
    const int ff = 6;
    const Tensor x = Tensor::leaf(small_weights(seq * d), {seq, d}, false);
    const Tensor w_up = Tensor::leaf(small_weights(d * ff), {d, ff}, false);
    const Tensor w_down = Tensor::leaf(small_weights(ff * d), {ff, d}, false);
    check_gradient(small_weights(d * ff), {d, ff}, [&](const Tensor& w_gate) {
        return sum(swiglu_feed_forward(x, w_gate, w_up, w_down));
    });
}

// Grouped-query attention: n_heads=4 query heads over n_kv_heads=2 shared
// key/value heads (group_size 2), head_dim 3. Gradient flows through the whole
// block (RoPE per head + per-head SDPA + concat + wo) to wq.
TEST(AutogradTransformer, GroupedQueryAttentionGradientFlows) {
    const int seq = 4;
    const int n_heads = 4;
    const int n_kv_heads = 2;
    const int head_dim = 3;
    const int d_model = n_heads * head_dim;  // 12
    const int kv_dim = n_kv_heads * head_dim;  // 6

    const Tensor x = Tensor::leaf(small_weights(seq * d_model), {seq, d_model}, false);
    const Tensor wk = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wv = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wo = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    check_gradient(small_weights(d_model * d_model), {d_model, d_model},
                   [&](const Tensor& wq) {
                       return sum(grouped_query_attention(x, wq, wk, wv, wo, n_heads,
                                                          n_kv_heads, head_dim));
                   });
}

// The gradient also reaches the key projection, whose columns are shared across
// each group of query heads (the defining property of GQA).
TEST(AutogradTransformer, GroupedQueryAttentionGradientReachesSharedKv) {
    const int seq = 3;
    const int n_heads = 4;
    const int n_kv_heads = 2;
    const int head_dim = 2;
    const int d_model = n_heads * head_dim;  // 8
    const int kv_dim = n_kv_heads * head_dim;  // 4

    const Tensor x = Tensor::leaf(small_weights(seq * d_model), {seq, d_model}, false);
    const Tensor wq = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wv = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wo = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    check_gradient(small_weights(d_model * kv_dim), {d_model, kv_dim},
                   [&](const Tensor& wk) {
                       return sum(grouped_query_attention(x, wq, wk, wv, wo, n_heads,
                                                          n_kv_heads, head_dim));
                   });
}

TEST(AutogradTransformer, GroupedQueryAttentionShapeAndFiniteness) {
    const int seq = 5;
    const int n_heads = 4;
    const int n_kv_heads = 2;
    const int head_dim = 3;
    const int d_model = n_heads * head_dim;
    const int kv_dim = n_kv_heads * head_dim;
    const Tensor x = Tensor::leaf(small_weights(seq * d_model), {seq, d_model}, false);
    const Tensor wq = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wk = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wv = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wo = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor out =
        grouped_query_attention(x, wq, wk, wv, wo, n_heads, n_kv_heads, head_dim);
    ASSERT_EQ(out.shape(), (std::vector<int>{seq, d_model}));
    for (float value : out.data()) {
        EXPECT_TRUE(std::isfinite(value));
    }
}

// GQA is causal: perturbing the last token cannot change position 0's output,
// but must change the last position's.
TEST(AutogradTransformer, GroupedQueryAttentionIsCausal) {
    const int seq = 4;
    const int n_heads = 2;
    const int n_kv_heads = 1;
    const int head_dim = 4;
    const int d_model = n_heads * head_dim;  // 8
    const int kv_dim = n_kv_heads * head_dim;  // 4
    const Tensor wq = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wk = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wv = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wo = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);

    std::vector<float> x = small_weights(seq * d_model);
    std::vector<float> x_perturbed = x;
    for (int col = 0; col < d_model; ++col) {
        x_perturbed[(seq - 1) * d_model + col] += 0.5F;
    }
    const Tensor out = grouped_query_attention(
        Tensor::leaf(x, {seq, d_model}, false), wq, wk, wv, wo, n_heads, n_kv_heads,
        head_dim);
    const Tensor out_perturbed = grouped_query_attention(
        Tensor::leaf(x_perturbed, {seq, d_model}, false), wq, wk, wv, wo, n_heads,
        n_kv_heads, head_dim);

    float first_diff = 0.0F;
    float last_diff = 0.0F;
    for (int col = 0; col < d_model; ++col) {
        first_diff = std::max(
            first_diff, std::abs(out.data()[col] - out_perturbed.data()[col]));
        last_diff = std::max(last_diff,
                             std::abs(out.data()[(seq - 1) * d_model + col] -
                                      out_perturbed.data()[(seq - 1) * d_model + col]));
    }
    EXPECT_LT(first_diff, 1e-5F);  // position 0 cannot see the future
    EXPECT_GT(last_diff, 1e-5F);   // the last position does
}

// Qwen3 QK-norm: the gradient flows to the per-head q_norm weight ([head_dim]),
// through the RMSNorm applied to each query head before RoPE.
TEST(AutogradTransformer, GroupedQueryAttentionQkNormGradientToQNorm) {
    const int seq = 3;
    const int n_heads = 4;
    const int n_kv_heads = 2;
    const int head_dim = 3;
    const int d_model = n_heads * head_dim;  // 12
    const int kv_dim = n_kv_heads * head_dim;  // 6

    const Tensor x = Tensor::leaf(small_weights(seq * d_model), {seq, d_model}, false);
    const Tensor wq = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wk = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wv = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wo = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor k_norm = Tensor::leaf(std::vector<float>(head_dim, 1.0F), {head_dim}, false);
    check_gradient(std::vector<float>{0.9F, 1.0F, 1.1F}, {head_dim},
                   [&](const Tensor& q_norm) {
                       return sum(grouped_query_attention(x, wq, wk, wv, wo, q_norm,
                                                          k_norm, n_heads, n_kv_heads,
                                                          head_dim));
                   });
}

// And to the k_norm weight.
TEST(AutogradTransformer, GroupedQueryAttentionQkNormGradientToKNorm) {
    const int seq = 3;
    const int n_heads = 2;
    const int n_kv_heads = 2;
    const int head_dim = 3;
    const int d_model = n_heads * head_dim;  // 6

    const Tensor x = Tensor::leaf(small_weights(seq * d_model), {seq, d_model}, false);
    const Tensor wq = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wk = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wv = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wo = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor q_norm = Tensor::leaf(std::vector<float>(head_dim, 1.0F), {head_dim}, false);
    check_gradient(std::vector<float>{1.1F, 0.9F, 1.0F}, {head_dim},
                   [&](const Tensor& k_norm) {
                       return sum(grouped_query_attention(x, wq, wk, wv, wo, q_norm,
                                                          k_norm, n_heads, n_kv_heads,
                                                          head_dim));
                   });
}

// QK-norm actually changes the attention output (proves it is applied, not
// silently dropped): the unit-weight normalized variant differs from plain GQA.
TEST(AutogradTransformer, QkNormChangesTheOutput) {
    const int seq = 4;
    const int n_heads = 2;
    const int n_kv_heads = 1;
    const int head_dim = 4;
    const int d_model = n_heads * head_dim;  // 8
    const int kv_dim = n_kv_heads * head_dim;  // 4
    const Tensor x = Tensor::leaf(small_weights(seq * d_model), {seq, d_model}, false);
    const Tensor wq = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor wk = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wv = Tensor::leaf(small_weights(d_model * kv_dim), {d_model, kv_dim}, false);
    const Tensor wo = Tensor::leaf(small_weights(d_model * d_model), {d_model, d_model}, false);
    const Tensor ones = Tensor::leaf(std::vector<float>(head_dim, 1.0F), {head_dim}, false);

    const Tensor plain =
        grouped_query_attention(x, wq, wk, wv, wo, n_heads, n_kv_heads, head_dim);
    const Tensor normed = grouped_query_attention(x, wq, wk, wv, wo, ones, ones,
                                                  n_heads, n_kv_heads, head_dim);
    ASSERT_EQ(plain.shape(), normed.shape());
    float max_diff = 0.0F;
    for (std::size_t i = 0; i < plain.data().size(); ++i) {
        max_diff = std::max(max_diff, std::abs(plain.data()[i] - normed.data()[i]));
    }
    EXPECT_GT(max_diff, 1e-4F);
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

TEST(AutogradTransformer, ForwardProducesFiniteLogits) {
    const Transformer model(small_config(), 42);
    const Tensor logits = model.forward({1, 2, 3, 4, 5});
    ASSERT_EQ(logits.shape(), (std::vector<int>{5, 12}));
    for (float value : logits.data()) {
        EXPECT_TRUE(std::isfinite(value));
    }
}

TEST(AutogradTransformer, ForwardIsDeterministic) {
    const Transformer model(small_config(), 7);
    const Tensor a = model.forward({3, 1, 4, 1, 5});
    const Tensor b = model.forward({3, 1, 4, 1, 5});
    EXPECT_EQ(a.data(), b.data());
}

// THE PAYOFF: our own native autograd + Adam trains the real transformer to
// memorize a sequence. The loss collapses and the model then predicts it greedily.
// No framework — every gradient came from our backward closures.
TEST(AutogradTransformer, OverfitsASequenceWithOurAutograd) {
    Transformer model(small_config(), 3);
    Adam optimizer(/*learning_rate=*/1e-2F);
    const std::vector<int> sequence = {2, 5, 7, 3, 9, 1, 4};
    const std::vector<int> targets(sequence.begin() + 1, sequence.end());
    const int vocab = model.config().vocab_size;

    float first_loss = 0.0F;
    float last_loss = 0.0F;
    for (int step = 0; step < 300; ++step) {
        zero_grad(model.parameters());
        const Tensor logits = model.forward(sequence);
        const Tensor predicted =
            row_slice(logits, 0, static_cast<int>(sequence.size()) - 1);
        Tensor loss = cross_entropy(predicted, targets);
        loss.backward();
        optimizer.step(model.parameters());
        if (step == 0) first_loss = loss.data()[0];
        last_loss = loss.data()[0];
    }
    EXPECT_LT(last_loss, first_loss * 0.2F);  // the loss collapsed

    // The model now predicts the sequence greedily: argmax(logits[t]) == seq[t+1].
    const Tensor logits = model.forward(sequence);
    for (std::size_t t = 0; t + 1 < sequence.size(); ++t) {
        const float* row = logits.data().data() + t * vocab;
        int best = 0;
        for (int c = 1; c < vocab; ++c) {
            if (row[c] > row[best]) best = c;
        }
        EXPECT_EQ(best, sequence[t + 1]) << "position " << t;
    }
}

// RLVR on our own autograd: the transformer learns a verifiable game PURELY FROM
// REWARD, never shown the answer. For prompt [0, q] it must emit answer(q); the
// reward is 1 iff the sampled token is correct. Policy gradient through the whole
// model + GRPO advantages + Adam drive each question's greedy answer to correct.
TEST(AutogradTransformer, LearnsAVerifiableGameByPolicyGradient) {
    Transformer model(small_config(), 5);  // vocab 12
    Adam optimizer(/*learning_rate=*/1e-2F);
    const std::vector<int> questions = {2, 5, 8};
    const auto answer = [](int q) { return q + 2; };  // 4, 7, 10 — in vocab
    const int vocab = model.config().vocab_size;

    std::mt19937 rng(3);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const int group = 24;

    for (int step = 0; step < 400; ++step) {
        const int q = questions[step % questions.size()];
        const std::vector<int> prompt = {0, q};
        const Tensor logits = model.forward(prompt);            // [2, vocab]
        const Tensor last = row_slice(logits, 1, 1);            // [1, vocab]
        const std::vector<double> probs = softmax_row(last.data());

        std::vector<int> actions(group);
        std::vector<double> rewards(group);
        for (int i = 0; i < group; ++i) {
            actions[i] = sample_from(probs, uniform(rng));
            rewards[i] = actions[i] == answer(q) ? 1.0 : 0.0;
        }
        const std::vector<double> advantages = group_advantages(rewards);

        zero_grad(model.parameters());
        Tensor loss = policy_gradient_loss(last, actions, advantages);
        loss.backward();
        optimizer.step(model.parameters());
    }

    for (int q : questions) {
        const Tensor logits = model.forward({0, q});
        const float* row = logits.data().data() + 1 * vocab;  // last position
        int best = 0;
        for (int c = 1; c < vocab; ++c) {
            if (row[c] > row[best]) best = c;
        }
        EXPECT_EQ(best, answer(q)) << "question " << q;
    }
}

}  // namespace
