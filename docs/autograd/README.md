# autograd — zetta's own reverse-mode autodiff

`zetta::autograd` is a native C++26 reverse-mode automatic-differentiation engine —
the foundation the RLVR model (#555) trains on. It exists so zetta owns its training
math end to end: **no foreign runtime** (ADR-17 — not libtorch, not a Python stack),
which keeps the binary lean, the build simple, and lets us write our own **fused
kernels** instead of inheriting a framework's. The design is micrograd-shaped (a
value graph with per-node backward closures), extended from scalars to **tensors**,
which is what a transformer needs.

## How it works

- **`Node`** — one value in the graph: `data`, `shape`, accumulated `grad`, its
  `parents`, and a `backward` closure that scatters this node's gradient to its
  parents.
- **`Tensor`** — a shared handle to a `Node`. Build the graph forward with the ops;
  call **`backward()`** on the scalar loss to seed its gradient to 1 and propagate
  through the graph in reverse topological order, accumulating each parameter's
  gradient.
- **Correctness** — every op is verified by **gradient checking**: the analytic
  gradient from `backward()` must match a finite-difference estimate. That is the
  standard proof an autograd op is right.

## Device-agnostic execution (shipped)

The reference kernels are plain host loops, but the heavy op (matmul, forward and
backward) routes through the `compute::ComputeDevice` seam: `set_compute_device()`
points the autograd at a `CpuDevice`, `CudaDevice`, or `MlxDevice` (thread-local, so
each cluster node picks its own), and the *same graph* runs on it. Verified: the
`Transformer` forward produces matching logits on the CPU and the MLX GPU
(`AutogradDeviceAgnostic.TransformerForwardCpuEqualsMlx`), so one distributed model
spans CPU + GPU + MLX nodes — the cluster uses what each node has. Gradients are host
floats, so the distributed allreduce (already built for the RLVR loop) combines them
across heterogeneous devices.

## Pages

- **[building-ai-primitives.md](building-ai-primitives.md)** — **the playbook for
  building any new model / layer / op / tokenizer / training loop natively on this
  stack** (recipes, the gradient-check discipline, the Qwen exemplar, the
  performance guide, real-checkpoint validation). Read it before building AI in
  zetta; the [`native-ai-dev`](../../.claude/skills/native-ai-dev/SKILL.md) skill
  drives it.
- (this README — overview; `tensor.hpp` carries the engine + ops as they ship)

## Shipped

- **`tensor.hpp`** — `Node`, `Tensor`, `backward()`, and the transformer op set:
  `matmul` · `sum` · `add` · `mul` · `scale` · `add_bias` · `gelu` · `silu` ·
  `row_softmax` · `rms_norm` · `transpose` · `embedding` · `patch_embedding` ·
  `cross_entropy` ·
  `row_slice` · `policy_gradient_loss` · `rope` · `col_slice` · `concat_cols`
  (the last two split/reassemble attention heads). Every op is gradient-checked.
- **`transformer.hpp`** — `causal_self_attention`, `grouped_query_attention`
  (the Qwen/LLaMA GQA: n_heads query heads over n_kv_heads shared key/value
  heads, RoPE per head with a selectable base, causal SDPA; a QK-norm overload
  adds the Qwen3 per-head RMSNorm on Q and K), `feed_forward`,
  `swiglu_feed_forward`, the `Transformer` model, `Adam`, and `zero_grad`. Trains
  by overfit and by RLVR policy gradient on the native engine.
- **`qwen.hpp`** — `QwenConfig` + `QwenModel`, the Qwen3-parity decoder-only
  transformer: embeddings → per-layer {RMSNorm → GQA+QK-norm+RoPE → RMSNorm →
  SwiGLU} → final RMSNorm → lm_head, with parameters keyed by **Hugging Face
  tensor names** (`model.embed_tokens.weight`, `model.layers.{L}.self_attn.*`,
  …) so a real safetensors checkpoint loads by name (`set_weight`). Supports GQA,
  QK-norm, tied embeddings, and the Qwen3 RoPE base. `parse_qwen_config()` builds
  the matching `QwenConfig` from a checkpoint's Hugging Face `config.json` (so the
  right architecture is instantiated before its weights load).
