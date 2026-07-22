#pragma once

// A FUSED, native-MLX Qwen3 inference kernel (#555; docs/autograd/). The autograd
// path routes each op through the ComputeDevice with a per-op host<->device sync,
// which kills GPU generation (per-token forwards drown in sync). This is the
// opposite: the whole decode step is ONE MLX graph, the KV cache lives in MLX
// (unified-memory) arrays between steps, and there is a single eval per token — no
// per-op sync. It is the fast Metal-GPU path for autoregressive inference and RLVR
// rollouts. Correctness is gated against the CPU QwenInference (same logits).
//
// MLX-only: this header exists only in the MLX build (ZETTA_WITH_MLX) and lives in
// zetta-compute where MLX is linked, never in zetta-core (like mlx_transformer).
#if defined(ZETTA_WITH_MLX)

#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <mlx/mlx.h>

#include "zetta/compute/mlx_transformer.hpp"  // AdamW

namespace zetta::compute::model {

// Qwen3 architecture hyperparameters (read from the checkpoint's config.json).
struct QwenParams {
    int vocab_size = 0;
    int d_model = 0;
    int n_layers = 0;
    int n_heads = 0;
    int n_kv_heads = 0;
    int head_dim = 0;
    int d_ff = 0;
    float rms_eps = 1e-6F;
    float rope_base = 1000000.0F;
    bool tie_word_embeddings = false;
};

class MlxQwenInference {
public:
    // Load config.json + model.safetensors from a checkpoint directory into MLX
    // arrays. std::nullopt (with a reason) if a file is missing/malformed.
    static std::optional<MlxQwenInference> from_directory(const std::string& directory,
                                                          std::string* error = nullptr);

    // Clear the KV cache and reset to position 0.
    void reset();

    // Forward `tokens` at the current position (a whole prompt for prefill, or one
    // token to decode): append their key/value to the cache and return the LAST
    // token's logits [vocab] as host floats. One MLX eval; the cache stays on-GPU.
    std::vector<float> forward(const std::vector<int>& tokens);

    // Greedy decode with the cache (prefill prompt, then step on the argmax). Same
    // result as the CPU QwenInference, on the GPU.
    std::vector<int> generate_greedy(const std::vector<int>& prompt, int max_new_tokens,
                                     int eos_id = -1);

    const QwenParams& config() const { return config_; }

private:
    MlxQwenInference(QwenParams config,
                     std::map<std::string, mlx::core::array> weights,
                     mlx::core::array inv_freq);

    QwenParams config_;
    std::map<std::string, mlx::core::array> weights_;  // HF-named, F32 MLX arrays
    std::vector<mlx::core::array> key_cache_;          // per layer [pos, n_kv, head_dim]
    std::vector<mlx::core::array> value_cache_;
    mlx::core::array inv_freq_;                        // [head_dim/2] RoPE frequencies
    int position_ = 0;
};

// Fused MLX Qwen TRAINING: a differentiable full-sequence forward whose parameters
// are MLX arrays, so mx::value_and_grad backprops through the whole Qwen3 stack in
// one graph — one eval per step — and AdamW updates on the GPU. The training analog
// of MlxQwenInference (GPU fine-tuning / RLVR), the same ~6× batched-forward win
// applied to the gradient step. Parameters are ordered [embed, per-layer x11,
// final norm, (lm_head)] — the autograd QwenModel order — so a checkpoint loads.
class MlxQwenModel {
public:
    static std::optional<MlxQwenModel> from_directory(const std::string& directory,
                                                      std::string* error = nullptr);

    std::vector<mlx::core::array>& parameters() { return params_; }
    const std::vector<mlx::core::array>& parameters() const { return params_; }
    const QwenParams& config() const { return config_; }

    // Full-sequence logits [seq, vocab], differentiable through `params`.
    mlx::core::array forward(const std::vector<mlx::core::array>& params,
                             const mlx::core::array& token_ids) const;

    // Next-token cross-entropy over `token_ids` (a scalar), differentiable through
    // `params` — hand this to mx::value_and_grad, then AdamW::update the result.
    mlx::core::array next_token_loss(const std::vector<mlx::core::array>& params,
                                     const mlx::core::array& token_ids) const;

    // KV-cached sampling generation over the CURRENT parameters (fast GPU rollouts):
    // each next token is drawn from softmax(logits / temperature) with `rng`
    // (temperature <= 0 is greedy). One MLX eval per token, cache in GPU arrays.
    std::vector<int> generate(const std::vector<int>& prompt, int max_new_tokens,
                              float temperature, std::mt19937& rng, int eos_id = -1);

    // Verifiable reward over a completion: (prompt ids, completion ids) -> reward.
    using CompletionReward =
        std::function<double(const std::vector<int>&, const std::vector<int>&)>;

    // THE FULLY-FUSED GPU RLVR STEP: roll out `group_size` completions with the
    // KV-cached inference (fast), score each with `reward`, turn the group's rewards
    // into group-relative advantages, and take ONE sequence policy-gradient step —
    // mx::value_and_grad over the whole group in a single MLX graph, then AdamW. The
    // parameters are shared, so the update flows straight into the next round's
    // rollouts. Returns the group's mean reward. This is blender-rl's GRPO, fully on
    // the Metal GPU. Stable sustained RLVR comes from a moderate learning rate — an
    // aggressive lr collapses the policy into reward-hacking repetition (see the
    // validation report); a reference-KL penalty was explored and did not help on this
    // scale (the dense form fights Adam's per-coordinate normalization).
    double grpo_step(AdamW& optimizer, const std::vector<int>& prompt,
                     const CompletionReward& reward, int group_size,
                     int max_new_tokens, float temperature, std::mt19937& rng);

    // Save the (possibly fine-tuned) parameters as a loadable checkpoint: writes
    // model.safetensors from `params_` (re-attaching the HF tensor names) into `out_dir`
    // and copies config.json + the tokenizer files from `source_dir`, so
    // `from_directory(out_dir)` round-trips a trained model. Non-throwing.
    bool save(const std::string& out_dir, const std::string& source_dir,
              std::string* error = nullptr) const;

private:
    // The HF tensor names in `params_` order (the inverse of from_directory's load order).
    std::vector<std::string> param_names() const;

    MlxQwenModel(QwenParams config, std::vector<mlx::core::array> params,
                 mlx::core::array inv_freq);

    // One KV-cached decode step over params_ at cache_pos_: returns the logits.
    std::vector<float> cached_step(int token_id);
    // Advantage-weighted sum of the completion's token log-probs (a scalar),
    // differentiable through `params`. tokens = prompt+completion, prompt_len = P.
    mlx::core::array sequence_pg_loss(const std::vector<mlx::core::array>& params,
                                      const std::vector<int>& tokens, int prompt_len,
                                      const std::vector<int>& actions, double advantage) const;

    QwenParams config_;
    std::vector<mlx::core::array> params_;  // ordered; see the class comment
    mlx::core::array inv_freq_;
    std::vector<mlx::core::array> key_cache_;    // per layer, for cached rollouts
    std::vector<mlx::core::array> value_cache_;
    int cache_pos_ = 0;
};

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
