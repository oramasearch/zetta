#include "zetta/compute/host_stub_device.hpp"

#include <cstddef>
#include <cstring>
#include <new>
#include <thread>

// The host-memory stub device. "Device memory" is plain `std::byte` storage on
// the heap; allocate/copy are new[]/std::memcpy. Operators are all unsupported —
// this exercises the interface's memory model, it is not a compute backend (the
// CPU reference backend with real operators is CpuDevice, #360).
namespace zetta::compute {

DeviceInfo HostStubDevice::info() const {
    DeviceInfo information;
    information.kind = DeviceKind::Cpu;
    information.name = "host-stub";
    information.compute_units = std::thread::hardware_concurrency();
    information.memory_bytes = 0;       // host RAM; not separately budgeted
    information.unified_memory = true;  // the "device" is the host address space
    return information;
}

DeviceBuffer HostStubDevice::allocate(DataType dtype,
                                      std::size_t element_count) {
    const std::size_t byte_count = element_count * byte_size(dtype);
    // new[] never returns null (it throws std::bad_alloc); convert that to an
    // empty buffer so the seam stays exception-free for callers.
    std::byte* storage = new (std::nothrow) std::byte[byte_count];
    if (storage == nullptr) return DeviceBuffer{};
    return make_buffer(storage, dtype, element_count);
}

ComputeStatus HostStubDevice::copy_from_host(const void* host_source,
                                             DeviceBuffer& destination,
                                             std::size_t byte_count) {
    if (byte_count == 0) return ComputeStatus::success();
    if (host_source == nullptr || destination.handle() == nullptr) {
        return ComputeStatus::invalid_argument("copy_from_host: null buffer");
    }
    if (byte_count > destination.size_bytes()) {
        return ComputeStatus::invalid_argument(
            "copy_from_host: byte_count exceeds destination size");
    }
    std::memcpy(destination.handle(), host_source, byte_count);
    return ComputeStatus::success();
}

ComputeStatus HostStubDevice::copy_to_host(const DeviceBuffer& source,
                                           void* host_destination,
                                           std::size_t byte_count) const {
    if (byte_count == 0) return ComputeStatus::success();
    if (host_destination == nullptr || source.handle() == nullptr) {
        return ComputeStatus::invalid_argument("copy_to_host: null buffer");
    }
    if (byte_count > source.size_bytes()) {
        return ComputeStatus::invalid_argument(
            "copy_to_host: byte_count exceeds source size");
    }
    std::memcpy(host_destination, source.handle(), byte_count);
    return ComputeStatus::success();
}

ComputeStatus HostStubDevice::synchronize() {
    // Transfers and (the absent) operators run synchronously on the host, so
    // there is nothing to wait for.
    return ComputeStatus::success();
}

// --- Operators: unsupported on a transfer-only stub --------------------------

ComputeStatus HostStubDevice::map(UnaryOp, const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("map");
}

ComputeStatus HostStubDevice::elementwise(BinaryOp, const DeviceBuffer&,
                                          const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("elementwise");
}

ComputeStatus HostStubDevice::reduce(ReduceOp, const DeviceBuffer&,
                                     DeviceBuffer&) {
    return ComputeStatus::unsupported("reduce");
}

ComputeStatus HostStubDevice::scan(ReduceOp, ScanInclusivity,
                                   const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("scan");
}

ComputeStatus HostStubDevice::histogram(const DeviceBuffer&, std::size_t, double,
                                        double, DeviceBuffer&) {
    return ComputeStatus::unsupported("histogram");
}

ComputeStatus HostStubDevice::gemm(const GemmParams&, const DeviceBuffer&,
                                   const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("gemm");
}

ComputeStatus HostStubDevice::axpy(double, const DeviceBuffer&, DeviceBuffer&) {
    return ComputeStatus::unsupported("axpy");
}

ComputeStatus HostStubDevice::sort(DeviceBuffer&, SortOrder) {
    return ComputeStatus::unsupported("sort");
}

ComputeStatus HostStubDevice::sort_by_key(DeviceBuffer&, DeviceBuffer&,
                                          SortOrder) {
    return ComputeStatus::unsupported("sort_by_key");
}

ComputeStatus HostStubDevice::gather(const DeviceBuffer&, const DeviceBuffer&,
                                     DeviceBuffer&) {
    return ComputeStatus::unsupported("gather");
}

ComputeStatus HostStubDevice::scatter(const DeviceBuffer&, const DeviceBuffer&,
                                      DeviceBuffer&) {
    return ComputeStatus::unsupported("scatter");
}

void HostStubDevice::release_buffer(void* handle) noexcept {
    delete[] static_cast<std::byte*>(handle);
}

}  // namespace zetta::compute
