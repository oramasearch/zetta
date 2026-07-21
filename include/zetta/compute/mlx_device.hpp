#pragma once

#include <cstddef>

#include "zetta/compute/compute_device.hpp"

// The MLX ComputeDevice backend — the Apple-Silicon GPU implementation of the
// operator seam (compute_device.hpp), via Apple's MLX array framework running on
// Metal. It is the default portable accelerator on macOS: GPU when available, with
// CpuDevice as the always-present fallback. SYCL is the deferred cross-platform
// backend (its own later slice); MLX and SYCL both target this same interface.
//
// This header is **MLX-free**: it includes no <mlx/*> header and exposes no MLX
// type, exactly as cuda_device.hpp is CUDA-free. All MLX headers live in
// src/core/compute/mlx_device.cpp, compiled into the zetta-compute static library
// only when MLX is present; MLX never leaks into zetta-core (the same discipline
// that keeps SDL/ImGui and CUDA out of it).
//
// The class is only declared in an MLX-enabled build (the `ZETTA_WITH_MLX` compile
// definition, set by CMake when ZETTA_ENABLE_MLX is on). In a build without MLX the
// symbol is not compiled, so naming MlxDevice without guarding on `ZETTA_WITH_MLX`
// is a clear compile error rather than a link error.
//
// Apple Silicon has **unified memory**: the CPU and GPU share one address space, so
// a device buffer is a plain host allocation the GPU can read directly — allocate
// and the host<->device copies are host-memory operations, and info() reports
// unified_memory = true.
namespace zetta::compute {

#if defined(ZETTA_WITH_MLX)

class MlxDevice final : public ComputeDevice {
public:
    MlxDevice() noexcept = default;

    DeviceInfo info() const override;

    DeviceBuffer allocate(DataType dtype, std::size_t element_count) override;

    ComputeStatus copy_from_host(const void* host_source,
                                 DeviceBuffer& destination,
                                 std::size_t byte_count) override;

    ComputeStatus copy_to_host(const DeviceBuffer& source,
                               void* host_destination,
                               std::size_t byte_count) const override;

    ComputeStatus synchronize() override;

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
    // gemm and axpy run on the GPU via MLX; the remaining operators return
    // Unsupported for now (added as the model runtime needs them).
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

#endif  // ZETTA_WITH_MLX

}  // namespace zetta::compute
