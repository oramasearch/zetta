#include "zetta/compute/cuda_transformer.hpp"

#include <string_view>

#include <cuda_runtime.h>

// Fused transformer kernels (#922). See the header for why these exist and the two rules they
// follow (operate on device buffers; fused and deterministic).
namespace zetta::compute {
namespace {

constexpr int kThreadsPerBlock = 256;

ComputeStatus from_cuda(cudaError_t error, std::string_view context) {
    if (error == cudaSuccess) return ComputeStatus::success();
    std::string reason = std::string{context} + ": " + cudaGetErrorString(error);
    if (error == cudaErrorMemoryAllocation) return ComputeStatus::out_of_memory(std::move(reason));
    return ComputeStatus::backend_error(std::move(reason));
}

// A launch is only "ok" once it has actually run: cudaGetLastError catches launch-time rejection,
// the synchronize surfaces faults raised inside the kernel. Reporting success before that turns a
// crashed kernel into silently wrong numbers.
ComputeStatus check_launch(std::string_view context) {
    const cudaError_t launch = cudaGetLastError();
    if (launch != cudaSuccess) return from_cuda(launch, context);
    return from_cuda(cudaDeviceSynchronize(), context);
}

const float* as_const_float(const DeviceBuffer& buffer) {
    return static_cast<const float*>(buffer.handle());
}
float* as_float(DeviceBuffer& buffer) { return static_cast<float*>(buffer.handle()); }

// Sum `value` across the block into thread 0's return, using shared memory. Deterministic for a
// fixed block size: the tree reduction always combines the same pairs in the same order.
__device__ float block_sum(float value, float* scratch) {
    const int lane = static_cast<int>(threadIdx.x);
    scratch[lane] = value;
    __syncthreads();
    for (int stride = kThreadsPerBlock / 2; stride > 0; stride >>= 1) {
        if (lane < stride) scratch[lane] += scratch[lane + stride];
        __syncthreads();
    }
    return scratch[0];
}

// out[r, c] = x[r, c] * rsqrt(mean(x[r, :]^2) + eps) * weight[c]
//
// One block per row. The row is read twice from L2/global (once to accumulate the sum of squares,
// once to scale) and written once — versus a composed implementation, which would materialize the
// squares, the row means, and the reciprocal as separate full-size tensors.
__global__ void rms_norm_kernel(const float* __restrict__ x, const float* __restrict__ weight,
                                float* __restrict__ out, int rows, int cols, float epsilon) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= rows) return;
    const float* row_x = x + static_cast<long long>(row) * cols;
    float* row_out = out + static_cast<long long>(row) * cols;

    float local = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float value = row_x[c];
        local += value * value;
    }
    const float total = block_sum(local, scratch);
    const float scale = rsqrtf(total / static_cast<float>(cols) + epsilon);

    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        row_out[c] = row_x[c] * scale * weight[c];
    }
}

// grad_x, one block per row. Needs two row reductions (sum of squares for the scale, and
// sum(dout * x) for the projection term), both fused into this single pass over the row.
__global__ void rms_norm_grad_x_kernel(const float* __restrict__ x,
                                      const float* __restrict__ weight,
                                      const float* __restrict__ grad_out,
                                      float* __restrict__ grad_x, int rows, int cols,
                                      float epsilon) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= rows) return;
    const long long offset = static_cast<long long>(row) * cols;
    const float* row_x = x + offset;
    const float* row_grad_out = grad_out + offset;
    float* row_grad_x = grad_x + offset;

    float local_square = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float value = row_x[c];
        local_square += value * value;
    }
    const float mean_square = block_sum(local_square, scratch) / static_cast<float>(cols);
    const float scale = rsqrtf(mean_square + epsilon);

    float local_dot = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        local_dot += row_grad_out[c] * weight[c] * row_x[c];
    }
    __syncthreads();  // block_sum reuses `scratch`, so the previous reduction must be finished
    const float mean_dot = block_sum(local_dot, scratch) / static_cast<float>(cols);

    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float dout = row_grad_out[c] * weight[c];
        row_grad_x[c] = scale * (dout - row_x[c] * scale * scale * mean_dot);
    }
}

// grad_weight[c] += sum_r grad_out[r, c] * x[r, c] * scale_r
//
// One thread per COLUMN, looping over rows in a fixed order. Deliberately not an atomicAdd from the
// per-row kernel: float atomics accumulate in whatever order blocks happen to retire, so the result
// varies in its last bits between identical runs — and #926 asserts CUDA reproduces the reference
// exactly. A deterministic gradient is worth one extra pass over the activation.
__global__ void rms_norm_grad_weight_kernel(const float* __restrict__ x,
                                            const float* __restrict__ grad_out,
                                            const float* __restrict__ row_scale,
                                            float* __restrict__ grad_weight, int rows, int cols) {
    const int col = static_cast<int>(blockIdx.x) * kThreadsPerBlock + static_cast<int>(threadIdx.x);
    if (col >= cols) return;
    float total = 0.0F;
    for (int row = 0; row < rows; ++row) {
        const long long offset = static_cast<long long>(row) * cols + col;
        total += grad_out[offset] * x[offset] * row_scale[row];
    }
    grad_weight[col] += total;  // accumulate, matching the autograd engine's leaf gradients
}

