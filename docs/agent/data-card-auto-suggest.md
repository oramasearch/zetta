# Data card — the Auto-Suggest training corpus

**Dataset.** The **Auto-Suggest** model's corpus (#554, FEAT-11) — teaches the model to read a
customer's data state and suggest zero-to-many use cases (each a prompt that feeds the base
agent). **Version.** Native generator, `feat/agent-rlvr-base` (2026-07-23); offline seed
`data/autosuggest-training-set.jsonl` (500 samples, `scripts/gen_autosuggest_corpus.py`).
**Related:** #554 · #879 (native generator) · #873 · #853.

## Intent & motivation

Solves *"I don't even know what to ask."* Auto-Suggest is a **separate model** from the pipeline-
building agent (a regular LLM, not the RLVR game-player): it scans the environment → a profile →
suggests use cases the user can check off, which then feed the intent agent. The corpus makes the
suggestions **verifiable**: a data state is constructed to support a KNOWN set of use cases (the
correct suggestions), with DISTRACTOR use cases whose data is ABSENT and must NOT be suggested —
**that absence is the discrimination signal**.

## Method

Same blender-lr discipline as the base-agent corpus ([data-card-base-agent.md](data-card-base-agent.md)):
grow by generation, gate by verification. Modeled as **admission-gated cases** so it rides the
same gate: `task` = the data state (`initial_state`), `required` = suggest the present use cases
(true) **and** withhold the absent ones (false); `reference` = the correct suggestion set;
`negative_controls` = dropping a correct suggestion or adding a distractor — each fails the
grader. The gate contract: suggestions ∩ distractors = ∅, and ≥ 1 distractor per sample.

## Sourcing & provenance

Grounded in the same solutions catalog (`build_catalog.py`). Scenarios rotate **single / multi /
zero** (garbage data supports nothing → suggest nothing), and include the **search/retrieval**
use case (a document/text corpus → suggest upload → index → serve; a distractor when only numeric
data is present).

- **Native generator (current, ADR-17).** `AutoSuggestDatasetGenerator` (#879) synthesizes gated
  data-state scenarios inside Zetta — churn data, document corpus, time-series, rich (multi),
  empty. A per-customer fork (#853) grows its own from the customer's real data state.
- **Offline seed (historical).** `scripts/gen_autosuggest_corpus.py` (OpenRouter), validated
  against `scripts/schemas/autosuggest.schema.json`, produced the 500-sample JSONL. Shape:
  `{data_state, suggestions, distractors, tier, difficulty, held_out, tags}`.

## Composition, uses, limitations, governance

- **Composition.** Gated data-state → (suggest-present, withhold-absent) cases; grows toward the
  full FEAT-11 scenario set (single/multi/zero across the catalog + UC-25 search).
- **Uses.** SFT of the Auto-Suggest model (data state → suggestions). Its output feeds the base
  agent — no duplication of the prompt→pipeline translation.
- **Limitations.** Native scenario set is a starting subset (5 scenarios) vs. the offline 500;
  the environment-profile modeling (beyond present/absent tables) is a follow-up.
- **Governance.** Synthetic + plan-derived, no customer PII; a fork's corpus stays in the
  customer's environment; every training/eval run writes a dated report to
  [`../reports/`](../reports/README.md).

## See also

[model-card-auto-suggest.md](model-card-auto-suggest.md) · [data-card-base-agent.md](data-card-base-agent.md).
