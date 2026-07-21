#include "zetta/autograd/transformer.hpp"

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

Tensor feed_forward(const Tensor& x, const Tensor& w1, const Tensor& w2) {
    return matmul(gelu(matmul(x, w1)), w2);
}

}  // namespace zetta::autograd
