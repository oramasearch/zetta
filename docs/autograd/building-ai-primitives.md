# Building AI models & primitives on zetta's native stack

This is the playbook for building a new model, layer, op, tokenizer, or training
loop **natively in zetta** — no PyTorch, no `transformers`, no `tokenizers`, no
ONNX, no BLAS you didn't write (ADR-17, the [first
principle](../../CLAUDE.md#the-first-principle)). It is written from the base agent
(#555): a Qwen3-parity transformer, its tokenizer, and its RLVR loop, all native
C++26, **validated against the real `Qwen/Qwen3-0.6B` checkpoint**
([report](../reports/qwen3-real-checkpoint-validation-2026-07-21.md)). When you
build the next model or primitive, follow this.

Use the **[`native-ai-dev`](../../.claude/skills/native-ai-dev/SKILL.md)** skill for
the step-by-step workflow; this page is the reference it points at.

## The non-negotiables

1. **Native, no foreign runtime.** If you need an op, implement it here. The model
   is the product, not a dependency. A framework in the training/inference path is
   the same violation as shelling out to a CLI.
2. **Gradient-check every differentiable op.** An op is not done until its analytic
   `backward()` matches finite differences (`check_gradient`). This is the
   gold-standard correctness proof and it is *cheap* — never skip it.
3. **Device-agnostic.** Heavy numerics route through the `compute::ComputeDevice`
   seam, so the same graph runs on CPU / CUDA / MLX. Never hardcode a backend into a
   model; the *caller* selects the device.
4. **Spread compute across the pool.** CPU-bound loops go through
   `runtime::parallel_for` (the shared work-stealing thread pool) — never a raw
   `std::thread`, never a single-threaded hot loop in a shipped kernel.
5. **No exceptions across the boundary.** Parsers / loaders return
   `std::optional` + a path-qualified reason (`load_qwen_model`,
   `parse_safetensors`, `BpeTokenizer::from_directory` all do this).
6. **Nibble-dev.** One primitive per commit: red test → green → gradient-check →
   build → commit with its docs. Compose upward; each rung depends only on green
   rungs.

## The layers you build on

| Layer | Header | What it gives you |
|---|---|---|
| Autodiff engine | `autograd/tensor.hpp` | `Tensor`/`Node`/`backward()` + the op library (matmul, add/mul/scale, gelu/silu, row_softmax, rms_norm, transpose, embedding, cross_entropy, rope, col_slice/concat_cols, row_slice, `(sequence_)policy_gradient_loss`) |
| Model blocks | `autograd/transformer.hpp` | `grouped_query_attention` (GQA + QK-norm + RoPE), `swiglu_feed_forward`, `Transformer`, `Adam`, `zero_grad` |
| A whole model | `autograd/qwen.hpp` | `QwenModel` (Qwen3-parity, HF-named params), `load_qwen_model`/`save_qwen_model`, `generate` |
| Checkpoints | `autograd/safetensors.hpp` | host-float `.safetensors` read/write (F32 + BF16) |
| Compute backend | `compute/compute_device.hpp` | the `ComputeDevice` seam (`CpuDevice` always; `MlxDevice`/`CudaDevice` opt-in) |
| Parallelism | `runtime.hpp` | `parallel_for` (data-parallel over the pool), `when_all` / `task` (coroutine fan-out) |
| Text I/O | `tokenizer/bpe.hpp` | byte-level BPE `BpeTokenizer` (text ⇄ token ids) |
| RLVR | `agent/qwen_rlvr.hpp` | `grpo_step` / `grpo_step_text` (GRPO over a model) |

## Recipe 1 — add a new differentiable op

Every op is a forward that builds a `Node` plus a `backward` closure that scatters
the output gradient to its parents. The shape to copy (from `tensor.cpp`):

```cpp
Tensor my_op(const Tensor& a) {
    std::vector<float> out = /* forward compute over a.data() */;
    auto node = make_node(std::move(out), /* out shape */);
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node, /*cached forward values*/](const std::vector<float>& g) {
        // accumulate dL/da into a_node->grad using g (dL/dout). Always += (a value
        // can feed multiple consumers). Cache anything from the forward you need.
    };
    return Tensor{node};
}
```

Then, **before anything else**, gradient-check it (`tests/autograd/test_autograd.cpp`):

```cpp
check_gradient({/*input*/}, {/*shape*/}, [&](const Tensor& x){ return sum(my_op(x)); });
```

`check_gradient` compares the analytic gradient from `backward()` against a central
finite difference — if they agree, the op is correct. Only then compose it into a
block. Heavy ops (matmul) additionally route through the `ComputeDevice`; see
`matmul` for the pattern (capture the device at construction so backprop runs on the
device the graph was built on).

## Recipe 2 — assemble a model (the Qwen exemplar)

`QwenModel` is the template for a real architecture. Key decisions to reuse:

- **Parameters are autograd leaves keyed by Hugging Face tensor names**
  (`model.layers.{L}.self_attn.q_proj.weight`, …), stored in a flat, stable-ordered
  vector (so an optimizer's per-parameter moments line up) plus a name→index map. A
  real checkpoint then loads by name.
- **HF Linear weights are `[out, in]`** and compute `x · Wᵀ`; the autograd's
  `matmul(x, M)` wants `M` as `[in, out]`, so `forward` uses
  `matmul(x, transpose(W))`. (Correct, but see Performance — pre-transpose for
  inference.)
- **Match the architecture exactly** or a real checkpoint produces noise. Qwen3:
  grouped-query attention, per-head **QK-norm** (RMSNorm on Q and K before RoPE),
  **RoPE** with `rope_theta` (1e6 for Qwen3, the rotate_half convention:
  `out[i]=a·cos−b·sin`, `out[i+d/2]=b·cos+a·sin`, `inv_freq=base^(−2i/d)`), **SwiGLU**
  MLP, RMSNorm (`rms_norm_eps` = 1e-6), tied embeddings, and a **decoupled
  `head_dim`** (128, so `n_heads·head_dim` ≠ `hidden_size`). No learned positional
  embedding (RoPE only).
- **Config from `config.json`** via `parse_qwen_config`; a new architecture family
  gets its own `parse_*_config`.

## Recipe 3 — checkpoints (load a real HF model)

`load_qwen_model(dir)` reads `config.json` (→ config) + `model.safetensors`
(single-file **or** sharded via `model.safetensors.index.json`, F32/BF16 → host f32)
and loads every parameter by name, returning a `LoadReport` of any
missing/mismatched tensors. `save_qwen_model` writes the inverse — how a
post-trained checkpoint ships. The safetensors layer (`safetensors.hpp`) is
model-agnostic; reuse it for any architecture.

## Recipe 4 — the tokenizer (text ⇄ ids)

Byte-level BPE (`BpeTokenizer`): `from_directory(dir)` loads a real HF
`tokenizer.json` (or `vocab.json` + `merges.txt`). The pipeline is pre-tokenize
(the model family's regex, hand-written as a scanner — `std::regex` cannot express
`\p{L}`/lookahead) → byte↔unicode map → BPE merges (integer-id, packed-`uint64` pair
keys, no hot-path allocation) → ids. The all-256-byte base vocab guarantees exact
round-trip. A different tokenizer family = a new pre-tokenizer + loader, same core.

## Recipe 5 — train it (and RLVR)

Supervised: `forward` → `cross_entropy` → `backward()` → `Adam::step`, with
`zero_grad` between steps (see `QwenModel` overfit tests). RLVR: `grpo_step` rolls
out a group of completions (`generate`), scores each with a **verifiable**
`CompletionReward` (a check that *runs*, not a learned reward model), turns rewards
into `group_advantages` (the group is its own baseline — no value function), and
takes one `sequence_policy_gradient_loss` step. `grpo_step_text` wraps it with the
tokenizer so the reward is over decoded **text**. This is blender-rl's GRPO,
native and device-agnostic.

## Performance — how to make it fast

The reference kernels are correct first; speed comes from these, in order of impact:

- **`parallel_for` over the *large* axis.** The matmul spreads across the pool,
  parallelizing over the bigger of rows/cols — transformer inference has few tokens
  (small rows) and huge projections (up to the 151936 vocab), so row-parallel gives
  no parallelism. **Caveat:** `parallel_for`'s grain (`kMinItemsPerChunk`, 4096)
  only splits when `count > 8192`, so it threads the lm_head (n=151936) but runs the
  layer projections (n ≤ 3072) inline — the grain is tuned for cheap element-wise
  items, not heavy matmul columns. Long training sequences (large m·n) thread
  naturally; short-sequence inference stays inline and is instead sped up by the KV
  cache below. A work-aware grain for matmul is an open runtime tuning question.
- **Don't transpose weights on the forward — use `matmul_transposed_b`.** HF weights
  are `[out, in]`; `matmul(x, transpose(W))` materializes the transpose every call
  (the 151936×1024 lm_head alone copies ~620 MB), which was ~40% of real-model
  forward latency. `matmul_transposed_b(x, W)` computes `x·Wᵀ` directly over
  contiguous rows (cache/SIMD-friendly). **Done** — `QwenModel` and the Qwen
  attention/MLP keep weights in HF layout and never transpose on the forward
  (2.3 s → 1.4 s on Qwen3-0.6B).
- **KV cache for generation — done (`QwenInference`).** Without it, decoding token
  *t* re-forwards the whole sequence (O(n²)). `QwenInference` is a plain-float
  (no-autograd) forward that caches per-layer K/V and forwards one token at a time,
  so each step is O(n) — verified to produce the same logits as `QwenModel::forward`.
  On the real Qwen3-0.6B it took generation from **0.4 → 2.87 tok/s** (~7×). Use it
  for generation / RLVR rollouts; training still uses the autograd forward.
- **The `ComputeDevice` gemm (MLX GPU).** `matmul` and `matmul_transposed_b` route
  through the active `ComputeDevice` (`set_compute_device`), so the model is
  device-agnostic: `QwenModel::forward` produces the same logits on CPU and the
  **`MlxDevice`** Apple-Silicon GPU (`-DZETTA_ENABLE_MLX=ON`) — verified on the real
  Qwen3-0.6B (max|CPU−MLX| ≈ 6e-5). **But per-op routing is not a speed win for
  small forwards:** each op uploads/downloads and forces an `mx::eval` sync, so a
  5-token real-model forward is *slower* on MLX (~450 ms) than the threaded CPU
  (~230 ms) — the sync overhead dominates the small compute. MLX pays off for
  **large batched training** (compute dominates transfer) or a **fully-on-device
  forward** (one lazy graph, one eval — what the native `MlxTransformer` does);
  routing individual autograd ops through the GPU does not exploit it. Use the GPU
  for big-batch training, the threaded CPU + KV cache for generation/rollouts.
- **Build flags.** Release is `-O3 -DNDEBUG`. Add **`-march=native`** on a
  known-target box (build box / a node) for AVX/NEON on the matmul inner loops —
  it is *not* a portable default (breaks on older CPUs), so gate it behind an
  option, don't hardcode it into `CMakeLists.txt`.
- **Coroutine fan-out.** For many independent rollouts (RLVR groups, eval), compose
  `runtime::when_all` over `io_task`s (the `distributed_training` pattern) — but for
  one big model the matmul already saturates cores, so don't oversubscribe by
  fanning out rollouts *and* threading each matmul.

## Validate against a real checkpoint

Correctness on toy data is not enough — download the real weights and run. The
guarded test pattern (`tests/tokenizer/test_qwen_real_checkpoint.cpp`) loads a real
checkpoint from `ZETTA_QWEN_DIR` and is **skipped** when unset, so CI needs no
multi-GB download. If tokenization is bit-exact to the reference and generation is
coherent ("The capital of France is" → " Paris"), the architecture is faithful — if
any detail is wrong, the output is noise. Use an external reference library **only**
to produce ground-truth ids to check against, never in the product path.

## Where things live

`include/zetta/autograd/` + `src/core/autograd/` (engine, transformer, Qwen,
safetensors) · `include/zetta/tokenizer/` (byte-level BPE) · `include/zetta/agent/`
(RLVR, text I/O) · `include/zetta/compute/` (the device seam) · `runtime.hpp` (the
pool + coroutines). Tests mirror these under `tests/`. Keep this network current
(the [docs discipline](../../CLAUDE.md#documentation)) — a new op updates
`tensor.hpp`'s list, this page, and the domain README in the same commit.
