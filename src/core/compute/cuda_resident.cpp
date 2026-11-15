// SPDX-License-Identifier: MIT

#include "zetta/compute/cuda_resident.hpp"

#include <utility>

namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

ResidentTensor ResidentTensor::upload(ComputeDevice& device, const std::vector<float>& values,
                                      std::vector<int> shape) {
    // Empty is invalid, not an empty-but-valid tensor. See the header: a zero-element weight means
    // a malformed checkpoint, and passing it along defers the failure to a kernel launch.
    if (values.empty()) return ResidentTensor{};
    DeviceBuffer buffer = device.upload<float>(std::span<const float>(values));
    if (buffer.empty()) return ResidentTensor{};
    return ResidentTensor{std::move(buffer), std::move(shape)};
}

ComputeStatus ResidentTensor::download(std::span<float> destination) const {
    if (buffer_.empty()) {
        return ComputeStatus::invalid_argument("ResidentTensor::download: tensor is not resident");
    }
    ComputeDevice* device = buffer_.device();
    if (device == nullptr) {
        return ComputeStatus::invalid_argument("ResidentTensor::download: no owning device");
    }
    return device->download<float>(buffer_, destination);
}

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
