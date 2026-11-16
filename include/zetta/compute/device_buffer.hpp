// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

#include "zetta/compute/data_type.hpp"

// A typed, owning handle to a region of device memory.
//
// This is the memory half of the ComputeDevice seam (compute_device.hpp). A
// DeviceBuffer is the unit operators read and write: it knows its element type
// (DataType) and element count, and holds an opaque, backend-defined handle to
// the actual storage — host RAM for a CPU device, GPU VRAM for a CUDA device.
// "Device memory" is whatever the owning device allocated; the buffer never
// interprets the handle, only the device that minted it does.
//
// Ownership is RAII and move-only, like a std::unique_ptr for device memory: the
// destructor returns the storage to its device, a move transfers ownership, and
// copies are disabled (two buffers must not free the same handle). The opaque
// handle is the one raw pointer the design allows — an unavoidable C-API-style
// resource handle — and it stays fully encapsulated and RAII-managed here, never
// handed out as an owning pointer (consumers get the typed accessors; backends
// get the raw handle to drive their kernels).
//
// Only a ComputeDevice can construct a non-empty buffer (via its allocate /
// make_buffer members) — the private constructor enforces that minting and
// freeing both go through the device.
namespace zetta::compute { class ComputeDevice; }

namespace zetta::compute {

class DeviceBuffer {
public:
    // An empty buffer owns nothing: no device, no handle, zero elements. This is
    // what allocate() returns on failure and what a moved-from buffer becomes.
    DeviceBuffer() = default;

    // Frees the held storage via its owning device (no-op when empty).
    ~DeviceBuffer();

    // Move-only: ownership of the device handle transfers; the source is left
    // empty. Copies are deleted so a handle is never double-freed.
    DeviceBuffer(DeviceBuffer&& other) noexcept;
    DeviceBuffer& operator=(DeviceBuffer&& other) noexcept;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;

    // Does this buffer own no storage?
    bool empty() const noexcept { return handle_ == nullptr; }
    explicit operator bool() const noexcept { return !empty(); }

    // The element type stored. Meaningful only for a non-empty buffer.
    DataType dtype() const noexcept { return dtype_; }

    // The number of elements the buffer holds.
    std::size_t element_count() const noexcept { return element_count_; }

    // The buffer's size in bytes (element_count * byte_size(dtype)).
    std::size_t size_bytes() const noexcept {
        return element_count_ * byte_size(dtype_);
    }

    // The device that owns this buffer (null when empty).
    ComputeDevice* device() const noexcept { return device_; }

    // The opaque, backend-defined storage handle. Backends cast this to their
    // own pointer type to drive kernels; callers should not dereference it.
    void* handle() noexcept { return handle_; }
    const void* handle() const noexcept { return handle_; }

private:
    friend class ComputeDevice;

    // Minted only by a ComputeDevice (through make_buffer). `handle` must be
    // storage `device` allocated for `element_count` elements of `dtype`.
    DeviceBuffer(ComputeDevice* device, void* handle, DataType dtype,
                 std::size_t element_count) noexcept
        : device_(device),
          handle_(handle),
          dtype_(dtype),
          element_count_(element_count) {}

    // Release the held storage (if any) back to its device and become empty.
    void reset() noexcept;

    ComputeDevice* device_ = nullptr;
    void* handle_ = nullptr;
    DataType dtype_ = DataType::Float32;
    std::size_t element_count_ = 0;
};

}  // namespace zetta::compute
