#include "zetta/compute/device_buffer.hpp"

#include <utility>

#include "zetta/compute/compute_device.hpp"

// DeviceBuffer's lifetime hooks and ComputeDevice::make_buffer live here, where
// both types are complete: a buffer's destructor frees its storage through the
// device (an incomplete type in device_buffer.hpp), and make_buffer constructs a
// buffer via its private constructor (ComputeDevice is a friend of DeviceBuffer).
namespace zetta::compute {

DeviceBuffer::~DeviceBuffer() { reset(); }

DeviceBuffer::DeviceBuffer(DeviceBuffer&& other) noexcept
    : device_(other.device_),
      handle_(other.handle_),
      dtype_(other.dtype_),
      element_count_(other.element_count_) {
    other.device_ = nullptr;
    other.handle_ = nullptr;
    other.element_count_ = 0;
}

DeviceBuffer& DeviceBuffer::operator=(DeviceBuffer&& other) noexcept {
    if (this != &other) {
        reset();  // free whatever we currently hold first
        device_ = other.device_;
        handle_ = other.handle_;
        dtype_ = other.dtype_;
        element_count_ = other.element_count_;
        other.device_ = nullptr;
        other.handle_ = nullptr;
        other.element_count_ = 0;
    }
    return *this;
}

void DeviceBuffer::reset() noexcept {
    if (handle_ != nullptr && device_ != nullptr) {
        device_->release_buffer(handle_);
    }
    device_ = nullptr;
    handle_ = nullptr;
    element_count_ = 0;
}

DeviceBuffer ComputeDevice::make_buffer(void* handle, DataType dtype,
                                        std::size_t element_count) noexcept {
    return DeviceBuffer(this, handle, dtype, element_count);
}

}  // namespace zetta::compute
