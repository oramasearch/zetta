#pragma once

// A native transformer built on MLX (#555; docs/agent/README.md). This is the real
// model runtime the RLVR loop trains — attention and MLP weights, not a single
// linear layer — authored in native C++ over MLX's array ops + autodiff, running on
// the CPU or the Metal GPU. MLX is the tensor library + autodiff engine (the compute
// backend, the way a BLAS is); the architecture is ours.
//
// This header is MLX-aware and exists ONLY in the MLX build (guarded by
// ZETTA_WITH_MLX). It is never included by zetta-core — the model runtime lives in
// zetta-compute, where MLX is linked, exactly like mlx_device.cpp. The building
// blocks are free functions over mlx::core::array so each is unit-testable on its
// own before the full stack composes them.
#if defined(ZETTA_WITH_MLX)

#include <mlx/mlx.h>

namespace zetta::compute::model {

struct TransformerConfig {
    int vocab_size = 32;
    int d_model = 32;
    int n_heads = 4;
    int n_layers = 2;
    int d_ff = 64;
    int max_seq = 16;

    int head_dim() const { return d_model / n_heads; }
};

// RMSNorm: normalize each row of `x` [seq, d_model] to unit RMS, then scale by
// `weight` [d_model].
mlx::core::array rms_norm(const mlx::core::array& x,
                          const mlx::core::array& weight, float eps = 1e-5F);

// GELU (the exact erf form): 0.5 * x * (1 + erf(x / sqrt(2))).
mlx::core::array gelu(const mlx::core::array& x);

// Position-wise feed-forward: gelu(x @ w1) @ w2. x [seq, d_model],
// w1 [d_model, d_ff], w2 [d_ff, d_model].
mlx::core::array feed_forward(const mlx::core::array& x,
                              const mlx::core::array& w1,
                              const mlx::core::array& w2);

// Multi-head CAUSAL self-attention over `x` [seq, d_model]. wq/wk/wv/wo are each
// [d_model, d_model], row-major. Position t attends only to positions <= t (the
// causal mask), so the model is a valid autoregressive decoder.
mlx::core::array causal_self_attention(const mlx::core::array& x,
                                       const mlx::core::array& wq,
                                       const mlx::core::array& wk,
                                       const mlx::core::array& wv,
                                       const mlx::core::array& wo, int n_heads);

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
