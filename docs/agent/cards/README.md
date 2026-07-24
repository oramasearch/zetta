# cards — data cards + model cards

Honest datasheets ([Datasheets for Datasets] style) + model cards for the agent domain's models
and their training corpora (#880). A card states **what** the artifact is, **how** it was made,
its **coverage/limits**, and its **governance** — the report is the deliverable, not the
checkpoint.

## Models — [`models/`](models/)

- [`base-agent.md`](models/base-agent.md) — the Orama Studio base agent (#555): intent → a
  production pipeline (`{message, tool_calls, execution_graph}`), Qwen3-0.6B, native, SFT → RLVR.
- [`auto-suggest.md`](models/auto-suggest.md) — Auto-Suggest (#554): a data state → zero-to-many
  suggested use cases (the second model; a regular LLM, not the RLVR agent).

## Datasets — [`datasets/`](datasets/)

- [`base-agent.md`](datasets/base-agent.md) — the base agent's admission-gated, plan-grounded
  corpus, in the action shape. Coverage: [`../training-coverage-checklist.md`](../training-coverage-checklist.md).
- [`auto-suggest.md`](datasets/auto-suggest.md) — the Auto-Suggest corpus (data state → suggest
  present / withhold absent distractors).

Both corpora are generated natively in Zetta by the dataset generators (#873/#878/#879) and
gated by `admit_corpus`. Cards are updated in the same change as a training-data or model change.