- **`safetensors.hpp`** — a host-float `.safetensors` reader/writer
  (`parse_safetensors` / `read_safetensors_file`, decoding **F32 and BF16** to
  host floats; `serialize_safetensors` / `write_safetensors_file`). No MLX, no
  foreign runtime — the always-compiled path that loads a Qwen checkpoint into a
  `QwenModel` by tensor name. Malformed input is a non-throwing boundary
  (`std::nullopt` + a reason).
- **Checkpoint I/O on `QwenModel`** — `to_safetensors()` dumps every parameter as
  an HF-named map; `load_safetensors()` loads one back by name (in place),
  returning a `LoadReport` of any missing / mismatched / unexpected tensors. A
  model dumped to a file and loaded into a fresh, differently-seeded model
  produces identical logits — the exact path a real Qwen checkpoint takes
  (verified with a synthetic checkpoint; the multi-GB weights aren't fetchable
  here, but the format and load path are the same).
- **Generation on `QwenModel`** — `generate_greedy()` (deterministic argmax
  decode) and `generate()` (temperature sampling with an rng), the autoregressive
  "act" step of the RLVR loop. A model overfit on a sequence greedily *generates*
  that sequence — training and generation are mirror images.
- **`QwenInference`** — a fast **KV-cached** generation engine: a plain-float
  (no-autograd) forward that caches per-layer key/value and forwards one token at a
  time, so each step is O(seq) instead of re-forwarding the whole sequence. Produces
  the same logits as `QwenModel::forward` (verified); on the real Qwen3-0.6B it took
  greedy generation from 0.4 → 2.87 tok/s (~7×). For generation / RLVR rollouts;
  training keeps the differentiable forward.
- **`load_qwen_model(dir)`** — the entry point: point it at a Hugging Face
  checkpoint directory and get a fully-loaded, ready-to-run `QwenModel` (config
  parsed, weights loaded by name). Handles both a single `model.safetensors` and a
  **sharded** checkpoint (`model.safetensors.index.json` + the
  `model-0000N-of-0000M.safetensors` shards it maps — how real multi-GB Qwen ships)
  by merging the shards. Proven with synthetic single-file and sharded directories
  that round-trip to identical logits; a real Qwen directory is the same files,
  with only the multi-GB weights and the tokenizer as the remaining external-asset
  step.
- **`save_qwen_model(model, dir)`** + **`serialize_qwen_config`** — the inverse:
  write a `config.json` + `model.safetensors` directory that `load_qwen_model`
  reads back identically. This is how a **post-trained base-agent checkpoint is
  shipped** — load a base, RLVR post-train it, save the improved checkpoint.
  Proven: a config round-trips through JSON, save→load is bit-identical, and an
  overfit model reloaded from a saved directory still generates its memorized
  sequence.

## Validated against the real Qwen3-0.6B checkpoint

This is not only synthetic: `load_qwen_model` loaded the **real** `Qwen/Qwen3-0.6B`
`model.safetensors` (1.5 GB bf16), and the native model generated correct,
coherent text — "The capital of France is" → " Paris. The capital of Italy",
"2 + 2 =" → " 4", etc. Four native gradient steps then flipped its answer from
"Paris" to a taught "Berlin", proving the real weights train through the autograd
(the path `grpo_step` composes on). Full write-up:
[`../reports/qwen3-real-checkpoint-validation-2026-07-21.md`](../reports/qwen3-real-checkpoint-validation-2026-07-21.md);
locked in by the guarded `tests/tokenizer/test_qwen_real_checkpoint.cpp`
(`ZETTA_QWEN_DIR`).
