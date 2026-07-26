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

// --- RoPE ------------------------------------------------------------------------------------
// Rotary positional embedding, the rotate_half convention Qwen/LLaMA use and the same one
// autograd::rope implements: rotate the pair (i, i + head_dim/2) of each row by
// angle = position * base^(-2i/head_dim).
//
//   out[p, i]        = x[p, i] * cos(angle) - x[p, i + half] * sin(angle)
//   out[p, i + half] = x[p, i] * sin(angle) + x[p, i + half] * cos(angle)
//
// `x` is [rows, head_dim] where the row index IS the position (so a [seq, n_heads * head_dim]
// tensor is passed one head at a time, or with rows = seq and head_dim per head). Being a
// rotation, it preserves each row's norm and is the identity at position 0 — two properties the
// tests assert directly, because they catch a swapped sin/cos that a formula comparison can miss.
// `rows_per_position` handles the packed multi-head layout. A projection is [seq, n_heads *
// head_dim], which reshapes CONTIGUOUSLY to [seq * n_heads, head_dim] — but then the row index is
// `s * n_heads + h`, not the position `s`. Passing rows_per_position = n_heads recovers it as
// `row / rows_per_position`, so every head of a token rotates by that token's angle. Passing 1
// (the default) is the plain [seq, head_dim] case where row IS the position.
ComputeStatus cuda_rope(const DeviceBuffer& x, DeviceBuffer& out, int rows, int head_dim,
                        float base, int position_offset, int rows_per_position = 1);

// The inverse rotation, which is what the transpose of an orthogonal matrix gives:
//   grad_x[p, i]        =  grad_out[p, i] * cos + grad_out[p, i + half] * sin
//   grad_x[p, i + half] = -grad_out[p, i] * sin + grad_out[p, i + half] * cos
ComputeStatus cuda_rope_backward(const DeviceBuffer& grad_out, DeviceBuffer& grad_x, int rows,
                                 int head_dim, float base, int position_offset,
                                 int rows_per_position = 1);

// --- SwiGLU ----------------------------------------------------------------------------------
// out = silu(gate) * up, elementwise, with silu(v) = v * sigmoid(v) — the Qwen MLP activation.
// Fused so the intermediate silu(gate) is never written to memory: at these sizes the op is
// bandwidth-bound, so materializing it would roughly double the traffic for no benefit.
ComputeStatus cuda_swiglu(const DeviceBuffer& gate, const DeviceBuffer& up, DeviceBuffer& out,
                          std::size_t elements);

// grad_gate = grad_out * up * sigmoid(g) * (1 + g * (1 - sigmoid(g)))
// grad_up   = grad_out * silu(g)
// Both are written in one pass, since both need sigmoid(gate) and recomputing it is cheaper than
// reading a saved copy back from memory.
ComputeStatus cuda_swiglu_backward(const DeviceBuffer& gate, const DeviceBuffer& up,
                                   const DeviceBuffer& grad_out, DeviceBuffer& grad_gate,
                                   DeviceBuffer& grad_up, std::size_t elements);

// --- Causal self-attention -------------------------------------------------------------------
// softmax(mask(Q K^T / sqrt(head_dim))) V, where position t attends only to positions <= t.
//
// This is the op that matters most. A composed implementation materializes the score matrix —
// [n_heads, seq, seq], which at seq=1000 and 16 heads is 16M floats *per layer* — then reads it
// twice more for the softmax and once more for the PV product. Here the scores are never written
// to memory at all: each block streams the keys for one query in tiles, keeping a running max and
// sum (the online-softmax recurrence), so memory traffic is O(seq * head_dim) instead of O(seq^2).
//
// GQA is supported: `n_kv_heads` may be smaller than `n_heads`, in which case query head h reads
// key/value head h / (n_heads / n_kv_heads), which is what Qwen3 does.
//
// Layouts, all row-major and contiguous:
//   q                [seq, n_heads * head_dim]
//   k, v             [seq, n_kv_heads * head_dim]
//   out              [seq, n_heads * head_dim]
//   softmax_lse      [n_heads, seq]  — log-sum-exp per (head, query)
//
// `softmax_lse` is an OUTPUT of the forward and an INPUT to the backward. Saving it is what lets
// the backward recompute the softmax without a second max pass and without ever materializing the
// scores; it is the one piece of state the two passes must share.
ComputeStatus cuda_causal_attention(const DeviceBuffer& q, const DeviceBuffer& k,
                                    const DeviceBuffer& v, DeviceBuffer& out,
                                    DeviceBuffer& softmax_lse, int seq, int n_heads,
                                    int n_kv_heads, int head_dim);

// The gradient. Split into two kernels by which index each one owns, so neither needs an atomic:
// grad_q is one block per (head, QUERY) accumulating over keys, while grad_k and grad_v are one
// block per (kv head, KEY) accumulating over queries. Atomics would be simpler and would make the
// gradients nondeterministic, which #926's exact-equality assertions cannot tolerate.
//
// `row_dot` is grad_out . out summed per (head, query) — the standard flash-attention backward
// term. Like `softmax_lse` it is computed once and shared, rather than recomputed per key block.
ComputeStatus cuda_causal_attention_backward(const DeviceBuffer& q, const DeviceBuffer& k,
                                             const DeviceBuffer& v, const DeviceBuffer& out,
                                             const DeviceBuffer& grad_out,
                                             const DeviceBuffer& softmax_lse, DeviceBuffer& row_dot,
                                             DeviceBuffer& grad_q, DeviceBuffer& grad_k,
                                             DeviceBuffer& grad_v, int seq, int n_heads,
                                             int n_kv_heads, int head_dim);

// --- Embedding lookup ------------------------------------------------------------------------
// out[t, :] = table[ids[t], :]. `ids` is an Int32 device buffer of `seq` token ids.
//
// A gather rather than a matmul: the one-hot formulation would multiply a [seq, vocab] matrix by
// the table, which for a 151k vocabulary is absurd. Ids live on the device so the whole forward
// takes only the token list across PCIe, not the embedded rows.
ComputeStatus cuda_embedding(const DeviceBuffer& table, const DeviceBuffer& ids, DeviceBuffer& out,
                             int seq, int d_model, int vocab_size);

// --- Cross-entropy (the language-model loss) ---------------------------------------------------
// loss = mean_t( logsumexp(logits[t]) - logits[t, target[t]] ), matching autograd::cross_entropy.
//
// Fused: the softmax is never materialized. A [seq, vocab] softmax at a 151k vocabulary is ~600 MB
// per forward, written once and read twice, for a scalar. Here each row's logsumexp is a block
// reduction and only `row_loss[seq]` is kept — which the backward needs anyway.
//
// `row_loss` is an output of the forward and an input to the backward, the same
// compute-once-share-it pattern `softmax_lse` uses in attention.
ComputeStatus cuda_cross_entropy(const DeviceBuffer& logits, const DeviceBuffer& targets,
                                 DeviceBuffer& row_loss, DeviceBuffer& total, int seq,
                                 int vocab_size);

// grad_logits = (softmax(logits) - onehot(target)) / seq, recomputed from the logits rather than
// from a saved softmax — recomputing an exp is far cheaper than storing and re-reading 600 MB.
ComputeStatus cuda_cross_entropy_backward(const DeviceBuffer& logits, const DeviceBuffer& targets,
                                          DeviceBuffer& grad_logits, int seq, int vocab_size);

#endif  // ZETTA_WITH_CUDA

}  // namespace zetta::compute
