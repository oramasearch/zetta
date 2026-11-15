// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

#include "zetta/compute/compute_device.hpp"

// The CPU reference backend of the ComputeDevice seam (ADR-27 / ARC-9, #360).
//
// CpuDevice is the always-built, always-available implementation of the
// ComputeDevice interface: it runs every declared operator (map/elementwise,
// reduce/scan, histogram, gemm/axpy, sort/sort_by_key, gather/scatter) on host
// memory, parallelizing the data-parallel ones across the zetta::runtime thread
// pool (via runtime::parallel_for). Its results are the **numerical reference**
// the optional CUDA backend (#361) is checked against, so correctness — not peak
// throughput — is the priority.
//
// Memory model: "device memory" is plain host RAM, exactly like HostStubDevice
// (allocate is new[], transfer is memcpy). The difference from the stub is that
// CpuDevice implements the operators instead of returning Unsupported. No
// SDL/CUDA type appears here — it is pure zetta-core and builds CPU-only.
namespace zetta::compute {

class CpuDevice final : public ComputeDevice {
public:
    DeviceInfo info() const override;

    // ---- Memory model (host RAM) --------------------------------------------
    DeviceBuffer allocate(DataType dtype, std::size_t element_count) override;

    ComputeStatus copy_from_host(const void* host_source,
                                 DeviceBuffer& destination,
                                 std::size_t byte_count) override;

    ComputeStatus copy_to_host(const DeviceBuffer& source,
                               void* host_destination,
                               std::size_t byte_count) const override;

    ComputeStatus synchronize() override;

    // ---- Operators (implemented on host memory) -----------------------------
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
