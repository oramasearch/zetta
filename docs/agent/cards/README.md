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

## Read the card before you change — or diagnose — a model

A card is not documentation you write afterwards; it is the **first thing you read** when you
touch a model, its corpus, its prompts, its decode path, or its serving surface — and before
you claim a model "is broken", "needs a retrain", or "outputs the wrong thing". It states the
**intended use, the out-of-scope uses, the trained output shape, the known limitations, and the
acceptance metrics**, so it is what distinguishes a real defect from the model doing exactly
what it was designed to do.

Skipping this step produces a confident, wrong diagnosis: **designed behavior gets filed as a
bug, and the proposed fix is a retrain that was never needed.** The base agent is the standing
example — its card says it emits a `{message, tool_calls, execution_graph}` **action** and is
explicitly *"not a general chatbot"*, so raw JSON from it is the contract being honored, and the
prose reply is already the `message` field (`parse_studio_action`).

**Before proposing a corpus or weight change, name the card section that makes the current
behavior wrong. If there isn't one, the bug is in the calling code, not the model.** Repo-wide
statement of this rule: the root [`CLAUDE.md`](../../../CLAUDE.md) progressive-disclosure table.
