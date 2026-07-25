// The Qwen serving-inference injection point storage (#555 serving). zetta-core
// holds the installed runner; the MLX layer installs it (install_mlx_qwen_inference).
// No MLX here — this TU never links libmlx (the module-split rule).

#include "zetta/agent/inference_runner.hpp"

#include <utility>

namespace zetta::agent {
namespace {

QwenInferenceRunner g_runner;

}  // namespace

void install_qwen_inference_runner(QwenInferenceRunner runner) {
    g_runner = std::move(runner);
}

bool has_qwen_inference_runner() { return static_cast<bool>(g_runner); }

const QwenInferenceRunner& qwen_inference_runner() { return g_runner; }

}  // namespace zetta::agent