// The per-row 1/rms factor, so grad_weight does not recompute it per column.
__global__ void rms_norm_row_scale_kernel(const float* __restrict__ x, float* __restrict__ row_scale,
                                          int rows, int cols, float epsilon) {
    __shared__ float scratch[kThreadsPerBlock];
    const int row = static_cast<int>(blockIdx.x);
    if (row >= rows) return;
    const float* row_x = x + static_cast<long long>(row) * cols;
    float local = 0.0F;
    for (int c = static_cast<int>(threadIdx.x); c < cols; c += kThreadsPerBlock) {
        const float value = row_x[c];
        local += value * value;
    }
    const float total = block_sum(local, scratch);
    if (threadIdx.x == 0) {
        row_scale[row] = rsqrtf(total / static_cast<float>(cols) + epsilon);
    }
}

// Shapes are validated at the boundary: a kernel launched with mismatched extents reads out of
// bounds, which shows up much later as corrupt numbers rather than as an error.
ComputeStatus validate(const DeviceBuffer& x, const DeviceBuffer& weight, int rows, int cols) {
    if (rows <= 0 || cols <= 0) {
        return ComputeStatus::invalid_argument("rms_norm: rows and cols must be positive");
    }
    if (x.empty() || weight.empty()) {
        return ComputeStatus::invalid_argument("rms_norm: x and weight must be allocated");
    }
    if (x.element_count() < static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm: x is smaller than rows * cols");
    }
    if (weight.element_count() < static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm: weight is smaller than cols");
    }
    return ComputeStatus::success();
}

}  // namespace

ComputeStatus cuda_rms_norm(const DeviceBuffer& x, const DeviceBuffer& weight, DeviceBuffer& out,
                            int rows, int cols, float epsilon) {
    if (const ComputeStatus valid = validate(x, weight, rows, cols); !valid.ok()) return valid;
    if (out.element_count() < static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm: out is smaller than rows * cols");
    }
    rms_norm_kernel<<<rows, kThreadsPerBlock>>>(as_const_float(x), as_const_float(weight),
                                                as_float(out), rows, cols, epsilon);
    return check_launch("rms_norm");
}

ComputeStatus cuda_rms_norm_backward(const DeviceBuffer& x, const DeviceBuffer& weight,
                                     const DeviceBuffer& grad_out, DeviceBuffer& grad_x,
                                     DeviceBuffer& grad_weight, int rows, int cols,
                                     float epsilon) {
    if (const ComputeStatus valid = validate(x, weight, rows, cols); !valid.ok()) return valid;
    const std::size_t elements = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
    if (grad_out.element_count() < elements || grad_x.element_count() < elements) {
        return ComputeStatus::invalid_argument(
            "rms_norm_backward: grad_out and grad_x must hold rows * cols");
    }
    if (grad_weight.element_count() < static_cast<std::size_t>(cols)) {
        return ComputeStatus::invalid_argument("rms_norm_backward: grad_weight must hold cols");
    }

    rms_norm_grad_x_kernel<<<rows, kThreadsPerBlock>>>(
        as_const_float(x), as_const_float(weight), as_const_float(grad_out), as_float(grad_x), rows,
        cols, epsilon);
    if (const ComputeStatus launched = check_launch("rms_norm_backward.grad_x"); !launched.ok()) {
        return launched;
    }

    // grad_weight reduces over rows, so it needs the per-row scale. Allocated through the owning
    // device rather than raw cudaMalloc, so it frees itself and is accounted for like any buffer.
    ComputeDevice* device = x.device();
    if (device == nullptr) {
        return ComputeStatus::invalid_argument("rms_norm_backward: x has no owning device");
    }
    DeviceBuffer row_scale =
        device->allocate(DataType::Float32, static_cast<std::size_t>(rows));
    if (row_scale.empty()) {
        return ComputeStatus::out_of_memory("rms_norm_backward: could not allocate row scales");
    }
    rms_norm_row_scale_kernel<<<rows, kThreadsPerBlock>>>(as_const_float(x), as_float(row_scale),
                                                          rows, cols, epsilon);
    if (const ComputeStatus launched = check_launch("rms_norm_backward.row_scale");
        !launched.ok()) {
        return launched;
    }

    const int column_blocks = (cols + kThreadsPerBlock - 1) / kThreadsPerBlock;
    rms_norm_grad_weight_kernel<<<column_blocks, kThreadsPerBlock>>>(
        as_const_float(x), as_const_float(grad_out), as_const_float(row_scale),
        as_float(grad_weight), rows, cols);
    return check_launch("rms_norm_backward.grad_weight");
}

}  // namespace zetta::compute
