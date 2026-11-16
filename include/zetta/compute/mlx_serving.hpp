// SPDX-License-Identifier: MIT

#pragma once

// Wire the fast Metal-GPU Qwen decode (MlxQwenInference, #555) into the agent's
// serving path. Call once at startup from the zetta-api layer (beside
// install_native_http_transport / install_postgres_protocol), guarded on the MLX
// build. It installs a zetta::agent::QwenInferenceRunner backed by a per-checkpoint
// MlxQwenInference cache, so chat()/respond() decode on the GPU instead of the
// per-op-synced autograd CPU path. No-op-safe: on a non-MLX build this header
// declares nothing, so the call site is compiled out under the same guard.
//
// This declaration carries NO MLX headers, so main() can include it without linking
// mlx (the definition lives in zetta-compute where libmlx is linked).

#if defined(ZETTA_WITH_MLX)

namespace zetta::compute::model {

void install_mlx_qwen_inference();

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
