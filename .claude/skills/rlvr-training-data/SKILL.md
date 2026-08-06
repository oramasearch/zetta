---
name: rlvr-training-data
description: Generate verifiable-reward (RLVR) training data — the task/game corpora that train zetta's native agents — following blender-rl's "grow by generation, gate by verification" method. Use when creating or growing a training corpus for the base agent or a per-customer fork, synthesizing tasks from a solutions catalog or a customer's data, authoring reference solutions + negative controls, running the admission gate, tagging difficulty, holding out a generalization slice, pushing to a HuggingFace dataset, or generating synthetic data (e.g. into Postgres) in a customer's schema. Triggers — "training data", "generate a corpus", "RLVR games", "reference solution", "admission gate", "synthetic data", "curriculum", "push to HF dataset", "train a future model".
---

# rlvr-training-data — build verifiable-reward training corpora

zetta trains its own agents with **RLVR** (reinforcement learning with verifiable
rewards). The agent is only as good as the **games** it practises on: a game is
*trainable* only if a deterministic grader can score it, and a corpus is *trustworthy*
only if it **discriminates** — reference solutions pass, plausible-wrong attempts fail.
This skill is how you produce that corpus, for the shipped base agent (#555) or a
per-customer fork (#853).

**Reference to reproduce, not embed:** `homer6/blender-rl` — a validated RLVR loop whose
dataset strategy (`docs/dataset_strategy.md`, `environments/blender_creation/task_generators.py`,
`environments/blender_creation/tests/admission_gate.py`, `scripts/export_dataset.py`) is
the method this skill distills. Also load **native-ai-dev** (building the model + RLVR
loop the corpus trains) and the module dev conventions (**zetta-dev**).

## The method — grow by generation, gate by verification (five principles)

Non-negotiable, straight from blender-rl's dataset strategy:

1. **Every task ships a validated reference solution.** A task isn't "done" until a
   reference scores high through the **live** grader *and* plausible-wrong attempts score
   materially lower. **Discrimination is the product, not the pass.**
2. **Difficulty is a data field, not a folder.** Each task carries `difficulty` / tier /
   skill-tags; the curriculum is a *query* over that metadata, re-sliceable without
   re-authoring.
3. **Rewards co-evolve with difficulty.** Harder tasks add harder *checks*, not just a
   longer prompt. A tier that reuses the old checks isn't harder.
4. **Grow by generation, gate by verification.** Author cheaply (templates, frontier-model
   assistance) but admit only what passes the reference + discrimination gate. **Volume
   without gating poisons the reward signal.**
5. **Hold out to measure generalization.** Some difficulty must never appear in training,
   or you can't tell learning from memorization.

## The zetta mapping (the native contract)

| blender-rl | zetta (`zetta::agent`, `zetta::ir`) |
|---|---|
| Task = prompt + grader contract in `info` | `Task` — description + `initial_state` + `tool_names` + `required` `EndState` + `grader()` (`EndStateGrader`) |
| Reference solution scores ≥ 0.95 | reference `EndState` **passes** the task's grader |
| Plausible-wrong attempts score low | `negative_controls` — each **fails** the grader |
| The admission gate | `admit_corpus(vector<AdmissionCase>)` — rejects untrainable tasks, rejected references, accepted negatives, or a corpus with **no** negatives |
| The reference *script* that builds the artifact | the reference **execution graph** (`ir::ExecutionGraph`) + the `Connect→Clean→Model→Index→Serve→Act` mutation **trajectory** that builds it |
| JSONL rows, grader contract in `info` | `Task`/`AdmissionCase` JSONL; the corpus loads as a HF dataset |

A task with **no required facts is untrainable** (nothing to verify) — the constraint #554
(Auto-Suggest) enforces before a task may enter the curriculum.

## Workflow

### Author one task (the atom)
1. **State the required end-state first** — the *facts* a correct solution must produce
   (a churn pipeline: connect · leakage-free clean · **gbdt + SHAP** model · calibrated
   serve/act). If you can't name the checkable facts, the task isn't trainable yet.
2. **Reference solution** — build the artifact (the execution graph) so its facts pass the
   grader. Ground the facts in a **real** `ir::ExecutionGraph`, not hand-written strings,
   wherever you can.
3. **Negative controls** — plausible-wrong solutions that each fail ≥ 1 required fact (an
   LLM-only pipeline, a leaky one, a no-calibration one). Discrimination lives here.
4. **Gate it** — `admit_corpus` must admit the case; assert each negative control
   *individually* fails the grader. Red-then-green, like any nibble.
5. **Tag difficulty** + decide whether it's held out.

### Grow the corpus
- **From a solutions catalog** (the base agent): per use case, ground the real-world data
  shape (**Perplexity**) → generate N tasks + reference trajectories (a frontier model via
  **OpenRouter**) → **run every one through the admission gate before it enters the
  corpus** → push to a **private HF dataset**. All creds/config via **env vars**
  (`HF_TOKEN`, `HF_DATASET_REPO`, `OPENROUTER_API_KEY`, `PERPLEXITY_API_KEY`); nothing is
  contacted except when a script is run.
- **From a customer's data** (a fork): synthesize data in the customer's **exact schema**
  (a frontier model → local Postgres) with realistic signal, so the fork trains near
  real-shaped data with **no live-source dependency**.

### Build the corpus machinery with nibble-dev
Bottom-up, most-concrete first: the discriminating task → facts from a real graph → the
reference trajectory → JSONL (de)serialization → difficulty / held-out → the generators.
Each a red → green → build → atomic-commit bite. See **nibble-dev**.

## Never
- ❌ A task with no verifiable required facts (untrainable).
- ❌ A corpus with no negative controls (the gate catches nothing).
- ❌ Volume without gating (poisons the reward signal).
- ❌ A generator that pushes to HF **before** the admission gate.
- ❌ Hardcoded secrets — creds are env vars. Foreign models/services are **authoring aids
  only**, never in the train/infer path (ADR-17: the model is the product).

## Related skills
- **native-ai-dev** — build the model / RLVR loop the corpus trains (load with this).
- **nibble-dev** — the atomic-commit discipline for the corpus machinery.
- **new-connector** / **new-input** — real data sources / input formats a task may consume.

## Version History
- v1.0.0 (2026-07-21): Initial — distilled from blender-rl's dataset strategy + the
  base-agent training-corpus build (#866 → #863 → #862; Charles/Motive churn as sample #1).
