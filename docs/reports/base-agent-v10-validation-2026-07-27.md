# Validation report — base agent v10 (multi-turn + teacher answers)

**Date.** 2026-07-27 · **Checkpoint.** `base-agent-0.6b-interaction-v10`
([Hub](https://huggingface.co/OramaSearch/base-agent-0.6b), `v10/`)
**Corpus.** `interaction-corpus-v10.jsonl` — 15,069 cases, 1,609 canary
**Base.** Qwen3-0.6B · **Hardware.** A100-SXM4-40GB
**Related.** [#941](https://github.com/oramasearch/zetta/issues/941) ·
[#942](https://github.com/oramasearch/zetta/issues/942) ·
[#939](https://github.com/oramasearch/zetta/issues/939)

## What was tested

Three changes landed at once, so **no result here is attributable to one of them** without an
ablation that was not run: multi-turn follow-ups (180 cases, with tool outcomes carried in history),
teacher-authored objection answers (five correct answers per pressed-on question instead of one),
and ungrounded coverage for the turns a conversation opens with.

## Training

| | |
|---|---|
| Steps | 15,100 (one epoch) |
| Loss | **3.256 → 0.00072** (min 2.36e-06) |

## Canary — 80 cases, strided

| Metric | v9 | **v10** |
|---|---|---|
| Parse rate | 100% | **100%** |
| Kind accuracy | 100% | **100%** (action 51/51 · answer 23/23 · clarify 5/5 · abstain 1/1) |
| Actions emitting tool calls | 100% | **100%** |
| Actions building a Pipeline | 98.1% | **98.0%** |
| Emitted a node id | 0 | **0** |
| **Grader pass** | 97.5% | **98.75%** |

One failure in eighty, on a missing `run_status`. On held-out cases *with their retrieved context*,
this checkpoint is essentially correct.

## Demo smoke test — passed, and that is the problem

`scripts/demo-smoke.sh` reported **all beats passed**. It should not have, and the gap is in the
test rather than in the reading of it.

**Action turns are genuinely good.** The pipelines are use-case-appropriate and the descriptions
match what gets built:

- machine failure → `connect/clean/model/serve/act/serve`, "fit a time-to-event model … gated so
  nothing acts without your approval"
- document Q&A → `connect/index/serve/serve`, "build a semantic index … grounded in your own records"
- the n8n migration turn → credits what they built, then names what it cannot do

**Answer turns are still wrong, and the test cannot see it.** Verbatim:

> **"Why not just use an LLM for this?"** → *"For a scoring question, Studio will answer over the
> REST surface, so an assistant reads it from the internet and the rest of the Pipeline is the same
> answer no matter which source the data came from."*

> **"How do I know the score is right?"** → *"Studio will answer over the REST surface, so anything
> that can make an HTTP request can drive it."*

> **"Write me a poem about the ocean."** → *"That is the most common way a model looks brilliant for
> the wrong reason."* (correctly abstains; the sentence is a fragment of a leakage answer)

The smoke test checks **mode, Pipeline shape, unique node ids, no raw JSON, and no fabricated
entity**. Every one of those passed. It checks nothing about whether an answer is *true*, so a
fluent non-sequitur in the right mode scores as success.

**The teacher-authored alternates did not fix the blending.** They are ~150 ungrounded product
cases against 15,000; the answer families remain overwhelmingly trained against a capability
listing that serving does not supply ([#939](https://github.com/oramasearch/zetta/issues/939)).

One more, worth recording because a buyer would catch it: asked for **churn** with no Project, v10
answered *"fit a regression model"*. Churn is classification. Ungrounded, it has no Project to tell
it which use case it is in, so it picks one.

## What this run establishes

- **Structure is solved.** Parse, mode, tool calls, Pipeline construction, node ids — 100% or near
  it, three checkpoints running.
- **Grounded answering is solved.** 98.75% on the canary.
- **Ungrounded answering is not**, and three corpus attempts have not moved it. The cause is
  architectural rather than a data shortfall: an answer's content lives in the weights, and a 0.6B
  interpolates between memorized answers when served off-distribution. The Workspace Profile
  proposal — always ground, never serve an ungrounded turn — remains the honest fix.
- **Multi-turn is untested.** The corpus teaches it and serving carries it, but neither the canary
  nor the smoke test exercises a follow-up. Nothing here says whether it works.

## Actions

1. **The smoke test must check answer content**, not only shape. A fluent non-sequitur in the right
   mode currently passes, which is how this run nearly got reported as clean.
2. **Add multi-turn beats** to the smoke test — the feature shipped this run and is unmeasured.
3. **Decide the Workspace Profile** ([#939](https://github.com/oramasearch/zetta/issues/939)).
   Three corpus attempts is enough evidence that more ungrounded cases is not the lever.
4. Ablate the three changes if the attribution matters later.

## Verdict

**Ship v10 over v9** — strictly better on the canary, materially better on action turns, no
regression observed. **Do not demo the objection questions**: "why not just use an LLM" is the first
thing a technical buyer asks and the answer is currently incoherent.
