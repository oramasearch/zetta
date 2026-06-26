#pragma once

#include <cstddef>

#include "zetta/compute/compute_device.hpp"

// A trivial in-process ComputeDevice backed by host memory.
//
// This is the "stub device" the acceptance test exercises: it implements the
// memory model fully (allocate + host<->device copy over plain host RAM, where
// "device memory" is just `std::byte` storage) so the host<->device round-trip
// can be tested without any real backend, and it returns
// ComputeStatus::unsupported for every operator — it is a memory/transfer stub,
// not a compute backend.
//
// It is NOT the CPU reference backend. The real, operator-implementing CPU
// device is CpuDevice (#360), which runs the declared operators on host memory
// via the zetta::runtime thread pool. HostStubDevice exists so the interface and
// memory model can ship and be tested on their own (this issue), and as the
// minimal worked example of how to implement the seam.
namespace zetta::compute {

class HostStubDevice final : public ComputeDevice {
public:
    DeviceInfo info() const override;

    DeviceBuffer allocate(DataType dtype, std::size_t element_count) override;

    ComputeStatus copy_from_host(const void* host_source,
                                 DeviceBuffer& destination,
                                 std::size_t byte_count) override;

    ComputeStatus copy_to_host(const DeviceBuffer& source,
                               void* host_destination,
                               std::size_t byte_count) const override;

    ComputeStatus synchronize() override;

    // Operators: a transfer-only stub supports none of them.
    ComputeStatus map(UnaryOp op, const DeviceBuffer& input,
                      DeviceBuffer& output) override;
    ComputeStatus elementwise(BinaryOp op, const DeviceBuffer& lhs,
                              const DeviceBuffer& rhs,
                              DeviceBuffer& output) override;
    ComputeStatus reduce(ReduceOp op, const DeviceBuffer& input,
                         DeviceBuffer& output) override;
    ComputeStatus scan(ReduceOp op, ScanInclusivity inclusivity,
                       const DeviceBuffer& input, DeviceBuffer& output) override;
    ComputeStatus histogram(const DeviceBuffer& input, std::size_t bin_count,
                            double range_min, double range_max,
                            DeviceBuffer& output_counts) override;
    ComputeStatus gemm(const GemmParams& params, const DeviceBuffer& a,
                       const DeviceBuffer& b, DeviceBuffer& c) override;
    ComputeStatus axpy(double alpha, const DeviceBuffer& x,
                       DeviceBuffer& y) override;
    ComputeStatus sort(DeviceBuffer& keys, SortOrder order) override;
    ComputeStatus sort_by_key(DeviceBuffer& keys, DeviceBuffer& values,
                              SortOrder order) override;
    ComputeStatus gather(const DeviceBuffer& input, const DeviceBuffer& indices,
                         DeviceBuffer& output) override;
    ComputeStatus scatter(const DeviceBuffer& input, const DeviceBuffer& indices,
                          DeviceBuffer& output) override;

private:
    void release_buffer(void* handle) noexcept override;
};

}  // namespace zetta::compute
