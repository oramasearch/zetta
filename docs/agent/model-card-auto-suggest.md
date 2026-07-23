# Model card — the Auto-Suggest model

**Model.** **Auto-Suggest** (#554, FEAT-11) — the second model in Orama Studio: reads a customer's
data state and suggests zero-to-many use cases to build. **Status.** Native corpus generator
built (#879); training pending. **Related:** #554 · #879 · #873 · #853.

## Intended use

- **In scope.** The "Discover Opportunities" entry path of Studio: with **no** stated intent, the
  user's data state (tables + columns; optionally a role like "I'm in sales") → an environment
  profile → **zero-to-many** suggested use cases. The user checks off any/all; those become
  prompts fed to the base agent, which builds the pipelines. To the user it's just suggestions —
  they never see "execution graph" or "gate" (the vocabulary contract, root `CLAUDE.md`).
- **Out of scope.** It does NOT build pipelines (that's the base agent) and does NOT act on the
  world. Suggesting a use case whose data is absent is a failure mode the corpus trains against.

## Architecture & training

- **A regular LLM, not the RLVR game-player.** Unlike the base agent, Auto-Suggest is a
  supervised suggestion model — only the intent agent plays the verifiable-reward game. Base
  weights + size are a training-path choice (small, on the customer's hardware, model-agnostic).
- **Training data.** [data-card-auto-suggest.md](data-card-auto-suggest.md) — gated data-state →
  (suggest-present, withhold-absent) cases; discrimination is the product.
- **Objective.** SFT on `data state → the correct suggestion set`, gated so suggestions and
  distractors are disjoint (≥ 1 distractor per sample). Forked + specialized per customer (#853)
  on the customer's own data state.

## Evaluation

- **Discrimination.** Precision/recall of suggestions vs. the supported set; **no** distractor
  (absent-data use case) is suggested; the zero case suggests nothing. Reference passes + controls
  fail via the same admission gate as the base-agent corpus.
- **Report.** Every training/eval run writes a dated validation report to
  [`../reports/`](../reports/README.md) (root `CLAUDE.md` rule).

## Limitations & governance

- Native scenario coverage is an initial subset; the environment-profile model (beyond
  present/absent tables — cardinalities, freshness, role) is a follow-up.
- Trains on customer data **in the customer's environment**; no flow-back; it only *suggests* —
  the human chooses, the base agent builds under its own approval gates.

## See also

[data-card-auto-suggest.md](data-card-auto-suggest.md) · [model-card-base-agent.md](model-card-base-agent.md).
