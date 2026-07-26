#include "zetta/compute/cuda_qwen_model.hpp"

#include <utility>

#include "zetta/autograd/qwen.hpp"
#include "zetta/autograd/tensor.hpp"

namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

CudaQwenModel CudaQwenModel::from_host(ComputeDevice& device,
                                       const zetta::autograd::QwenModel& host) {
    CudaQwenModel model;
    model.parameters_.reserve(host.parameters().size());
    for (const zetta::autograd::Tensor& parameter : host.parameters()) {
        ResidentTensor resident =
            ResidentTensor::upload(device, parameter.data(), parameter.shape());
        if (!resident.valid()) {
            // All or nothing: a partially-resident model would run and produce plausible wrong
            // numbers, which is far worse to diagnose than a refusal at load.
            return CudaQwenModel{};
        }
        model.parameters_.push_back(std::move(resident));
    }
    return model;
}

std::size_t CudaQwenModel::resident_bytes() const noexcept {
    std::size_t total = 0;
    for (const ResidentTensor& parameter : parameters_) total += parameter.size_bytes();
    return total;
}

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
