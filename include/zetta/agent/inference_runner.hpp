#pragma once

// The Qwen inference injection point (#555 serving) — how the agent's chat/respond
// loop reaches the FAST Metal-GPU decode without zetta-core ever linking MLX.
//
// zetta-core owns the agent (chat.cpp) but must not link MLX (the module-split rule,
// like the connector transport). So serving inference is an injected callable: the
// MLX layer in zetta-compute installs an MlxQwenInference-backed runner at startup
// (install_mlx_qwen_inference, guarded on ZETTA_WITH_MLX), and chat() prefers it,
// falling back to the native autograd::QwenInference on the CPU when none is
// installed (an MLX-less build, or a checkpoint the MLX path cannot load).
//
// The runner is greedy + deterministic and gated against the CPU path (same logits,
// see mlx_qwen.hpp), so it is a drop-in — the fast path returns the same tokens.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace zetta::agent {

// Generate a greedy completion for `prompt_tokens` from the model checkpoint at
// `model_path`: returns the completion token ids, or std::nullopt to fall back to
// the built-in autograd path (e.g. the checkpoint could not be loaded on the GPU).
using QwenInferenceRunner = std::function<std::optional<std::vector<int>>(
    const std::string& model_path, const std::vector<int>& prompt_tokens,
    int max_new_tokens, int eos_id)>;

// Install the process-wide serving runner. Called once at startup by the MLX layer.
void install_qwen_inference_runner(QwenInferenceRunner runner);

// Whether a serving runner is installed (an MLX build that wired one in).
bool has_qwen_inference_runner();

// The installed runner. Only valid when has_qwen_inference_runner().
const QwenInferenceRunner& qwen_inference_runner();

}  // namespace zetta::agent
