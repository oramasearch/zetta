#include "zetta/compute/mlx_transformer.hpp"

#if defined(ZETTA_WITH_MLX)

#include <cmath>
#include <cstdint>
#include <vector>

namespace zetta::compute::model {

namespace mx = mlx::core;

mx::array rms_norm(const mx::array& x, const mx::array& weight, float eps) {
    // mean of squares over the last axis, keeping dims for the broadcast divide.
    const mx::array mean_square =
        mx::mean(mx::multiply(x, x), std::vector<int>{-1}, /*keepdims=*/true);
    const mx::array normalized =
        mx::multiply(x, mx::rsqrt(mean_square + eps));
    return mx::multiply(normalized, weight);
}

mx::array gelu(const mx::array& x) {
    return 0.5F * x * (1.0F + mx::erf(x * (1.0F / std::sqrt(2.0F))));
}

mx::array feed_forward(const mx::array& x, const mx::array& w1,
                       const mx::array& w2) {
    return mx::matmul(gelu(mx::matmul(x, w1)), w2);
}

mx::array causal_self_attention(const mx::array& x, const mx::array& wq,
                                const mx::array& wk, const mx::array& wv,
                                const mx::array& wo, int n_heads) {
    const int seq = x.shape(0);
    const int d_model = x.shape(1);
    const int head_dim = d_model / n_heads;
    const float scale = 1.0F / std::sqrt(static_cast<float>(head_dim));

    // Project and split into heads: [seq, d_model] -> [n_heads, seq, head_dim].
    const auto project = [&](const mx::array& weight) {
        mx::array projected = mx::matmul(x, weight);
        projected = mx::reshape(projected, mx::Shape{seq, n_heads, head_dim});
        return mx::transpose(projected, std::vector<int>{1, 0, 2});
    };
    const mx::array query = project(wq);
    const mx::array key = project(wk);
    const mx::array value = project(wv);

    // scores [n_heads, seq, seq] = (Q @ K^T) * scale.
    mx::array scores =
        mx::matmul(query, mx::transpose(key, std::vector<int>{0, 2, 1})) * scale;

    // Additive causal mask: 0 where j <= i, -1e9 above the diagonal. tril(ones) is 1
    // on/below the diagonal; (mask - 1) * 1e9 is 0 there and -1e9 above. Broadcasts
    // over the heads dimension.
    const mx::array lower =
        mx::tril(mx::ones(mx::Shape{seq, seq}, mx::float32));
    scores = scores + (lower - 1.0F) * 1e9F;

    const mx::array attention = mx::softmax(scores, std::vector<int>{-1});
    mx::array context = mx::matmul(attention, value);  // [n_heads, seq, head_dim]
    context = mx::transpose(context, std::vector<int>{1, 0, 2});  // [seq, n_heads, hd]
    context = mx::reshape(context, mx::Shape{seq, d_model});
    return mx::matmul(context, wo);
}

namespace {

// Random init for a 2-D parameter, scaled small (like a transformer's).
mx::array init_matrix(int rows, int cols, std::uint64_t seed) {
    return mx::random::normal(mx::Shape{rows, cols}, mx::random::key(seed)) * 0.02F;
}

// The parameter index where layer `l`'s eight tensors begin (token + positional
// embedding occupy 0 and 1).
int layer_base(int layer) { return 2 + 8 * layer; }

}  // namespace

MlxTransformer::MlxTransformer(const TransformerConfig& config,
                              std::uint64_t seed)
    : config_(config) {
    const int d = config.d_model;
    std::uint64_t next = seed + 1;

    parameters_.push_back(init_matrix(config.vocab_size, d, next++));  // token emb
    parameters_.push_back(init_matrix(config.max_seq, d, next++));     // positional
    for (int layer = 0; layer < config.n_layers; ++layer) {
        parameters_.push_back(mx::ones(mx::Shape{d}, mx::float32));    // attn_norm
        parameters_.push_back(init_matrix(d, d, next++));             // wq
        parameters_.push_back(init_matrix(d, d, next++));             // wk
        parameters_.push_back(init_matrix(d, d, next++));             // wv
        parameters_.push_back(init_matrix(d, d, next++));             // wo
        parameters_.push_back(mx::ones(mx::Shape{d}, mx::float32));    // ff_norm
        parameters_.push_back(init_matrix(d, config.d_ff, next++));   // w1
        parameters_.push_back(init_matrix(config.d_ff, d, next++));   // w2
    }
    parameters_.push_back(mx::ones(mx::Shape{d}, mx::float32));        // final_norm
    parameters_.push_back(init_matrix(d, config.vocab_size, next++)); // output proj
}

mx::array MlxTransformer::forward(const TransformerConfig& config,
                                 const std::vector<mx::array>& parameters,
                                 const mx::array& token_ids) {
    const int seq = token_ids.shape(0);
    const mx::array positions = mx::astype(mx::arange(seq), mx::int32);

    // Embed tokens + positions.
    mx::array x = mx::take(parameters[0], token_ids, 0) +
                  mx::take(parameters[1], positions, 0);

    // Pre-norm residual blocks: attention then feed-forward.
    for (int layer = 0; layer < config.n_layers; ++layer) {
        const int base = layer_base(layer);
        x = x + causal_self_attention(rms_norm(x, parameters[base + 0]),
                                      parameters[base + 1], parameters[base + 2],
                                      parameters[base + 3], parameters[base + 4],
                                      config.n_heads);
        x = x + feed_forward(rms_norm(x, parameters[base + 5]),
                             parameters[base + 6], parameters[base + 7]);
    }

    const int final_norm = layer_base(config.n_layers);
    x = rms_norm(x, parameters[final_norm]);
    return mx::matmul(x, parameters[final_norm + 1]);  // logits [seq, vocab]
}

mx::array MlxTransformer::forward(const mx::array& token_ids) const {
    return forward(config_, parameters_, token_ids);
}

mx::array cross_entropy_next_token(const TransformerConfig& config,
                                   const std::vector<mx::array>& parameters,
                                   const mx::array& token_ids) {
    const int seq = token_ids.shape(0);
    const mx::array logits = MlxTransformer::forward(config, parameters, token_ids);

    // Predict token[t+1] from position t: logits[0..seq-2] against targets[1..seq-1].
    const mx::array predicted =
        mx::slice(logits, mx::Shape{0, 0}, mx::Shape{seq - 1, config.vocab_size});
    const mx::array targets =
        mx::slice(token_ids, mx::Shape{1}, mx::Shape{seq});  // [seq-1] int32

    // log-softmax, then gather the log-probability of each target token.
    const mx::array log_probs =
        predicted - mx::logsumexp(predicted, std::vector<int>{-1}, true);
    const mx::array target_index = mx::reshape(targets, mx::Shape{seq - 1, 1});
    const mx::array chosen = mx::take_along_axis(log_probs, target_index, 1);
    return -mx::mean(chosen, /*keepdims=*/false);
}

AdamW::AdamW(float learning_rate, float beta1, float beta2, float epsilon,
             float weight_decay)
    : learning_rate_(learning_rate),
      beta1_(beta1),
      beta2_(beta2),
      epsilon_(epsilon),
      weight_decay_(weight_decay) {}

void AdamW::update(std::vector<mx::array>& parameters,
                   const std::vector<mx::array>& gradients) {
    if (first_moment_.empty()) {
        for (const mx::array& parameter : parameters) {
            first_moment_.push_back(
                mx::zeros(parameter.shape(), parameter.dtype()));
            second_moment_.push_back(
                mx::zeros(parameter.shape(), parameter.dtype()));
        }
    }
    ++step_;
    const float bias_correction1 = 1.0F - std::pow(beta1_, step_);
    const float bias_correction2 = 1.0F - std::pow(beta2_, step_);

    for (std::size_t i = 0; i < parameters.size(); ++i) {
        first_moment_[i] =
            beta1_ * first_moment_[i] + (1.0F - beta1_) * gradients[i];
        second_moment_[i] = beta2_ * second_moment_[i] +
                            (1.0F - beta2_) * mx::square(gradients[i]);
        const mx::array corrected_first = first_moment_[i] / bias_correction1;
        const mx::array corrected_second = second_moment_[i] / bias_correction2;
        const mx::array step =
            corrected_first / (mx::sqrt(corrected_second) + epsilon_);
        parameters[i] = parameters[i] -
                        learning_rate_ * (step + weight_decay_ * parameters[i]);
    }
    // Realize the parameters and moments so the graph does not grow across steps.
    mx::eval(parameters);
    mx::eval(first_moment_);
    mx::eval(second_moment_);
}

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
