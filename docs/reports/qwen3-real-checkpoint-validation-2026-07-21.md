# Report — native pipeline validated against real Qwen3-0.6B

**Produced:** 2026-07-21.
**Against:** branch `feat/agent-rlvr-base` (PR #857), commit at the tip of the
`zetta::autograd` + `zetta::tokenizer` line.
**Checkpoint:** `Qwen/Qwen3-0.6B` (Hugging Face), downloaded verbatim —
`config.json`, `tokenizer.json`, `model.safetensors` (1,503,300,328 bytes, bf16).

## What this validates

The native, framework-free pipeline — zetta's own BPE tokenizer
([`zetta::tokenizer`](../tokenizer/README.md)) + the Qwen3-parity model on zetta's
own autograd ([`zetta::autograd`](../autograd/README.md), #555) — loads a **real
open-weights Qwen checkpoint** and runs it correctly, end to end. No PyTorch, no
`transformers`, no `tokenizers` in the product path (the HF `tokenizers` library
was used **only** to produce the reference token ids this report checks against).

If any architectural detail were wrong — RoPE convention, GQA head grouping,
QK-norm, SwiGLU order, RMSNorm epsilon placement, the HF `[out,in]` weight
transpose, tied embeddings, the decoupled `head_dim` (128, so 16×128=2048 ≠ the
1024 hidden size), or the bf16 decode — the output would be noise, not correct
facts. It is correct facts.

## Result 1 — tokenizer is bit-exact

`BpeTokenizer::from_directory` loaded the real `tokenizer.json` (vocab 151,669 +
added tokens). On 9 diverse strings its `encode()` produced **token ids identical
to the HF reference** and `decode(encode(x)) == x` on every one:

```
EXACT-MATCH: 9/9
ROUND-TRIP:  9/9
```

Cases covered code, Unicode (`café → π ≈ 3.14159`), contractions (`don't … I'll`),
whitespace/tabs/newlines, numbers, and the chat special tokens (`<|im_start|>` =
151644, `<|im_end|>` = 151645). This exercises the pre-tokenizer (the Qwen regex as
a hand-written scanner), the byte↔unicode map, the BPE merges, and special-token
matching against the real vocab + merges.

## Result 2 — model generates correct text from real weights

`load_qwen_model` decoded the 0.6B bf16 tensors to f32 and built the model
(310 parameter tensors). Greedy decoding (argmax), native forward:

| Prompt | Native completion |
|---|---|
| `The capital of France is` | ` Paris. The capital of Italy` |
| `The opposite of hot is` | ` cold, and the opposite of` |
| `Water is made of hydrogen and` | ` oxygen. The number of mo…` |
| `2 + 2 =` | ` 4, so the sum` |

The next-token argmax after "The capital of France is" is ` Paris` (id 12095).

## Result 3 — the real weights are trainable (post-training works)

Four cross-entropy gradient steps (native autograd backward + `Adam`, lr 1e-4)
teaching the deliberately-wrong continuation "Berlin" flipped the model's greedy
answer:

```
before: "The capital of France is" -> " Paris"
after:  "The capital of France is" -> " Berlin"
```

Gradients flow correctly through the real loaded weights and update them — the
exact forward/backward path `grpo_step` / `grpo_step_text` (RLVR) compose on. This
is post-training working on real open weights.

## Performance (host reference path, single-thread, naive matmul)

| Operation | Measured |
|---|---|
| Load 1.5 GB bf16 → f32 | ~10 s (one-time) |
| Forward, 5-token prompt | 2.3 s |
| Greedy generation | ~0.4 tokens/s |
| Train step (fwd + bwd + Adam) | 7–10 s |

Generation is slow for want of a **KV cache** (each new token re-forwards the whole
growing sequence) and because `lm_head` computes logits for every position over the
full 151,936 vocab. The headroom needs no new architecture: KV cache,
last-position-only `lm_head` at decode, routing matmul through the `ComputeDevice`
`gemm` (optimized CPU backend, or **MLX on the GPU** — the device-agnostic path is
already built), and `parallel_for` across cores.

## Reproduce

Download the checkpoint (`config.json`, `tokenizer.json`, `model.safetensors`) from
`https://huggingface.co/Qwen/Qwen3-0.6B` into a directory, then run the guarded
integration test with that directory:

```
ZETTA_QWEN_DIR=/path/to/qwen3-0.6b ctest --test-dir build -R QwenRealCheckpoint --output-on-failure
```

The test (`tests/tokenizer/test_qwen_real_checkpoint.cpp`) is **skipped** when
`ZETTA_QWEN_DIR` is unset, so the default build/CI needs no multi-GB download.

## Result 4 — throughput (added 2026-07-21, same checkpoint)

Optimizations landed after the correctness pass, all measured on the real
Qwen3-0.6B (16-core Apple-Silicon):

- **`matmul_transposed_b`** (no per-forward weight transpose) + **work-aware
  `parallel_for` grain** (thread the layer matmuls, not just the lm_head): forward
  **2.3 s → 226 ms** (~10×).
- **KV cache** (`QwenInference`): greedy generation **0.4 → 13.5 tok/s** (~34×).
- **MLX GPU (per-op routing):** device-agnostic and correct (max|CPU−MLX| ≈ 6e-5)
  but **not** a generation win — token-by-token decoding drowns in per-op sync
  (1.3 tok/s). It wins **~6×** on *batched* forwards (seq ≥ 128).
- **Fused MLX kernel** (`MlxQwenInference`, whole decode as one MLX graph, KV cache
  in GPU arrays, one eval/token): **42.9 tok/s** — same tokens as CPU, ~3× the
  threaded CPU KV cache. The fast GPU inference path.

## Result 5 — RLVR post-training on the real weights (2026-07-21)

`grpo_step_text` (KV-cached rollouts + sequence policy gradient) run on the real
0.6B, teaching a target word purely from a verifiable "completion contains the
word" reward — never shown the answer:

```
prompt "I think the best pet is a"   target "cat"   (lr 1e-4, group 16, 3 new tokens)
before: greedy = " dog. I"
step 0  mean_reward 0.125   greedy " cat or a"
step 1  mean_reward 1.0     greedy " cat or a"   -> learned; stopped before collapse
```

The model shifted its answer **" dog" → " cat"** in two GRPO steps from reward
alone. Notes: (1) the reward must be *reachable* — a target the policy never samples
(e.g. forcing " Berlin" after "The capital of France is") gives zero gradient; a
multi-token "contains the word" reward on an open prompt fixes that. (2) Unregularized
RLVR at higher lr mode-collapses into degenerate repetition after convergence — a
low lr + early-stop (or a reference-KL penalty, future work) keeps it stable.

## Bottom line

Every code-level layer of "RLVR post-training over open weights" (#555) is done and
**verified against a real Qwen checkpoint**: the tokenizer is bit-exact, the model
loads and generates correctly (42.9 tok/s fused-MLX / 13.5 tok/s CPU), and the real
weights **train from reward** (" dog" → " cat"). What remains is scale (bigger
checkpoints, a reference-KL regularizer, a sustained campaign) — not correctness.
