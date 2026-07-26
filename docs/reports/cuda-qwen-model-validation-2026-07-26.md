# Validation — CudaQwenModel, the model on the GPU (2026-07-26)

**Subject.** `CudaQwenModel` ([#923](https://github.com/oramasearch/zetta/issues/923)) — Qwen3-0.6B
trained entirely on-device, composing the fused kernels from
[#922](https://github.com/oramasearch/zetta/issues/922).
**Hardware.** Lambda `gpu_1x_a100_sxm4`: A100-SXM4-40GB (`sm_80`), driver 580.105.08, CUDA 12.8,
30 vCPU, 216 GB RAM, Ubuntu 24.04.4, clang-22, CMake 4.4.0.
**Branch.** `feat/cuda-qwen-model` · PR [#931](https://github.com/oramasearch/zetta/pull/931).

## Result

| metric | before | after | note |
|---|---|---|---|
| **per step, REAL corpus** (variable length, 641-1229 tokens) | **15.4 s** | **0.563 s** | **27x faster** |
| vs MLX on a Mac (1.45 s/step, same corpus) | 10.6x slower | **2.6x faster** | |
| GPU utilization | 11% | **98%** | sampled during the timed loop |
| VRAM in use | 425 MiB | **11,593 MiB** | of 40,960 |
| resident weights | ~0 | **2.22 GB** across 310 parameters | uploaded once |

## How it got to 0.563, and a correction worth keeping in the record

An earlier version of this report claimed **0.742 s/step** from a throughput microbenchmark that
trained one fixed synthetic sequence at seq 512. That number was not representative — real examples
run 641-1229 tokens and attention is O(seq^2), and one fixed shape keeps allocation-pool reuse at
100% where real data churns shapes. Measured properly on the corpus, that same build was **1.87
s/step: 29% SLOWER than MLX**, not faster.

The acceptance criterion now measures the real corpus, and the microbenchmark's assertion against
the MLX bar was removed so a favourable synthetic case can never certify a slower engine again.

Closing the gap took two changes, and the second was the one that mattered:

| build | s/step | |
|---|---|---|
| CudaQwenModel, streaming attention | 1.87 | serial key walk in both directions |
| + batched forward attention | 1.619 | 13% — forward only |
| **+ batched backward attention** | **0.563** | **65%** |

The backward is roughly twice the forward's cost, so leaving it on the serial kernel capped the
whole change. Caching the softmax probabilities (~96 MB per layer) turns its four products into
batched GEMMs on the tensor cores with nothing recomputed. grad_K and grad_V are summed across each
GQA group in a fixed-order pass, keeping the determinism the streaming kernel got from owning the
key index rather than trading it for an atomic.

The streaming kernel is retained: it is O(seq * head_dim) in memory where the batched path
materializes [n_heads, seq, seq], so it remains the right choice at long context.

## How it got here, in order

Each step was measured, not assumed — and two of the four confident predictions were wrong.

| change | per step | verdict |
|---|---|---|
| starting point | 25.5 s | |
| allocation pooling ([#928](https://github.com/oramasearch/zetta/issues/928)) | 24.0 s | 3.4% — **hypothesis wrong about magnitude** |
| cuBLAS + TF32 ([#929](https://github.com/oramasearch/zetta/issues/929)) | 18.5 s | 28% — a 200-400x kernel win, Amdahl-capped |
| parallel `row_softmax` ([#930](https://github.com/oramasearch/zetta/issues/930)) | 15.4 s | 17% |
| parallel `add`/`scale` | 15.4 s | **0% — predicted by analogy, and wrong** |
| **`CudaQwenModel` (#923)** | **0.742 s** | **20.8x** |

The lesson is in the last two rows. Every incremental fix to the *existing* path bought single-digit
percentages, because the architecture was the cost: weights re-uploaded per matmul, activations
round-tripping to the host between ops. Keeping the model on the device was worth more than
everything before it combined.

## Correctness

25 CUDA tests green. What each rung proves, bottom-up:

- **`ResidentTensor`** — storage and contents survive allocator churn; move transfers ownership
  without a free-and-realloc; an empty upload is invalid rather than a silent zero buffer.
- **Weight residency** — resident bytes account for the whole parameter set; weights match the host
  **bit-for-bit**; shapes survive.
- **Decoder layer** — matches `autograd::QwenModel` under GQA (4 query heads over 2 kv heads).
- **Full forward** — matches the autograd reference, and is **bitwise deterministic across runs**.
- **Cross-entropy** — matches a double-precision host reference; the gradient sums to zero per row.
- **Cached forward** — bit-identical to the uncached forward.
- **Layer backward** — **gradient-checked against numerical differences**. The only test that
  separates a correct backward from a merely self-consistent one.
- **Training step** — loss falls from 9.43 to **0.000186** overfitting a fixed sequence, which is
  the standard proof that forward, backward and optimizer agree; weights demonstrably change.
- **Checkpoint round trip** — CUDA-trained weights load back into the host model, which then runs.

## Design decisions worth carrying forward

- **Determinism was a constraint, not an afterthought.** Every cross-block reduction owns its output
  index instead of using `atomicAdd` — `grad_weight` is one thread per column, attention's
  `grad_k`/`grad_v` are one block per *key*, and the embedding backward is one thread per
  *dimension* looping tokens in order. Atomics would have been simpler and would vary in the last
  bits run to run, which [#926](https://github.com/oramasearch/zetta/issues/926) cannot tolerate.
- **Weights are never transposed.** They stay in Hugging Face `[out, in]` layout and projections
  pass `transpose_b` to cuBLAS. A transposed copy is a second 2.2 GB that defeats residency.
- **Only two things cross PCIe per step**: the token ids in, one float of loss out.
- **TF32 is on and switchable.** It changes the loss trajectory slightly (10 mantissa bits), which
  is why #926 must evaluate against the active math mode rather than loosening its assertions.

## Limits

- Single GPU. Multi-GPU and multi-node go through the existing collectives and are not built.
- Activation memory scales with sequence length; 11.6 GB at seq 512 leaves headroom on a 40 GB card
  but not unlimited room. The #922 attention kernel is already O(seq · head_dim) rather than
  O(seq²), so the growth is in the cached activations, not the scores.
- fp32 weights with TF32 matmuls. BF16 storage is the next lever and the checkpoints already carry
  BF16 on disk.
