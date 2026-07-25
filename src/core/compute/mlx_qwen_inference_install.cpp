// Install the fast Metal-GPU Qwen decode (MlxQwenInference, #555) as the agent's
// serving-inference runner. This is the ONE bridge from the MLX kernel (zetta-compute)
// to the agent's injection point (zetta-core), so chat()/respond() run on the GPU
// while zetta-core still links no MLX. Built only in the MLX build.

#if defined(ZETTA_WITH_MLX)

#include "zetta/compute/mlx_serving.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/agent/inference_runner.hpp"
#include "zetta/compute/mlx_qwen.hpp"

namespace zetta::compute::model {

void install_mlx_qwen_inference() {
    zetta::agent::install_qwen_inference_runner(
        [](const std::string& model_path, const std::vector<int>& prompt_tokens,
           int max_new_tokens, int eos_id) -> std::optional<std::vector<int>> {
            // One MlxQwenInference per checkpoint, kept warm across turns (loading the
            // checkpoint is the expensive part). Serialized: the KV cache is stateful
            // and Metal is a single GPU, so one decode at a time. reset() clears the
            // cache so each generation is an independent greedy decode from the prompt.
            static std::mutex mutex;
            static std::map<std::string, std::unique_ptr<MlxQwenInference>> engines;
            std::lock_guard<std::mutex> lock(mutex);
            auto found = engines.find(model_path);
            if (found == engines.end()) {
                std::string error;
                std::optional<MlxQwenInference> loaded =
                    MlxQwenInference::from_directory(model_path, &error);
                if (!loaded) return std::nullopt;  // fall back to the autograd CPU path
                found = engines
                            .emplace(model_path, std::make_unique<MlxQwenInference>(
                                                     std::move(*loaded)))
                            .first;
            }
            found->second->reset();
            return found->second->generate_greedy(prompt_tokens, max_new_tokens, eos_id);
        });
}

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
