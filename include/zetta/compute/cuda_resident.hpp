#pragma once

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/device_buffer.hpp"

// A tensor that LIVES on the device (#923, rung 1).
//
// The autograd engine's `device_gemm` uploads both operands and downloads the result on every call,
// so weights cross PCIe hundreds of times per training step and nothing accumulates on the card.
// Measured consequence: 425 MiB resident of 40,960, GPU at ~11%, 15.4 s/step against MLX's 1.45 on
// weaker hardware (#928). The fix is not a faster transfer — it is not transferring: a parameter is
// uploaded once when the model loads and stays put for the life of the model.
//
// This type is deliberately thin. It owns a `DeviceBuffer` plus the shape, and it is **move-only**
// with no copy: a copy would duplicate 2.4 GB of weights silently, and an implicit re-upload is the
// exact bug this exists to prevent. Anything that needs the storage asks for `handle()` and drives
// a kernel with it.
namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

class ResidentTensor {
public:
    ResidentTensor() = default;

    // Upload `values` once and keep them. Returns an INVALID tensor on an empty input or a failed
    // allocation, rather than a valid-looking empty buffer — a zero-element weight means the
    // checkpoint is malformed, and letting it through defers that failure to a kernel launch,
    // where it surfaces as corrupt numbers instead of an error.
    static ResidentTensor upload(ComputeDevice& device, const std::vector<float>& values,
                                 std::vector<int> shape);

    // Take ownership of a buffer that is already on the device — how an op returns its result
    // without a round trip. The counterpart to `upload`, and the reason a layer's output can feed
    // the next layer directly.
    static ResidentTensor adopt(DeviceBuffer buffer, std::vector<int> shape) {
        if (buffer.empty()) return ResidentTensor{};
        return ResidentTensor{std::move(buffer), std::move(shape)};
    }

    // Move-only. The model holds these in a vector that grows during load, so a move must transfer
    // ownership without a free-and-realloc; a copy would silently duplicate the whole model.
    ResidentTensor(ResidentTensor&&) noexcept = default;
    ResidentTensor& operator=(ResidentTensor&&) noexcept = default;
    ResidentTensor(const ResidentTensor&) = delete;
    ResidentTensor& operator=(const ResidentTensor&) = delete;

    bool valid() const noexcept { return !buffer_.empty(); }
    const std::vector<int>& shape() const noexcept { return shape_; }
    std::size_t element_count() const noexcept { return buffer_.element_count(); }
    std::size_t size_bytes() const noexcept { return buffer_.size_bytes(); }

    // The device storage, for handing to a kernel. Const-correct in intent: a caller that only
    // reads should take the const overload.
    void* handle() noexcept { return buffer_.handle(); }
    const void* handle() const noexcept { return buffer_.handle(); }

    DeviceBuffer& buffer() noexcept { return buffer_; }
    const DeviceBuffer& buffer() const noexcept { return buffer_; }

    // Copy back to the host. Used by checkpoint save and by tests — NOT on any hot path, which is
    // the entire point of the type.
    ComputeStatus download(std::span<float> destination) const;

private:
    ResidentTensor(DeviceBuffer buffer, std::vector<int> shape)
        : buffer_(std::move(buffer)), shape_(std::move(shape)) {}

    DeviceBuffer buffer_;
    std::vector<int> shape_;
};

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
