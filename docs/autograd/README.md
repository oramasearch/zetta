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

- (this README — overview; `tensor.hpp` carries the engine + ops as they ship)

## Shipped

- **`tensor.hpp`** — `Node`, `Tensor`, `backward()`, and the transformer op set:
  `matmul` · `sum` · `add` · `mul` · `scale` · `add_bias` · `gelu` · `silu` ·
  `row_softmax` · `rms_norm` · `transpose` · `embedding` · `cross_entropy` ·
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
- **`load_qwen_model(dir)`** — the entry point: point it at a Hugging Face
  checkpoint directory (`config.json` + `model.safetensors`) and get a
  fully-loaded, ready-to-run `QwenModel` (config parsed, weights loaded by name).
  Proven with a synthetic directory that round-trips to identical logits; a real
  Qwen directory is the same two files, with only the multi-GB weights and the
  tokenizer as the remaining external-asset step.
