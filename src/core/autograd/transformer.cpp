#include "zetta/autograd/transformer.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace zetta::autograd {

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
