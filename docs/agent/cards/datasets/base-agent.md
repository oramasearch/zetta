# Data card — the base-agent training corpus

**Dataset.** The Orama Studio auto-AI **base agent's** training corpus — the tasks that teach the
agent to turn a plain-language ask into a production **pipeline**.
**Version.** Native generator, `feat/agent-rlvr-base` (2026-07-23). Supersedes the offline
`data/base-agent-training-set.jsonl` (565 tasks) — see [Sourcing](#sourcing--provenance).
**Owner.** The agent domain (#555). **Related:** #866 (synthesis) · #873 (the reshape epic) ·
#878 (full-plan coverage) · #863 (the fine-tune that consumes it).

## Intent & motivation

The base agent is the entrypoint of Orama Studio: a creator describes a data/AI problem and the
agent builds + runs a correct production pipeline, no ML expertise required. This corpus trains
that behavior. The product's value is **verified production-grade** AI — "any joker can vibe-code
something, but is it correct?" — so the corpus's job is not to teach the agent to *sound* right,
but to make correctness *demonstrated*: every task ships a reference solution that PASSES a
deterministic gate and negative controls that FAIL it. **Discrimination is the product, not the
pass.**

## Method — blender-rl (grow by generation, gate by verification)

Reproduces (does not embed) `homer6/blender-rl`'s dataset strategy:

- **The agent emits an ACTION** — `{message, tool_calls, execution_graph}` (agent/action.hpp) —
  the pipeline it builds. This mirrors blender-rl's "the model emits Blender Python"; the
  engine RUNS the pipeline and the gate scores it. The action is NOT the reward's checks.
- **Verifiable reward.** The reward is the deterministic correctness gate over the *run*
  pipeline — the task's `required` facts, scored as a rubric **active-weight renormalized over
  only the checks that task exercises** (`agent/grader.hpp`, `agent/solution_graph.hpp`). This is
  why a search pipeline (index/retrieval/ranked/grounded) and a churn pipeline (leakage-free/
  temporal-holdout/calibrated/SHAP) are graded on *different* checks — the shape follows the use
  case, never a fixed six stages. No learned reward model.
- **The admission gate.** A task is admitted only if its reference passes (≥ the gate) and its
  negative controls each fail (`agent/admission_gate.hpp`, `admit_corpus`). A corpus where
  nothing can fail measures nothing.
- **Difficulty as data · held-out canary.** `tier` / `difficulty` / `held_out` are fields, so the
  curriculum is a query; a held-out canary of unseen combinations measures generalization
  (planned).

### The #873 action reshape (facts → action)

The base agent was originally SFT'd to emit the *facts* — a drift from blender-rl (where the
agent emits the action and the facts are the reward's checks). The corpus now trains the ACTION:
the SFT target is the reference `{message, tool_calls, execution_graph}` synthesized from each
task's reference facts by the generic **solution-graph bridge** (`build_solution_graph` /
`reference_action_from_facts`), and the facts stay the gate's checks on the run pipeline.
**The facts on disk are unchanged; the action target is derived at render time** — so no corpus
regeneration was needed for the reshape (proven: 565/565 tasks → gradeable actions).

## Sourcing & provenance

Grounded in the **solutions-discovery plan** (`solutions-designs/docs/plan/` — 24 use cases (02),
21 methods + 11 components + the gate (03), 12 solutions (04), the data supply chain (08)):

- **Native generator (current, ADR-17).** `BaseAgentDatasetGenerator` (#873/#878) synthesizes the
  corpus *inside Zetta* from `plan_solution_templates()` (`src/core/dataset/plan_templates.cpp`) —
  each plan subject reduced to the facts its correct pipeline must satisfy, mined from the plan's
  per-method graphs + per-solution "what the gate checks" sections. **No LLM in the product
  path.** This is what a per-customer fork (#853) uses to grow its own corpus from its own data.
- **Offline seed (historical).** `scripts/gen_base_corpus.py` (via `build_catalog.py`) LLM-
  generated tasks per use case (OpenRouter), plus hardcoded SEARCH + ABSTAIN families and the
  native churn seed. It produced `data/base-agent-training-set.jsonl` (git `7c6ff72`, 565 tasks)
  + `base-agent-test-set.jsonl` (117). Retained as reference until the native generator fully
  replaces it.

## Composition

One admission-gated case per subject: `{task {id, description, initial_state, tool_names,
required}, reference, negative_controls}`, plus the derived reference `action` (#873).

- **Coverage (native generator, verified via `generate_dataset base-agent`): 40 gated subjects,
  all admitted** — churn + search (hand-built rich graphs), the supervised solutions S-2…S-12,
  **every method M-10…M-21** (the 11 that had zero coverage in the offline corpus), the remaining
  use cases UC-13…24, the data supply chain (DSC), autonomy (AC), and ABSTAIN. See
  [`training-coverage-checklist.md`](../../training-coverage-checklist.md).
- **Fact vocabulary.** Step facts (`connect`/`clean`/`model`/`index`/`serve`/`act`) + methodology
  facts (`leakage_free`, `point_in_time`, `model_kind`, `temporal_validation`, `calibrated`,
  `explain`, `retrieval`, `ranked`, `grounded_in_data`, `approval_gated`, `abstain`, …).
- **Splits.** `held_out` field; a canary of unseen combinations is planned.

## Uses

- **Intended.** SFT of the base agent (intent → action), then RLVR (`grpo_step` over the native
  gate). The forked, per-customer agent (#853) trains on the customer's own environment.
- **Out of scope.** Not a benchmark of any specific model; not customer data (synthetic +
  plan-derived); the abstain family is illustrative, not an exhaustive safety set.

## Limitations & biases

- **Generated reference actions are structural.** `build_solution_graph` carries a subject's
  facts as node attributes — correct + gradeable, but the pipeline is a skeleton, not a
  hand-tuned graph (churn + search are the hand-built exemplars). Messages are currently generic
  (abstain excepted). Richer graphs/messages are a follow-up.
- **Fact vocabulary is a modeling choice.** The gate checks are our encoding of the plan's
  correctness profiles, not an external standard; they will evolve (a rubric change is a breaking
  dataset change — bump + re-report).
- **Coverage is broad, not deep.** One-to-a-few variants per subject today; even variant counts +
  the canary are the next polish (#878).
- **Historical offline corpus** was LLM-generated (OpenRouter) with heterogeneous fact tags (993
  distinct) — the native generator is deterministic + uniform by contrast.

## Governance & ethics

- **Synthetic + plan-derived — no customer PII.** The base corpus contains no customer data.
- **No egress / consent.** The product trains on customer data *in the customer's environment*;
  a fork's corpus (#853) never leaves it, and the no-flow-back posture governs any Marketplace
  boundary.
- **Reproducible.** Native generation is deterministic (no RNG in the product path); the offline
  generators are `uv run` + env-gated, never a build/CI dependency (ADR-17).

## Maintenance & future

Tracked in the [coverage checklist](../../training-coverage-checklist.md) + #878/#873: even per-subject
variants; a held-out canary; richer reference graphs/messages; the LLM-assist mode for diverse
natural-language intents (needs LLM-client creds, #877); the per-customer fork (#853). Every
training/eval run writes a dated validation report to [`../reports/`](../../../reports/README.md) (root
`CLAUDE.md` rule).

## See also

[model-card-base-agent.md](../models/base-agent.md) · [data-card-auto-suggest.md](auto-suggest.md) ·
[training-runbook.md](../../training-runbook.md) · [inference-runbook.md](../../inference-runbook.md) ·
[build-and-run-runbook.md](../../build-and-run-runbook.md).
