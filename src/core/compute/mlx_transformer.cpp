#include "zetta/compute/mlx_transformer.hpp"

#if defined(ZETTA_WITH_MLX)

#include <cmath>
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

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
