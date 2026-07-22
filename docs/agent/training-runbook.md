# Runbook — training the base agent (the fine-tune run)

How to turn the generated corpus into a trained base-agent checkpoint (#863 → #555). This
is the step *after* the data is generated — see the
[training-data runbook](training-data-runbook.md) for producing `data/corpus.jsonl`. The
method + why it works: [`rlvr-to-production.md`](rlvr-to-production.md); the two models
(agent vs. pipeline model): [`model-lifecycle.md`](model-lifecycle.md).

> **Status (2026-07-22).** The per-step training machinery is **built and validated on the
> real Qwen3-0.6B**; the full-corpus training driver + install + eval are the **in-flight
> #863/#859 work**. Each step below marks what's ready vs. in progress.

## Prerequisites

- **The corpus:** `data/corpus.jsonl` (494 gated tasks; committed) — see the data runbook.
- **The weights:** Qwen3-0.6B (validated), Qwen3-8B (downloaded). Point `ZETTA_QWEN_DIR`
  at the checkpoint dir (the guarded tests use it).
- **The build:** MLX-on for the fast GPU path (`cmake -S . -B build -DZETTA_ENABLE_MLX=ON`);
  the default CPU build always works, just slower.

## Which path

| Path | Model | Where | Ticket |
|---|---|---|---|
| **A (now)** | full fine-tune **Qwen3-0.6B** | this box (64 GB, fits, validated) | #863 |
| B | **8B + LoRA** | this box (needs LoRA built) | #864 |
| C | full **8B** | a dedicated Linux/CUDA box | #865 |
| — | *why 0.6B first:* a full 8B fine-tune needs ~128 GB (params+grads+Adam) — see #862. |

## Step 0 — re-gate the corpus with the native authority

The Python generator's gate is a pre-filter; before training, confirm the corpus passes
the **native** `admit_corpus` (the authority training uses):

```bash
ZETTA_CORPUS=data/corpus.jsonl ./build/tests/zetta-tests \
  --gtest_filter='CorpusNativeGate.*'
```

Expected: *"494 tasks loaded … 0 rejected"* — every task loads via `agent/corpus_io.hpp`
and is admitted (reference passes, negatives each fail). **Ready.**

## Step 1 — the training run (0.6B)

The per-step machinery (all **ready**, on `compute::model::MlxQwenModel`):

- **SFT-first** (fastest, most reliable): `next_token_loss` + `AdamW` over each task's
  intent → reference solution. Loss drops through real backprop on Metal (~290 ms/step).
- **RLVR fast-follow:** `grpo_step` — KV-cached rollouts → the deterministic grader
  (`agent/grader.hpp`, the task's `required` facts) → group-relative advantages → one
  policy-gradient step. Stable at a moderate lr (~5× the CPU path). Same corpus tasks;
  the grader *is* the reward.

**In progress (#863/#859):** the driver that loads the whole corpus (`load_corpus_jsonl`),
renders each task into a training example, and loops SFT→RLVR over all 494 — plus wiring
the neural policy to the graph-building sandbox so `grpo_step`'s reward is the native
grader over the agent's tool calls (#859). Until that lands, training is exercised on the
validated per-task path (the `FusedMlxTraining*` / `FusedGpuRlvr*` guarded tests).

## Step 2 — checkpoint

`save_qwen_model(model, dir)` writes the fine-tuned checkpoint (config + safetensors,
HF-named), loadable by `load_qwen_model` / `MlxQwenModel::from_directory` for the next
round and for serving. **Ready.**

## Step 3 — install as the main agent

`expose_model` flips the served-model flag (`api/serving.hpp`); the agent answers on the
serving path. **In progress** — the base-agent serving wiring is the tail of #863/#862.

## Step 4 — evaluate (the acceptance)

The #555 acceptance: the checkpoint **measurably beats the open-weights start** on the
#248 enterprise tool-calling eval, with the admission gate green (reference solutions
pass, negative controls fail). #859 verifies the environment drives the neural policy
end-to-end first. Record the run in `docs/reports/`.

## Related

- [training-data-runbook.md](training-data-runbook.md) — generate the corpus first.
- **native-ai-dev** skill — the training/RLVR machinery (`MlxQwenModel`, `grpo_step`,
  checkpoints, `-march=native`, KV cache, fused kernels).
- #863 (0.6B, now) · #864/#865 (8B) · #859 (env wiring) · #858/#248 (the campaign + eval)
  · #866 (the corpus).
