// SPDX-License-Identifier: MIT

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

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

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

// A realistically-sized native base config (a small-model shape). NOTE: this is
// zetta's native decoder (learned positional embeddings, combined attention
// projections, GELU MLP). Ingesting an actual HuggingFace Qwen checkpoint
// additionally requires matching Qwen's architecture (RoPE, grouped-query
// attention, SwiGLU, QK-norm) so the safetensors tensor names + shapes line up —
// the format loader (safetensors.hpp, incl. BF16) is done; that architecture parity
// is the remaining native-model work.
inline TransformerConfig base_agent_config() {
    TransformerConfig config;
    config.vocab_size = 32000;
    config.d_model = 512;
    config.n_heads = 8;
    config.n_layers = 8;
    config.d_ff = 2048;
    config.max_seq = 2048;
    return config;
}

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

// The full decoder-only transformer. Its parameters are a FLAT, fixed-order list of
// mlx arrays — token embedding, positional embedding, then per layer {attn_norm, wq,
// wk, wv, wo, ff_norm, w1, w2}, then final_norm and the output projection — so the
// whole model can be differentiated at once with mx::value_and_grad (the next rung).
// forward() runs the pre-norm residual stack (embed → N × (attention + MLP) → norm →
// project) and returns logits [seq, vocab_size].
class MlxTransformer {
public:
    explicit MlxTransformer(const TransformerConfig& config,
                            std::uint64_t seed = 0);

    const TransformerConfig& config() const { return config_; }

    // The flat parameter list (the order forward() indexes) — what value_and_grad
    // differentiates and the optimizer updates.
    const std::vector<mlx::core::array>& parameters() const { return parameters_; }
    std::vector<mlx::core::array>& parameters() { return parameters_; }

    // Logits [seq, vocab] for token_ids [seq] (int32), from an explicit parameter
    // list — a pure function of the parameters, so it is differentiable.
    static mlx::core::array forward(const TransformerConfig& config,
                                    const std::vector<mlx::core::array>& parameters,
                                    const mlx::core::array& token_ids);

    // forward() over this model's own parameters.
    mlx::core::array forward(const mlx::core::array& token_ids) const;

    // Autoregressive greedy generation: repeatedly forward the growing sequence and
    // append the argmax next token, up to max_new_tokens (bounded by max_seq).
    // Returns the generated tokens (excluding the prompt) — how the agent emits an
    // action as text, one token at a time. Deterministic (greedy).
    static std::vector<int> generate(const TransformerConfig& config,
                                     const std::vector<mlx::core::array>& parameters,
                                     const std::vector<int>& prompt,
                                     int max_new_tokens);
    std::vector<int> generate(const std::vector<int>& prompt,
                              int max_new_tokens) const;

    // The parameters as named tensors (a canonical name per tensor) — the view a
    // checkpoint is written from and read into.
    std::map<std::string, mlx::core::array> named_parameters() const;

    // Load parameters by name, validating every expected tensor is present and
    // shape-matches. Returns false (leaving the model unchanged) on any missing or
    // mismatched tensor — a non-throwing boundary for untrusted checkpoints.
    bool load_named_parameters(
        const std::map<std::string, mlx::core::array>& named);

    // Checkpoint I/O via safetensors (the HuggingFace format).
    bool save_checkpoint(const std::string& path) const;
    bool load_checkpoint(const std::string& path);

private:
    TransformerConfig config_;
    std::vector<mlx::core::array> parameters_;
};

// The next-token cross-entropy loss (a scalar): predict token[t+1] from position t
// across the sequence. It is a pure function of the parameters, so mx::value_and_grad
// differentiates it end to end — the proof that real backprop trains the attention
// and MLP weights. (The RLVR objective in the next rung is the same forward pass with
// a policy-gradient loss in place of cross-entropy.)
mlx::core::array cross_entropy_next_token(
    const TransformerConfig& config,
    const std::vector<mlx::core::array>& parameters,
    const mlx::core::array& token_ids);

// The RLVR policy-gradient loss for one prompt: -mean(advantage * log pi(action)),
// where pi is the softmax of the model's logits at the LAST position and the
// actions were sampled from a prior forward pass (with their group-relative
// advantages). A pure function of the parameters, so mx::value_and_grad
// differentiates it end to end — the #555 objective through the real transformer.
// No supervised target: the model learns only from the verifiable reward that fed
// the advantages.
mlx::core::array policy_gradient_loss(
    const TransformerConfig& config,
    const std::vector<mlx::core::array>& parameters, const mlx::core::array& prompt,
    const std::vector<int>& sampled_actions,
    const std::vector<double>& advantages);

// AdamW over the transformer's flat parameter list. update() applies one step in
// place, maintaining the first and second moment per parameter with bias correction
// and decoupled weight decay.
class AdamW {
public:
    explicit AdamW(float learning_rate = 1e-3F, float beta1 = 0.9F,
                   float beta2 = 0.999F, float epsilon = 1e-8F,
                   float weight_decay = 0.0F);

    void update(std::vector<mlx::core::array>& parameters,
                const std::vector<mlx::core::array>& gradients);

private:
    float learning_rate_;
    float beta1_;
    float beta2_;
    float epsilon_;
    float weight_decay_;
    int step_ = 0;
    std::vector<mlx::core::array> first_moment_;
    std::vector<mlx::core::array> second_moment_;
};

// Flatten a list of arrays into one contiguous host vector (each array row-major, in
// order), and split a flat vector back into arrays matching a template's shapes.
// These bridge the model's parameter/gradient tensors to the cluster's vector-based
// allreduce.
std::vector<double> flatten_arrays(const std::vector<mlx::core::array>& arrays);
std::vector<mlx::core::array> unflatten_like(
    const std::vector<double>& flat,
    const std::vector<mlx::core::array>& shape_template);

// The cross-node gradient combine (sum across nodes). Identity for a single node; a
// Collectives-backed sum for the cluster — the same allreduce the rest of the RLVR
// loop uses, over the flattened transformer gradient.
using GradientAllReduce =
    std::function<std::vector<double>(const std::vector<double>&)>;

// One DISTRIBUTED AdamW step: flatten `gradients`, sum them across the cluster via
// `allreduce`, unflatten, and apply. With an identity allreduce this is a plain
// AdamW step; with a summing allreduce it is data-parallel distributed training of
// the transformer — every node applies the same global gradient, so the model stays
// identical across the cluster (single-node == multi-node).
void distributed_adamw_step(std::vector<mlx::core::array>& parameters,
                            const std::vector<mlx::core::array>& gradients,
                            AdamW& optimizer, const GradientAllReduce& allreduce);

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
