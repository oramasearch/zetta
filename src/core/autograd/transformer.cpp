#include "zetta/autograd/transformer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace zetta::autograd {

namespace {

// Index where layer `l`'s eight tensors begin (token + positional embedding are 0,1).
int layer_base(int layer) { return 2 + 8 * layer; }

}  // namespace

Transformer::Transformer(const TransformerConfig& config, std::uint64_t seed)
    : config_(config) {
    std::mt19937 rng(static_cast<unsigned>(seed) + 1);
    std::normal_distribution<float> normal(0.0F, 0.02F);
    const int d = config.d_model;

    const auto random_matrix = [&](int rows, int cols) {
        std::vector<float> values(static_cast<std::size_t>(rows) * cols, 0.0F);
        for (float& value : values) {
            value = normal(rng);
        }
        return Tensor::leaf(std::move(values), {rows, cols}, true);
    };
    const auto ones_vector = [&](int size) {
        return Tensor::leaf(std::vector<float>(size, 1.0F), {size}, true);
    };

    parameters_.push_back(random_matrix(config.vocab_size, d));  // token embedding
    parameters_.push_back(random_matrix(config.max_seq, d));     // positional
    for (int layer = 0; layer < config.n_layers; ++layer) {
        parameters_.push_back(ones_vector(d));                   // attn_norm
        parameters_.push_back(random_matrix(d, d));              // wq
        parameters_.push_back(random_matrix(d, d));              // wk
        parameters_.push_back(random_matrix(d, d));              // wv
        parameters_.push_back(random_matrix(d, d));              // wo
        parameters_.push_back(ones_vector(d));                   // ff_norm
        parameters_.push_back(random_matrix(d, config.d_ff));    // w1
        parameters_.push_back(random_matrix(config.d_ff, d));    // w2
    }
    parameters_.push_back(ones_vector(d));                       // final_norm
    parameters_.push_back(random_matrix(d, config.vocab_size));  // output proj
}

Tensor Transformer::forward(const std::vector<int>& ids) const {
    const int seq = static_cast<int>(ids.size());
    std::vector<int> positions(seq, 0);
    for (int i = 0; i < seq; ++i) {
        positions[i] = i;
    }

    // Embed tokens + positions.
    Tensor h = add(embedding(parameters_[0], ids),
                   embedding(parameters_[1], positions));

    // Pre-norm residual blocks.
    for (int layer = 0; layer < config_.n_layers; ++layer) {
        const int base = layer_base(layer);
        h = add(h, causal_self_attention(rms_norm(h, parameters_[base + 0]),
                                         parameters_[base + 1], parameters_[base + 2],
                                         parameters_[base + 3], parameters_[base + 4]));
        h = add(h, feed_forward(rms_norm(h, parameters_[base + 5]),
                                parameters_[base + 6], parameters_[base + 7]));
    }

    const int final_norm = layer_base(config_.n_layers);
    h = rms_norm(h, parameters_[final_norm]);
    return matmul(h, parameters_[final_norm + 1]);  // logits [seq, vocab]
}

Tensor causal_self_attention(const Tensor& x, const Tensor& wq, const Tensor& wk,
                             const Tensor& wv, const Tensor& wo) {
    const int seq = x.shape()[0];
    const int d_model = x.shape()[1];
    const float scale_factor = 1.0F / std::sqrt(static_cast<float>(d_model));

    const Tensor query = matmul(x, wq);  // [seq, d]
    const Tensor key = matmul(x, wk);
    const Tensor value = matmul(x, wv);

    // scores [seq, seq] = (Q K^T) / sqrt(d), then an additive causal mask so a
    // position cannot attend to the future.
    Tensor scores = scale(matmul(query, transpose(key)), scale_factor);
    std::vector<float> mask(static_cast<std::size_t>(seq) * seq, 0.0F);
    for (int i = 0; i < seq; ++i) {
        for (int j = i + 1; j < seq; ++j) {
            mask[i * seq + j] = -1e9F;
        }
    }
    scores = add(scores, Tensor::leaf(std::move(mask), {seq, seq}, false));

    const Tensor attention = row_softmax(scores);  // [seq, seq]
    const Tensor context = matmul(attention, value);  // [seq, d]
    return matmul(context, wo);
}

