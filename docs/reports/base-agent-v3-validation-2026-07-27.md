# Validation report — base agent v3 (corpus v3 + prompt masking)

**Date.** 2026-07-27 · **Checkpoint.** `data/checkpoints/base-agent-0.6b-interaction-v3`
**Base weights.** Qwen3-0.6B · **Corpus.** `interaction-corpus-v3.jsonl` (6,102 cases, 11 families)
**Canary.** `interaction-corpus-v3.jsonl.canary` (658 cases, verified disjoint from training)
**Hardware.** A100-SXM4-40GB · **Related.** [#933](https://github.com/oramasearch/zetta/issues/933) ·
[#932](https://github.com/oramasearch/zetta/issues/932) · [#936](https://github.com/oramasearch/zetta/issues/936) ·
[#937](https://github.com/oramasearch/zetta/issues/937)

## What was tested

The first checkpoint trained on a corpus where every `action` carries the tool calls that build its
Pipeline, and the first trained with the prompt masked out of the loss. The previous checkpoint
(`base-agent-0.6b-interaction-v1`) returned a fluent message with an empty `tool_calls` and built
nothing — it had never been shown a tool call, because 91% of the v2 corpus's action cases had none.

## Training run

| | |
|---|---|
| Steps | 6,000 (one epoch over 6,102 examples) |
| Wall clock | 55 min (~0.55 s/step) |
| Loss | **2.062 → 0.000111** (min 2.86e-06) |
| Prompt / completion | mean 593 / 83 tokens — completion is **12.3%** of the sequence |

The starting loss of 2.06 is the first honest number this project has produced. The previous run
reported 3.97 → 0.0127 over the whole sequence, of which ~87% was the prompt — the measured 12.3%
completion share here confirms that estimate almost exactly. **The two numbers are not comparable**,
and the earlier one should not be quoted.

## Held-out canary — 100 cases, strided across the corpus

| Metric | Result |
|---|---|
| Parse rate | **100%** |
| Kind accuracy | **100%** (action 53/53 · answer 40/40 · clarify 4/4 · abstain 3/3) |
| Action cases emitting tool calls | **100%** (53/53) |
| Action cases building a Pipeline | **100%** (53/53) |
| Emitted a node id | **0** |
| **Grader pass rate** | **47%** |

Structurally the checkpoint is sound: it always parses, always picks the right mode, always calls
tools when it should build, always produces a compiling Pipeline with unique node ids, and never
writes a node id itself. Against the previous checkpoint's 0% tool-call rate this is the difference
between an agent that builds nothing and one that builds.

**The 47% grader pass rate is the honest weak spot** and the number to improve. Mode and structure
are right; the case-specific facts often are not — the right output column, `leakage_free`,
`calibrated`, the named driver. The cause is visible directly: asked to build for churn, lead
scoring, and collections, the agent produced the **identical** five-step Pipeline and the identical
message each time. It has learned one pipeline shape, which passes any grader whose required facts
that shape happens to satisfy and fails the rest. This is [#937](https://github.com/oramasearch/zetta/issues/937)
— the corpus contains 354 distinct asks presented about 19 ways each — showing up in the metric.

## End-to-end through the serving harness

Installed in a running node and driven over `/v1/chat` (the same path the GUI and MCP use):

```
"Set up a churn prediction pipeline on our accounts data."
  kind: action · 5 tool calls, all ok · 5 nodes, 5 unique ids
  connect(source) -> clean -> model -> serve -> serve(solution)
  reply: "Here's the pipeline I'll build and run for your request."
```

Lead scoring and collections produce the same structure. The user-facing reply is prose; the raw
action envelope no longer leaks.

## Two defects found during evaluation, both fixed

The first eval run reported **0% parse rate**, which was wrong — the checkpoint was emitting correct
tool calls the whole time. Two independent bugs:

1. **The CUDA SFT driver never appended a stop token.** The MLX driver does, with a comment naming
   the exact consequence. Without one the model is never shown where an answer ends, so it wrote one
   action and then began another.
2. **`parse_studio_action` spanned the first `{` to the last `}`.** Against that runaway generation
   the span covers `{...}{...}` — not valid JSON — so a perfectly good first action was discarded.
   It now takes the first *balanced* object, string-aware, so trailing junk of any kind is harmless.

Fixing the parser alone took the same checkpoint from **0% to 100%** parse rate. The stop-token fix
is in the drivers and applies to the next run; this checkpoint still over-generates, and the harness
absorbs it.

**A third defect was in the eval itself.** It took the first N canary cases, and the corpus is
grouped by family, so an 80-case prefix drew 77 `answer` cases from a canary that is 53% `action` —
and reported a pipeline-building rate computed from three governance cases that legitimately build
nothing. It now strides. The 80-case prefix numbers in this session's history are not
representative; the strided 100-case run above is.

## A change that was tried and reverted

Grounding every ungrounded chat turn in the capability listing — to serve product-knowledge
questions on the distribution they were trained on. It fixed the language question but **regressed
churn from `action` to `clarify`**, because a capability listing makes a build request look like a
capability question. Pipeline building is the more important path, so it was reverted. Retrieval
here needs a routing decision, not an unconditional injection.

## Known limitations

- **One pipeline shape for every ask.** The most consequential limitation, and the cause of the 47%
  grader rate. [#937](https://github.com/oramasearch/zetta/issues/937).
- **Product knowledge needs its grounding.** Measured: 95.8% parse / 70.8% grader pass on product
  cases *with* the capability listing; without it the model blends memorized answers — asked which
  languages a step supports it answered about the connect step. Serving does not supply that listing
  and cannot naively, per the reverted change above.
- **The model over-generates.** No stop token in this run's training data. Fixed for the next run.
- **The checkpoint is not self-contained.** Saved before the fix that copies tokenizer files; those
  were copied by hand. The next run's checkpoint will carry them.

## What to do next

1. Retrain with the stop token, so the model ends its own turn rather than relying on the parser.
2. Address [#937](https://github.com/oramasearch/zetta/issues/937) — grader pass rate will not move
   much while every ask trains toward the same pipeline.
3. Decide how product-knowledge retrieval is routed, rather than grounding every turn.
