// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <vector>

#include "zetta/autograd/tensor.hpp"

// The transformer, built on zetta's own autograd (#555; docs/autograd/README.md).
// These blocks compose the primitive ops (matmul, transpose, scale, add,
// row_softmax, gelu, rms_norm) into the pieces of a decoder, so training the model
// is just building the graph forward and calling backward() — no framework. The
// heavy op (matmul) routes through the ComputeDevice seam in a later slice, making
// the same model run on CpuDevice / CudaDevice / MlxDevice.
namespace zetta::autograd {

// Single-head causal self-attention over `x` [seq, d_model]. wq/wk/wv/wo are each
// [d_model, d_model]. Position t attends only to positions <= t (an additive causal
// mask before the softmax), so the block is a valid autoregressive decoder.
Tensor causal_self_attention(const Tensor& x, const Tensor& wq, const Tensor& wk,
                             const Tensor& wv, const Tensor& wo);

// Grouped-query causal self-attention (the Qwen/LLaMA attention). `x` is
// [seq, d_model]; the projection weights are **Hugging Face layout `[out, in]`**
// (applied as x·Wᵀ via matmul_transposed_b, so no transpose is materialized):
//   wq [n_heads    * head_dim, d_model]
//   wk [n_kv_heads * head_dim, d_model]
//   wv [n_kv_heads * head_dim, d_model]
//   wo [d_model, n_heads * head_dim]
// Each of the n_heads query heads shares a key/value head with its group
// (group_size = n_heads / n_kv_heads), so n_kv_heads == n_heads is plain
// multi-head attention and n_kv_heads == 1 is multi-query. RoPE is applied
// per head to Q and K, each head does scaled-dot-product attention under the
// causal mask, and the head outputs are concatenated and projected by wo.
// Requires n_heads % n_kv_heads == 0. `rope_base` is the RoPE θ (10000 for
// GPT/LLaMA-1, 1000000 for Qwen3).
Tensor grouped_query_attention(const Tensor& x, const Tensor& wq, const Tensor& wk,
                               const Tensor& wv, const Tensor& wo, int n_heads,
                               int n_kv_heads, int head_dim,
                               float rope_base = 10000.0F);

// Grouped-query attention with QK-norm (the Qwen3 variant): before RoPE, each
// head's query and key vector is RMSNorm'd by a learned weight — q_norm and
// k_norm are both [head_dim], shared across heads (Qwen3's self_attn.q_norm /
// self_attn.k_norm). This stabilizes attention logits. Otherwise identical to
// the plain grouped_query_attention above.
Tensor grouped_query_attention(const Tensor& x, const Tensor& wq, const Tensor& wk,
                               const Tensor& wv, const Tensor& wo,
                               const Tensor& q_norm, const Tensor& k_norm,
                               int n_heads, int n_kv_heads, int head_dim,
                               float rope_base = 10000.0F);

// Position-wise feed-forward: gelu(x @ w1) @ w2. x [seq, d_model],
// w1 [d_model, d_ff], w2 [d_ff, d_model].
Tensor feed_forward(const Tensor& x, const Tensor& w1, const Tensor& w2);

// SwiGLU feed-forward (the Qwen/LLaMA MLP): (silu(x·w_gateᵀ) ⊙ (x·w_upᵀ)) · w_downᵀ.
// Weights are **Hugging Face layout `[out, in]`** (applied via matmul_transposed_b):
// w_gate, w_up are [d_ff, d_model]; w_down is [d_model, d_ff].
Tensor swiglu_feed_forward(const Tensor& x, const Tensor& w_gate,
                           const Tensor& w_up, const Tensor& w_down);

struct TransformerConfig {
    int vocab_size = 16;
    int d_model = 16;
    int n_layers = 2;
    int d_ff = 32;
    int max_seq = 16;
};

// A single-head decoder-only transformer built entirely on the autograd ops. Its
// parameters are a flat list of Tensor leaves — token + positional embeddings, then
// per layer {attn_norm, wq, wk, wv, wo, ff_norm, w1, w2}, then final_norm and the
// output projection — so training is: forward(ids) -> loss -> backward() -> step the
// leaves. forward() runs the pre-norm residual stack and returns logits [seq, vocab].
class Transformer {
public:
    explicit Transformer(const TransformerConfig& config, std::uint64_t seed = 0);

    const TransformerConfig& config() const { return config_; }
    std::vector<Tensor>& parameters() { return parameters_; }
    const std::vector<Tensor>& parameters() const { return parameters_; }

    // Logits [seq, vocab] for the token id sequence.
    Tensor forward(const std::vector<int>& ids) const;

private:
    TransformerConfig config_;
    std::vector<Tensor> parameters_;
};

// Zero the gradient of every parameter — call before each backward() so gradients
// do not accumulate across training steps.
void zero_grad(std::vector<Tensor>& parameters);

// Adam over autograd Tensor leaves: step() updates each parameter in place from its
// accumulated gradient, maintaining per-parameter first and second moments with
// bias correction.
class Adam {
public:
    explicit Adam(float learning_rate = 1e-3F, float beta1 = 0.9F,
                  float beta2 = 0.999F, float epsilon = 1e-8F);
    void step(std::vector<Tensor>& parameters);
    float learning_rate() const noexcept { return learning_rate_; }

private:
    float learning_rate_;
    float beta1_;
    float beta2_;
    float epsilon_;
    int step_count_ = 0;
    std::vector<std::vector<float>> first_moment_;
    std::vector<std::vector<float>> second_moment_;
};

}  // namespace zetta::autograd
