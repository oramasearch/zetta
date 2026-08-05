# Data card — the base-agent training corpus

**Dataset.** The Orama Studio auto-AI **base agent's** training corpus — the tasks that teach the
agent to handle the interactions a user actually has with Studio: turning a plain-language ask
into a production **pipeline**, answering about a Project from retrieved facts, changing a
Pipeline, writing code inside a node, declining, and asking for clarification.
**Version.** v4 — **every Project is a use case**, 2026-07-27. **10,026 gated cases across 11
families**, 1,094 canary. Supersedes v3 (6,102 cases), which trained a checkpoint to a loss of
0.0001 and **47% grader pass on held-out cases** — see [Limitations](#limitations--biases).
Supersedes v2 (withdrawn) and v1 — see [Sourcing](#sourcing--provenance).
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

## Where it lives

**Published, privately, to the Hub** —
[`OramaSearch/base-agent-interaction-corpus`](https://huggingface.co/datasets/OramaSearch/base-agent-interaction-corpus).
Every version and its canary. Not in git: they were once ignored as "reproducible from the native
generator", which is false the moment the generator changes — regenerating from a later commit
yields a *different* corpus, so a checkpoint whose corpus was not published cannot be reproduced or
audited. **The corpus is the provenance of the model.** Procedure: the **hf-ops** skill.

This card is the in-repo datasheet; the Hub card is the same content for a reader outside the repo.
**When one changes, change both.**

## Composition

Each case is `{task {id, description, initial_state, tool_names, required}, reference,
negative_controls}`, plus the reference `action` (#873) and the **`grounding`** the harness would
have retrieved before that ask (#918). The grounding is half of a grounded-QA example: a corpus
that dropped it would present "answer using ONLY these facts" followed by no facts, training the
model to answer from priors — the exact fabrication this corpus exists to remove.

### v2 — the interaction corpus (#918)

The agent is harness code with a model attached, so the corpus is the recorded input/output of
the interactions the harness runs. Full contract:
[`../../interaction-contracts.md`](../../interaction-contracts.md).

| Family | Mode | Cases | Graded on |
|---|---|---|---|
| `qa_results` | answer | 960 | numbers/entities ⊆ context, rank order |
| `qa_data` | answer | 540 | same |
| `platform` | answer | 720 | same (cluster, jobs, run failures) |
| `capability_discovery` | answer | 102 | the capability exists in the supplied registry listing |
| `abstain` | abstain | 186 | declined and built nothing |
| `clarify` | clarify | 240 | exactly one question, built nothing |
| `feature_engineering` | action | 1,080 | the gate; leakage-free + point-in-time preserved |
| `code_in_node` | action | 1,200 | **executing** the generated Python/SQL |
| `modify` | action | 900 | the gate; changed only what was asked |
| `governance` | action + answer | 720 | the capability actually requested |
| `product_knowledge` | answer | 72 | the named capability exists in the supplied listing |

Plus the plan subjects (churn, search, S-2…S-12, M-10…M-21, UC-13…24, DSC, AC, ABSTAIN).

**Where the volume comes from.** A deterministic cross product of subjects × contexts × phrasings
— no RNG, so the corpus is reproducible. The multiplier that matters is **context variation**:
hold the question fixed and vary the retrieved facts, and the example becomes *unanswerable
without reading them*. Trailing contexts are the canary, so a model that memorized answers rather
than reading the supplied facts fails outright.

### What changed in v4 (2026-07-27) — the corpus stopped teaching one pipeline

v3 trained cleanly and generalized badly: loss 0.0001, **47% grader pass on the canary**, and the
same five-step pipeline emitted for churn, lead scoring and collections alike. The corpus explains
it exactly — 3,139 action cases carried only **10 pipeline shapes, 92% of them two**, and the whole
target set held **8.74 bits of entropy**, roughly 430 effective outcomes for a 600M-parameter model
to memorize. Low loss was never evidence of learning; there was very little to learn. Procedure:
[`../../corpus-diversity-runbook.md`](../../corpus-diversity-runbook.md).

**Every synthetic Project now IS a use case.** `ProjectContext` carries the solution template it
represents, and the feature, modify and code families build on that Project's real pipeline instead
of a shared supervised skeleton. A code step requested on a forecasting Project lands inside the
forecasting pipeline — backtested, uncalibrated — rather than collapsing it to
`connect→clean→serve`, which alone had been 34% of all action cases.

**The templates were corrected against their own descriptions**, which found real defects rather
than just adding variety: S-12 promised to "route the risky ones for review" and had no `act` step;
S-10 promised to "flag likely fraud for review" and had none either. S-8, M-13, M-21, UC-18, UC-19,
UC-21 and UC-24 shared the error. Each now carries the action step its description requires,
**approval-gated** — a fraud model that auto-blocks loses good customers. M-11 and UC-24 index what
they extract, because fields nobody can find are not touchless.

**Replies say what they did.** Action messages describe the pipeline built rather than repeating one
constant across 1,802 cases; governance, abstain and run-status answers name the Project and the
capability. A confirmation that does not say what was queued is not one a user can check.

**Contexts 30 → 50.** At 30 they could not reach all 40 templates, so a quarter of the catalogue
never appeared as a Project.

| | v3 | v4 |
|---|---|---|
| cases | 6,102 | 10,026 |
| distinct targets | 715 | **2,111** |
| **target entropy** | 8.74 bits | **10.65 bits** |
| distinct pipeline shapes | 10 | 12 |
| dominant shape share | 57.4% | **34.8%** |
| distinct messages | 681 | 1,360 |

Guarded by two new rules: no pipeline shape may exceed 45% of action cases, and target entropy must
clear 9.5 bits. **Both thresholds are locally calibrated, not published standards** — the
tool-calling corpus literature (ToolBench, APIGen, ToolMind, xLAM, τ-bench) argues for diversity
consistently but reports no entropy figure to compare against.

### What changed in v3 (2026-07-26)

**Every `action` carries the tool calls that build its Pipeline.** The agent is a tool-calling
harness — the model decides which steps to call, the harness executes them and mints node ids —
and the corpus did not encode that. `make_case` took `graph` and `tool_calls` as independent
defaults, so families passed a graph and silently omitted the calls: **2,862 of 3,139 action cases
(91%) declared a real Pipeline and called nothing.** Every case was individually valid and the
corpus admitted cleanly; the *distribution* was the defect, and the checkpoint trained on it
returned a fluent message with `tool_calls: []` and built no Pipeline. The calls are now derived
from the graph (`tool_calls_from_graph`, the exact inverse of `build_pipeline_from_tool_calls`),
so the two encodings cannot disagree. Zero action cases now lack calls
(https://github.com/oramasearch/zetta/issues/933).

**Targets no longer contain node ids.** `render_action_example` strips `execution_graph`. Node ids
are random UUIDs; a model asked to write them can only learn what one looks like, which is exactly
what happened — the checkpoint emitted `11111111-…` for every node, colliding two steps on one id.
Mean target: 103 → 44 tokens.

**Replies vary with the input.** Two families answered every case with one constant sentence —
`feature_engineering` across 1,080 cases and `modify` across 900. A constant is the cheapest target
to memorize, and the checkpoint memorized it, replaying corpus messages verbatim for unrelated
asks. Both now name the actual change and the actual table. Distinct messages: **281 → 681**
(4.1% → 11.2% of cases).

**The canary is genuinely held out.** Families that do not vary with a `ProjectContext` were
generated identically for both splits, putting **105 duplicate cases** in the canary, where they
measured memorization and reported it as generalization. The canary is now deduplicated against
training.

**New: `product_knowledge`.** 72 cases covering what Studio is, the languages, the sources, the
models, the deploy targets, Verification and Saved Versions, MCP and REST, and the use cases —
turns that carry no Pipeline and call no tools. Grounded in a capability listing so that naming a
feature which does not exist is a graded failure; overselling is this family's specific
fabrication risk. Every entry is verified against source
(https://github.com/oramasearch/zetta/issues/935).

**The corpus is now guarded by 27 rules** over the distribution as a whole
(`tests/dataset/test_corpus_invariants.cpp`) — shape, learnability, anti-fabrication,
verifiability, balance, and split hygiene. Per-case validation could not catch any of the above
(https://github.com/oramasearch/zetta/issues/934).

**A required fact must be projectable.** `response_solution_facts` defines what a family may
require — you cannot verify what you cannot check, and a corpus requiring unverifiable facts
silently stops measuring anything. This is why abstention does not require `in_scope` (a property
of the ask, not of the answer), and why governance is graded on the capability called rather than
on prose claiming an approval happened.

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

- **v2 is WITHDRAWN for training.** 91% of its `action` cases carried a Pipeline with an empty
  `tool_calls`, so it taught the agent that an action calls nothing; it also serialized random node
  ids first in the target, burning the token budget on unlearnable hex. Any checkpoint trained on
  v2 exhibits both and should not be read as evidence about the model
  (https://github.com/oramasearch/zetta/issues/933). v3 fixes both.
- **v3 is superseded and should not be trained on.** It taught 10 pipeline shapes across 3,139
  action cases and produced a checkpoint that emitted one pipeline for every ask (47% grader pass).
- **Techniques from the literature not yet applied.** No distractor tools (tools present but wrong
  for the ask, so the model must select rather than recall), no parallel/multi-call query styles, no
  multi-turn tool results, and the emitted code is graded structurally rather than by executing it.
  These are the next diversity axes, recorded in the runbook.
- **Corpus size overstates corpus information.** 6,102 cases contain **354 distinct asks** and
  **184 distinct grounding blocks** — the cross product presents roughly 354 problems about 19
  ways each. Repetition against *varying* context is the design and is load-bearing (it is the
  anti-fabrication signal, asserted by `RepeatedAsksAppearAgainstDifferentContext`), but a model
  that memorizes 354 mappings will score well on training while having learned much less than the
  row count suggests. Deciding whether to grow the ask vocabulary, raise the context count, or cut
  phrasings is https://github.com/oramasearch/zetta/issues/937 — and it should be settled before
  any decision to scale the model.
- **The `product_knowledge` entry list is hand-maintained** and will drift from what the platform
  actually does. Every entry was verified against source when written, but nothing enforces that
  over time; deriving the list from the capability/connector/provisioner registries is the fix.
  An entry added without the code to back it trains the agent to lie about the product.
- **Generated reference actions are structural.** `build_solution_graph` carries a subject's
  facts as node attributes — correct + gradeable, but the pipeline is a skeleton, not a
  hand-tuned graph (churn + search are the hand-built exemplars). Messages are currently generic
  (abstain excepted). Richer graphs/messages are a follow-up.
- **Fact vocabulary is a modeling choice.** The gate checks are our encoding of the plan's
  correctness profiles, not an external standard; they will evolve (a rubric change is a breaking
  dataset change — bump + re-report).
- **Coverage is broad, and now deep on interactions but not on phrasing.** v2 (#918) fixed the
  "one case per subject" problem — 6,793 cases across 10 interaction families with a real canary.
  The remaining thinness is **paraphrase diversity**: asks are drawn from fixed template banks
  (3–4 phrasings each), so the corpus teaches the interaction well and natural-language variety
  less well. Deterministic template paraphrase is the ADR-17-clean way to widen it; the
  LLM-assist mode (#877) is the alternative and needs credentials.
- **Synthetic contexts, not customer data.** The grounded-QA contexts are generated Projects with
  believable measures, drivers, and accounts — deliberately never a perfect measure, since AUC
  1.000 reads as leakage to the technical buyer. They exercise the *shape* of real retrieval; a
  per-customer fork (#853) regenerates them from the customer's own node.
- **Code cases are graded on execution, but the executor is not yet wired into generation.** The
  `code_in_node` family carries the language and the expected output column as facts; running the
  emitted code as the reward is the RLVR step, not the SFT step.
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
