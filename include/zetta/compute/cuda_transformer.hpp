#pragma once

#include <cstddef>

#include "zetta/compute/compute_device.hpp"

// Fused transformer kernels for the CUDA backend (#922, part of #920).
//
// `cuda_device.cu` provides the DATA-ENGINE primitives (gemm, reduce, scan, sort, histogram,
// gather/scatter) that the GBDT and DDF paths use. None of them are transformer ops, so training a
// transformer through `ComputeDevice` today offloads only matmul and runs RMSNorm, RoPE, SwiGLU,
// softmax and every elementwise op in scalar host code. Measured consequence on an A100 40GB:
// **~11% GPU utilization, 425 MiB of 40,960 MiB resident, 25.6 s/step** — against 1.45 s/step for
// the MLX path on a Mac laptop (#928). The GPU is not slow; it is starved.
//
// These functions close that gap by keeping the work on the device. Two rules they all follow:
//
//   * **Operate on DeviceBuffer, never on host vectors.** An op that takes host data forces a
//     round trip and re-creates the problem it exists to solve. Composition happens on-device.
//   * **Fused, and deterministic.** Fusion is the point: a composed RMSNorm reads and writes the
//     activation three times where a fused one does it once, and at these sizes the ops are
//     bandwidth-bound, so the fusion *is* the speedup. Determinism is equally load-bearing — the
//     equivalence gate (#926) asserts CUDA reproduces the CPU reference exactly for greedy decode,
//     so no kernel here may use atomics for a reduction whose order would then vary run to run.
//
// Declared only in a CUDA build (`ZETTA_WITH_CUDA`); the header stays CUDA-free so it can be
// included anywhere in zetta-compute without pulling in <cuda_runtime.h>, the same discipline
// cuda_device.hpp follows.
namespace zetta::compute {

#if defined(ZETTA_WITH_CUDA)

// --- RMSNorm ---------------------------------------------------------------------------------
// out[r, c] = x[r, c] * rsqrt(mean_c(x[r, :]^2) + epsilon) * weight[c]
//
// One block per row, so the row's sum of squares is a block reduction in shared memory and the
// activation is read once and written once. `x`, `out` are [rows, cols]; `weight` is [cols].
ComputeStatus cuda_rms_norm(const DeviceBuffer& x, const DeviceBuffer& weight, DeviceBuffer& out,
                            int rows, int cols, float epsilon);

// The gradient of the above.
//
//   scale_r = rsqrt(mean_c(x[r, :]^2) + epsilon)
//   dout    = grad_out[r, c] * weight[c]
//   grad_x  = scale_r * (dout - x[r, c] * scale_r^2 * mean_c(dout * x[r, :]))
//   grad_w  = sum_r grad_out[r, c] * x[r, c] * scale_r
//
// `grad_x` is computed one block per row (a reduction over the row). `grad_w` is a reduction over
// ROWS, which is deliberately a second kernel with one thread per column rather than an atomicAdd
// into `grad_w`: float atomics accumulate in nondeterministic order, and a gradient that changes
// in its last bits between runs would make #926's equality assertions untestable.
//
// `grad_weight` is ACCUMULATED into (+=), matching how the autograd engine accumulates leaf
// gradients across a step; zero it first if you want a fresh value.
ComputeStatus cuda_rms_norm_backward(const DeviceBuffer& x, const DeviceBuffer& weight,
                                     const DeviceBuffer& grad_out, DeviceBuffer& grad_x,
                                     DeviceBuffer& grad_weight, int rows, int cols, float epsilon);

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
