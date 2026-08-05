# The two models — and how the pipeline model trains, checkpoints, and serves

A frequent and important point of confusion: there are **two different models** in the
system, and only one of them is the RLVR agent. This document names both and traces the
lifecycle of the second — the one that actually predicts churn (or whatever the use case
is) — from training through checkpointing to future forward passes.

## Two models, don't conflate them

| | **The agent** | **The pipeline model** |
|---|---|---|
| What it is | a Qwen-class, RLVR-trained tool-calling model | a native histogram-**GBDT** (the churn predictor) |
| What it does | *builds* the execution graph from your intent | the actual predictor *inside* the graph's `Model` step |
| Trained by | GRPO on the verifiable-reward corpus (zetta#866) | the graph, at run time, on **your** data |
| Checkpointed via | `save_qwen_model` | the `Model` entity (below) |
| Reused for | forward passes that emit the next tool call | forward passes that score new rows |

The agent produces the **graph**; the graph produces the trained, checkpointed **pipeline
model**. Everything below is about the second one.

## 1. It trains when the graph runs

The graph's `Model` step runs a native histogram-GBDT **`train` ComputeJob**
(`ml/gbdt_train_job.hpp`) — a *"recorded, durable, audited unit of accelerated compute
that appears in `system.jobs` and produces a `Model`."* It is:

- **distributed** — SPMD over sharded feature tables, combined by gradient-histogram
  `allreduce`, so a single-node run and a multi-node run grow the **same trees** (single ==
  multi by construction);
- **device-backed** — the feature columns live on the frame's `ComputeDevice` (CPU or CUDA);
- **complete** — paired with SHAP explanations (`ml/tree_shap.hpp`) and calibration
  (Platt / isotonic, `ml/calibration.cpp`), so the model is explainable and its scores are
  probabilities you can act on.

## 2. It is checkpointed (durably, and it replicates)

The trained model is persisted as a **`Model` entity**:

- a small descriptor rides the CRDT as **`model:<uuid>`**, carrying `ModelWeights` =
  **byte size + SHA-256** fingerprint;
- the **serialized weights body** itself is stored **off the CRDT** — on disk, transferred
  peer-to-peer, verified against size + sha256. The GBDT struct *is* the serialized body,
  and *"a calibrated model carries its calibration in its own weights body."*

So the checkpoint is durable (survives restarts — `Node::tick` reconciles it from the
document), fingerprinted (tamper-evident), and **replicated** to peers like all other
state.

## 3. It serves future forward passes

The checkpoint is loaded for scoring — `ml/gbdt_score.hpp` / a `score` ComputeJob — and
served behind `expose_model` → `POST /v1/models/{name}/predict` (`serve:predict`). Future
scoring on new accounts loads the persisted weights and runs forward passes; the
calibration loop compares scores to realized outcomes over time.

## The honest nuance — what's built vs. the integration seam

- The **primitives** — train → checkpoint → score/serve, distributed, calibrated,
  explained — are **built and real**, and the UC-1 churn path
  (`ml/local_featurize_train_job.hpp`, the `docs/use-cases/uc1-*` slices) exercises the
  real train → persist → score chain.
- The graph orchestrator's **default** step runner (`pipeline/executor.hpp`
  `DefaultStepExecutor`) is explicitly a **reference stub** — its comment says it *"does
  not (yet) train a real model or build a real index."* The real GBDT training plugs in
  **behind the `StepExecutor` seam** as the ComputeJob above.
- So the remaining integration is wiring the **generic** graph `Model` step to invoke
  `gbdt_train_job` through that seam — which is exactly what the seam exists for, not a
  rewrite.

## The full loop

The agent builds a gate-passing graph → the graph's `Model` step **trains** the GBDT on
your data → **checkpoints** it (`model:<uuid>` + a fingerprinted weights blob) → **serves**
it for scoring → the calibration loop feeds measured outcomes back as signal.

## Code pointers

| Stage | Code |
|---|---|
| Train (distributed GBDT) | `ml/gbdt_train_job.hpp`, `ml/gbdt.hpp` |
| Explain / calibrate | `ml/tree_shap.hpp`, `ml/calibration.cpp` |
| Checkpoint (Model entity + weights) | `ml/model.hpp` (`model:<uuid>`, `ModelWeights`) |
| Score / serve | `ml/gbdt_score.hpp`, `api/serving.hpp` (`expose_model`, `/predict`) |
| The graph step seam | `pipeline/executor.hpp` (`StepExecutor` / `DefaultStepExecutor`) |

See also [`rlvr-to-production.md`](rlvr-to-production.md) (how the corpus → RLVR → the
correctness gate) and [`how-orama-works-plain.md`](how-orama-works-plain.md) (the
non-technical version).
