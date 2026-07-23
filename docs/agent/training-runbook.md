# Runbook — training the base agent (the fine-tune run)

How to turn the generated corpus into a trained base-agent checkpoint (#863 → #555). This
is the step *after* the data is generated — see the
[training-data runbook](training-data-runbook.md) for producing `data/base-agent-training-set.jsonl`. The
method + why it works: [`rlvr-to-production.md`](rlvr-to-production.md); the two models
(agent vs. pipeline model): [`model-lifecycle.md`](model-lifecycle.md).

> **Status (2026-07-22).** The **SFT training driver is built** (Step 1) — it trains the
> 0.6B on the corpus and saves a reloadable checkpoint (`MlxQwenModel::save`). The RLVR
> neural-policy wire-up (#859), install (Step 3), and the #248 acceptance eval remain in
> flight. Each step marks what's ready vs. in progress.

## Prerequisites

- **The corpus:** `data/base-agent-training-set.jsonl` (494 gated tasks; committed) — see the data runbook.
- **The weights:** Qwen3-0.6B (validated), Qwen3-8B (downloaded). Pull them **natively**
  (no Python/curl; ADR-17) and point `ZETTA_QWEN_DIR` at the printed directory:

  ```bash
  ZETTA_QWEN_DIR=$(./build/bin/zetta pull Qwen/Qwen3-0.6B)   # -> ./models/Qwen3-0.6B
  ```

  `zetta pull` fetches config + tokenizer + safetensors (single or sharded) over the
  engine's own HTTPS client, following the LFS redirect and streaming each shard to disk
  (`connect/model_fetch.hpp`). Set `HF_TOKEN` for a gated repo; `--dir PATH` to choose where.
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
ZETTA_CORPUS=data/base-agent-training-set.jsonl ./build/tests/zetta-tests \
  --gtest_filter='CorpusNativeGate.*'
```

Expected: *"494 tasks loaded … 0 rejected"* — every task loads via `agent/corpus_io.hpp`
and is admitted (reference passes, negatives each fail). **Ready.**

## Step 1 — the training run (0.6B) — **ready**

The SFT driver `SftBaseAgentOnCorpus` (a guarded test in
`tests/tokenizer/test_qwen_real_checkpoint.cpp`) loads the 0.6B, re-gates the corpus with
the native authority, renders each task as **intent → the required-facts solution**, and
fine-tunes with `next_token_loss` + `AdamW` on the GPU (~290 ms/step), then saves + reloads
the checkpoint.

```bash
# a full pass over the corpus (raise steps for more epochs); MLX build required
ZETTA_QWEN_DIR=<qwen3-0.6b> ZETTA_CORPUS=data/base-agent-training-set.jsonl \
  ZETTA_SFT_EXAMPLES=494 ZETTA_SFT_STEPS=1500 \
  ZETTA_CHECKPOINT_OUT=data/checkpoints/base-agent-0.6b \
  ./build/tests/zetta-tests --gtest_filter='QwenRealCheckpoint.SftBaseAgentOnCorpus'
```

Expected: the loss drops (a 30-step smoke run went 4.6 → 0.4) and a complete, reloadable
checkpoint lands in `ZETTA_CHECKPOINT_OUT`.

- **RLVR fast-follow (#859):** `grpo_step` — KV-cached rollouts → the deterministic grader
  (`agent/grader.hpp`, the task's `required` facts) → group-relative advantages → one
  policy-gradient step (stable at a moderate lr, ~5× the CPU path). The remaining wire-up
  is the neural policy → the graph-building sandbox so the reward is the native grader over
  the agent's tool calls; the per-step RLVR machinery is validated (`FusedGpuRlvr*`).

## Step 2 — checkpoint — **ready**

`MlxQwenModel::save(out_dir, source_dir)` writes the fine-tuned weights as
`model.safetensors` (re-attaching the HF tensor names) and copies `config.json` + the
tokenizer, so `from_directory(out_dir)` reloads the trained base agent — the SFT driver
above does this at the end. (`save_qwen_model` is the CPU-path equivalent.)

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
