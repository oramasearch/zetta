#pragma once

#include <cstddef>
#include <memory>

#include "zetta/compute/compute_device.hpp"

// The CUDA ComputeDevice backend — the GPU implementation of the operator seam
// (compute_device.hpp). It allocates buffers in GPU VRAM, transfers host<->device
// over the CUDA runtime, and runs the declared operator set on the device.
//
// This header is **CUDA-free**: it includes no <cuda_runtime.h> and exposes no
// CUDA type, exactly the way src/gui/app.hpp forward-declares SDL types so the
// heavy headers stay in one .cu. All CUDA headers and kernels live in
// src/core/compute/cuda_device.cu, compiled into the zetta-compute static library
// only when the CUDA toolkit is detected; CUDA never leaks into zetta-core (the
// same discipline that keeps SDL/ImGui out of zetta-core).
//
// The class is only declared in a CUDA-enabled build (the `ZETTA_WITH_CUDA`
// compile definition, set by CMake when ZETTA_ENABLE_CUDA is on). In a CPU-only
// build the symbol is not compiled or linked, so a consumer that names CudaDevice
// without guarding on `ZETTA_WITH_CUDA` gets a clear compile error rather than a
// cryptic link error. The device registry (#362) selects it behind that guard.
namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

class CudaDevice final : public ComputeDevice {
public:
    // Drive CUDA device ordinal `device_index` (0 = the first GPU). The device is
    // selected (cudaSetDevice) before each operation, so several CudaDevice
    // instances over distinct ordinals can coexist on a multi-GPU node.
    explicit CudaDevice(int device_index = 0) noexcept;

    // Returns any pooled device memory to CUDA.
    ~CudaDevice() override;

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

    // Strided-batched GEMM: `batch` independent matmuls of identical shape, each operand advanced
    // by its own stride. This is what attention needs — QK^T and PV are one matmul per head, and
    // issuing them as a batch keeps the tensor cores fed instead of paying a launch per head.
    //
    // Row-major in, like `gemm`: the column-major transpose trick applies identically.
    ComputeStatus gemm_batched(const GemmParams& params, const DeviceBuffer& a,
                               std::size_t stride_a, const DeviceBuffer& b, std::size_t stride_b,
                               DeviceBuffer& c, std::size_t stride_c, int batch);

    // --- allocation pool -------------------------------------------------------------------
    // How many bytes of freed device memory to keep for reuse instead of returning to CUDA.
    // Default 2 GiB; 0 disables pooling entirely (every release goes straight to cudaFree).
    //
    // This exists because cudaMalloc and cudaFree SYNCHRONIZE the device, and an iterative
    // workload allocates constantly: one training step through the autograd engine's device_gemm
    // issues roughly 1,700 allocate/free pairs, which serializes the GPU against the host. The
    // measured symptom was ~11% GPU utilization while training (#928).
    void set_pool_limit(std::size_t bytes);

    // Bytes currently held in the pool, and how many allocations were served from it. A test (or
    // a benchmark) asserts reuse actually happens rather than assuming it: a pool whose hit count
    // stays zero is a pool that silently is not working.
    std::size_t pooled_bytes() const;
    std::size_t pool_hits() const;

    // Return every pooled block to CUDA. Called by the destructor; exposed so a caller that is
    // about to need peak memory (a large model load) can hand the reserve back first, and so
    // #924's memory ceiling is not silently consumed by a stale reserve.
    void drain_pool();

private:
    void release_buffer(void* handle) noexcept override;

    int device_index_ = 0;

    // Freed blocks keyed by exact byte size, plus the size of every live block (release_buffer
    // receives only a handle, so the size has to be remembered to pool it). A raw mutex is the
    // sanctioned exception here: the lowest cross-thread handoff inside a backend adapter, in the
    // one TU that names CUDA, never visible in a public API.
    struct Pool;
    std::shared_ptr<Pool> pool_;
};

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