namespace {

// The additive causal mask [seq, seq]: 0 on/below the diagonal, -inf above, so
// after the softmax a position places no weight on the future.
Tensor causal_mask(int seq) {
    std::vector<float> mask(static_cast<std::size_t>(seq) * seq, 0.0F);
    for (int i = 0; i < seq; ++i) {
        for (int j = i + 1; j < seq; ++j) {
            mask[i * seq + j] = -1e9F;
        }
    }
    return Tensor::leaf(std::move(mask), {seq, seq}, false);
}

// Scaled-dot-product attention for one head: q, k, v are [seq, head_dim].
// softmax((q kᵀ)/sqrt(head_dim) + causal_mask) · v -> [seq, head_dim].
Tensor attend_one_head(const Tensor& q, const Tensor& k, const Tensor& v,
                       const Tensor& mask, float scale_factor) {
    Tensor scores = scale(matmul(q, transpose(k)), scale_factor);
    scores = add(scores, mask);
    return matmul(row_softmax(scores), v);
}

// One head's projection band, optionally RMSNorm'd (QK-norm) then RoPE-rotated.
Tensor head_qk(const Tensor& projection, int head, int head_dim,
               const Tensor& norm_weight, float rope_base) {
    Tensor band = col_slice(projection, head * head_dim, head_dim);
    if (norm_weight.valid()) {
        band = rms_norm(band, norm_weight);  // per-head QK-norm (Qwen3)
    }
    return rope(band, rope_base);
}

// Shared implementation. q_norm / k_norm are [head_dim] when QK-norm is used, or
// invalid (default-constructed) Tensors to skip it.
Tensor grouped_query_attention_impl(const Tensor& x, const Tensor& wq,
                                    const Tensor& wk, const Tensor& wv,
                                    const Tensor& wo, const Tensor& q_norm,
                                    const Tensor& k_norm, int n_heads,
                                    int n_kv_heads, int head_dim, float rope_base) {
    const int seq = x.shape()[0];
    const int group_size = n_heads / n_kv_heads;  // query heads per kv head
    const float scale_factor = 1.0F / std::sqrt(static_cast<float>(head_dim));

    // Weights are Hugging Face layout [out, in]; matmul_transposed_b computes x·Wᵀ
    // without materializing transpose(W) (the real-checkpoint inference win).
    const Tensor query = matmul_transposed_b(x, wq);  // [seq, n_heads    * head_dim]
    const Tensor key = matmul_transposed_b(x, wk);    // [seq, n_kv_heads * head_dim]
    const Tensor value = matmul_transposed_b(x, wv);  // [seq, n_kv_heads * head_dim]
    const Tensor mask = causal_mask(seq);

    std::vector<Tensor> head_outputs;
    head_outputs.reserve(n_heads);
    for (int head = 0; head < n_heads; ++head) {
        const int kv_head = head / group_size;
        const Tensor q_head = head_qk(query, head, head_dim, q_norm, rope_base);
        const Tensor k_head = head_qk(key, kv_head, head_dim, k_norm, rope_base);
        const Tensor v_head = col_slice(value, kv_head * head_dim, head_dim);
        head_outputs.push_back(
            attend_one_head(q_head, k_head, v_head, mask, scale_factor));
    }

    const Tensor context = concat_cols(head_outputs);  // [seq, n_heads*head_dim]
    return matmul_transposed_b(context, wo);  // wo [d_model, n_heads*head_dim]
}

}  // namespace

Tensor grouped_query_attention(const Tensor& x, const Tensor& wq, const Tensor& wk,
                               const Tensor& wv, const Tensor& wo, int n_heads,
                               int n_kv_heads, int head_dim, float rope_base) {
    return grouped_query_attention_impl(x, wq, wk, wv, wo, /*q_norm=*/Tensor(),
                                        /*k_norm=*/Tensor(), n_heads, n_kv_heads,
                                        head_dim, rope_base);
}

Tensor grouped_query_attention(const Tensor& x, const Tensor& wq, const Tensor& wk,
                               const Tensor& wv, const Tensor& wo,
                               const Tensor& q_norm, const Tensor& k_norm,
                               int n_heads, int n_kv_heads, int head_dim,
                               float rope_base) {
    return grouped_query_attention_impl(x, wq, wk, wv, wo, q_norm, k_norm, n_heads,
                                        n_kv_heads, head_dim, rope_base);
}

Tensor feed_forward(const Tensor& x, const Tensor& w1, const Tensor& w2) {
    return matmul(gelu(matmul(x, w1)), w2);
}

Tensor swiglu_feed_forward(const Tensor& x, const Tensor& w_gate,
                           const Tensor& w_up, const Tensor& w_down) {
    // Weights are Hugging Face layout [out, in]; matmul_transposed_b = x·Wᵀ.
    const Tensor gate = silu(matmul_transposed_b(x, w_gate));  // w_gate [d_ff, d_model]
    const Tensor up = matmul_transposed_b(x, w_up);            // w_up   [d_ff, d_model]
    return matmul_transposed_b(mul(gate, up), w_down);         // w_down [d_model, d_ff]
}

void zero_grad(std::vector<Tensor>& parameters) {
    for (Tensor& parameter : parameters) {
        std::vector<float>& grad = parameter.node()->grad;
        std::fill(grad.begin(), grad.end(), 0.0F);
    }
}

Adam::Adam(float learning_rate, float beta1, float beta2, float epsilon)
    : learning_rate_(learning_rate),
      beta1_(beta1),
      beta2_(beta2),
      epsilon_(epsilon) {}

void Adam::step(std::vector<Tensor>& parameters) {
    if (first_moment_.empty()) {
        for (const Tensor& parameter : parameters) {
            first_moment_.emplace_back(parameter.data().size(), 0.0F);
            second_moment_.emplace_back(parameter.data().size(), 0.0F);
        }
    }
    ++step_count_;
    const float bias1 = 1.0F - std::pow(beta1_, step_count_);
    const float bias2 = 1.0F - std::pow(beta2_, step_count_);

    for (std::size_t i = 0; i < parameters.size(); ++i) {
        std::vector<float>& data = parameters[i].node()->data;
        const std::vector<float>& grad = parameters[i].node()->grad;
        for (std::size_t j = 0; j < data.size(); ++j) {
            first_moment_[i][j] =
                beta1_ * first_moment_[i][j] + (1.0F - beta1_) * grad[j];
            second_moment_[i][j] =
                beta2_ * second_moment_[i][j] + (1.0F - beta2_) * grad[j] * grad[j];
            const float m_hat = first_moment_[i][j] / bias1;
            const float v_hat = second_moment_[i][j] / bias2;
            data[j] -= learning_rate_ * m_hat / (std::sqrt(v_hat) + epsilon_);
        }
    }
}

}  // namespace zetta::autograd
