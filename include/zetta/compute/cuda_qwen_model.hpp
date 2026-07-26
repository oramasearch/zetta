#pragma once

#include <cstddef>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/cuda_resident.hpp"

// A Qwen model whose weights LIVE on the GPU (#923).
//
// The autograd engine keeps parameters on the host and ships them across PCIe on every matmul, so a
// 0.6B model holds 425 MiB of a 40 GB card and the GPU idles at ~11% (#928). This type inverts
// that: the parameters are uploaded once, in the order `autograd::QwenModel` exposes them, and stay
// for the model's lifetime. Forward, loss, backward and the optimizer step (rungs 3-7) then compose
// the fused #922 kernels over resident storage, and only a scalar loss returns to the host.
//
// Deliberately built FROM a loaded `autograd::QwenModel` rather than from a checkpoint directory:
// that model already owns config parsing, safetensors loading, BF16 widening and shape validation,
// all of which are device-independent. Re-implementing them here would duplicate the one part of
// the pipeline that is already correct on every backend.
#include "zetta/autograd/qwen.hpp"  // QwenConfig (a value type; no device dependency)

namespace zetta::autograd {
class QwenModel;
}

namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

class CudaQwenModel {
public:
    CudaQwenModel() = default;

    // Upload every parameter of `host`, preserving its order and shapes. Returns an invalid model
    // if any upload fails, rather than a partially-resident one — a model missing a weight would
    // produce plausible-looking wrong numbers instead of an error.
    static CudaQwenModel from_host(ComputeDevice& device, const zetta::autograd::QwenModel& host);

    CudaQwenModel(CudaQwenModel&&) noexcept = default;
    CudaQwenModel& operator=(CudaQwenModel&&) noexcept = default;
    CudaQwenModel(const CudaQwenModel&) = delete;
    CudaQwenModel& operator=(const CudaQwenModel&) = delete;

    bool valid() const noexcept { return !parameters_.empty(); }
    std::size_t parameter_count() const noexcept { return parameters_.size(); }

    // Total device bytes held by the parameters. The number that was 425 MiB of 40,960 before this
    // existed, and the one an acceptance run reports.
    std::size_t resident_bytes() const noexcept;

    const ResidentTensor& parameter(std::size_t index) const { return parameters_[index]; }
    ResidentTensor& parameter(std::size_t index) { return parameters_[index]; }

    // One decoder layer, entirely on-device (#923 rung 3): pre-norm residual attention, then
    // pre-norm residual SwiGLU MLP, composing the fused #922 kernels with cuBLAS projections.
    // `hidden` and `out` are [seq, d_model] resident tensors; nothing crosses PCIe in between.
    ComputeStatus forward_layer(ComputeDevice& device, const ResidentTensor& hidden, int layer,
                                int seq, ResidentTensor& out) const;

    // The whole forward (#923 rung 4): embed -> every layer -> final norm -> output projection.
    // `ids` is the ONLY thing that crosses PCIe on the way in, and the logits the only thing on the
    // way out. Result is [seq, vocab_size].
    ComputeStatus forward(ComputeDevice& device, const std::vector<int>& ids,
                          ResidentTensor& logits) const;

private:
    // Parameters are registered by autograd::QwenModel in a fixed order: embed_tokens, then 11 per
    // layer, then model.norm (then lm_head when embeddings are untied). Indexing arithmetically
    // rather than by name keeps the hot path free of string lookups; `kPerLayer` is asserted
    // against the actual count at load, so a change to that order fails loudly at construction
    // instead of silently reading the wrong weight.
    static constexpr std::size_t kPerLayer = 11;
    std::size_t layer_base(int layer) const {
        return 1 + static_cast<std::size_t>(layer) * kPerLayer;
    }

    std::vector<ResidentTensor> parameters_;
    zetta::autograd::QwenConfig config_{};
};

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
