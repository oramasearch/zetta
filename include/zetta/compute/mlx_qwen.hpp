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

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <mlx/mlx.h>

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

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
