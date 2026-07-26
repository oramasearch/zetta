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

private:
    std::vector<ResidentTensor> parameters_;
};

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
