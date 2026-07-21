---
name: native-ai-dev
description: Build AI models, layers, autograd ops, tokenizers, and training/RLVR loops NATIVELY in zetta — on zetta::autograd + the ComputeDevice seam + the byte-level BPE tokenizer, with no foreign runtime (no PyTorch / transformers / tokenizers / ONNX / BLAS, ADR-17). Use when adding a differentiable op or layer, assembling a transformer or other model, matching a Hugging Face architecture (RoPE/GQA/QK-norm/SwiGLU/RMSNorm), loading or saving a safetensors/HF checkpoint, building a BPE tokenizer, writing a training or GRPO/RLVR loop, generating text, or making any of these fast (thread pool / ComputeDevice / -march=native / KV cache). Triggers: "add an autograd op", "build a model/layer", "load a Qwen/HF checkpoint", "native tokenizer", "RLVR", "gradient check", "make the model faster".
---

# native-ai-dev — build AI natively in zetta

zetta builds its own AI: the model is the product, not a dependency. Everything —
autodiff, the transformer, the tokenizer, training, RLVR, generation, checkpoints —
is native C++26 on `zetta::autograd` + the `compute::ComputeDevice` seam + the
`zetta::tokenizer` BPE, with **no foreign runtime** (ADR-17). This was proven by
building the base agent (#555) and running it against the **real Qwen3-0.6B**
checkpoint.

**Read [`docs/autograd/building-ai-primitives.md`](../../../docs/autograd/building-ai-primitives.md)
first** — it is the full reference (the layers, the recipes, the performance
playbook, the validation pattern). This skill is the workflow that drives it. Also
load **`zetta-dev`** for the repo-wide C++/build/commit conventions.

## The six non-negotiables (never violate)

1. **Native, no foreign runtime.** Need an op? Implement it here. No framework in
   the train/infer path — same violation as shelling out to a CLI.
2. **Gradient-check every differentiable op** with `check_gradient` (analytic
   `backward()` vs finite differences) before composing it. An op is not done until
   it passes.
3. **Device-agnostic.** Heavy numerics go through `ComputeDevice`; the *caller*
   picks CPU / CUDA / MLX — never hardcode a backend in a model.
4. **Spread compute across the pool** with `runtime::parallel_for` (parallelize the
   *large* axis) — never a raw thread, never a single-threaded shipped hot loop.
5. **Non-throwing boundaries** — loaders/parsers return `std::optional` + a
   path-qualified reason.
6. **Nibble-dev** — one primitive per commit: red → green → gradient-check → build
   → commit with its docs.

## Workflow — pick the task

### Adding a differentiable op
1. Write the forward + `backward` closure in `src/core/autograd/tensor.cpp` (copy an
   existing op's shape: build a `Node`, set `parents`, capture cached forward values,
   `+=` into parents' grad). Declare it in `include/zetta/autograd/tensor.hpp`.
2. **Gradient-check it** in `tests/autograd/test_autograd.cpp`:
   `check_gradient({input}, {shape}, [&](const Tensor& x){ return sum(my_op(x)); });`
3. Only then compose it into a layer. Update `tensor.hpp`'s op list + the domain
   README + the building-ai guide in the same commit.

### Building / matching a model
1. Config from the checkpoint's `config.json` (`parse_qwen_config`, or a new
   `parse_*_config` for a new family).
2. Parameters = autograd leaves **keyed by Hugging Face tensor names**, flat stable
   order + a name→index map (so a checkpoint loads by name and optimizer moments line
   up). HF Linear weights are `[out,in]` → `matmul(x, transpose(W))`.
3. **Match the architecture exactly** or a real checkpoint gives noise. Verify the
   RoPE convention (rotate_half, `inv_freq=base^(-2i/d)`, `rope_theta`), QK-norm,
   `rms_norm_eps`, SwiGLU order, tied embeddings, and any decoupled `head_dim`.
4. Prove it trains (overfit → loss collapses → greedy prediction matches).

### Checkpoints, tokenizer, training, generation
- **Checkpoint:** `load_qwen_model(dir)` / `save_qwen_model` (single-file + sharded,
  F32/BF16); the `safetensors.hpp` layer is model-agnostic.
- **Tokenizer:** `BpeTokenizer::from_directory` (real HF `tokenizer.json` or
  `vocab.json`+`merges.txt`). New family = new pre-tokenizer scanner + loader, same
  BPE core.
- **Train / RLVR:** supervised (`cross_entropy` + `Adam` + `zero_grad`) or
  `grpo_step` / `grpo_step_text` (roll out a group → **verifiable** reward →
  `group_advantages` → `sequence_policy_gradient_loss`).
- **Text:** `generate_text` (tokenizer + model).

### Making it fast — the custom-speedup playbook

All of these are **built and measured on the real Qwen3-0.6B** (see
[building-ai-primitives.md](../../../docs/autograd/building-ai-primitives.md) §
Performance and the [report](../../../docs/reports/qwen3-real-checkpoint-validation-2026-07-21.md)).
Apply them in this order; always measure against a real checkpoint.

**CPU (the always-present path):**
1. **Build flags** — Release is `-O3 -DNDEBUG`. Add **`-march=native`** on a
   known-target box (build box / a node) for AVX/NEON on the matmul inner loops —
   opt-in, *not* a portable default (breaks on older CPUs).
2. **Don't transpose weights every forward** — HF weights are `[out,in]`;
   `matmul(x, transpose(W))` re-materializes the transpose each call (the 151936×1024
   lm_head = ~620 MB/forward). Use **`matmul_transposed_b(x, W)`** (`x·Wᵀ` over
   contiguous rows). *(2.3 s → 1.4 s.)*
3. **Thread the matmul over the LARGE axis, with a work-aware grain** — `parallel_for`'s
   default grain is an *item count* (4096) tuned for cheap ops; a matmul column is a
   whole dot product. Parallelize over `max(rows, cols)` and pick the grain by *work*
   (inline below ~131k flops, else split across all workers) via the grained
   `parallel_for` overload. Transformer inference has few tokens but wide projections,
   so row-parallel gives no parallelism. *(1.4 s → 226 ms; the big CPU win.)*
4. **KV cache for generation** — decoding re-forwards the whole sequence (O(n²)); a
   per-layer K/V cache makes each token O(n). `QwenInference`. *(0.4 → 13.5 tok/s.)*

**GPU (MLX) — the rule is: keep the WHOLE computation on-device in ONE graph.**
5. **Never route autograd ops one-at-a-time through the `ComputeDevice` for GPU
   inference** — each op uploads/downloads and forces an `mx::eval` sync; a 5-token
   forward is *slower* on MLX (450 ms) than the threaded CPU (230 ms), and generation
   is 1.3 tok/s. (Per-op MLX is still device-agnostic and *does* win ~6× on **batched**
   forwards, seq ≥ 128 — compute dominates transfer there.)
6. **Fused MLX kernels** (`zetta::compute::model`, `MlxQwenInference` /
   `MlxQwenModel`) — build the whole forward / decode / training step / RLVR step as
   **one MLX graph**, KV cache in MLX unified-memory arrays, **one `eval` per step**,
   `mx::value_and_grad` for training. Measured: **inference 42.9 tok/s** (~3× CPU
   KV-cache), **training ~290 ms/step** (~25–35× CPU), **fused GPU `grpo_step`
   ~2.2 s/step** (learns " dog"→" cat"). This is the fast GPU path.

Two orthogonal axes: **generation throughput** ← KV cache (CPU) or a fused decode
graph (GPU); **batched compute** (training / long-seq forwards) ← MLX gemm / fused
training. Match the tool to the workload.

## Validate against a real checkpoint

Toy tests aren't enough — download the real weights and run. Use the guarded pattern
(`tests/tokenizer/test_qwen_real_checkpoint.cpp`, gated on `ZETTA_QWEN_DIR`, skipped
otherwise so CI needs no download): if tokenization is **bit-exact** to a reference
and generation is coherent, the architecture is faithful. Use an external library
**only** to produce ground-truth ids to check against — never in the product path.

## Build & test

```bash
cmake --build build --target zetta-tests
./build/tests/zetta-tests --gtest_filter='Autograd.*:AutogradTransformer.*:QwenModel.*:Bpe.*'
```

## Related skills
- **zetta-dev** — repo-wide C++26 / build / commit / docs conventions (load with this).
- **new-input** — a new *input language* frontend (different from a model/op).
- **nibble-dev** (bridge) — the small-atomic-commit discipline this skill assumes.

## Version History
- v1.0.0 (2026-07-21): Initial — distilled from building the base agent (#555) and
  validating it on the real Qwen3-0.6B checkpoint.
