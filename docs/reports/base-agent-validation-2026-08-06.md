# Base agent v14 — validation report

**Checkpoint:** `base-agent-0.6b-interaction-v14` · **Corpus:** `interaction-corpus-v14`
(16,239 train / 1,739 canary) · **Date:** 2026-08-06 · **Tracking:**
[#983](https://github.com/oramasearch/zetta/issues/983),
[#942](https://github.com/oramasearch/zetta/issues/942)

The report is the deliverable; the checkpoint is not.

## What changed from v13

The corpus gained a **`bare_turn` family** (180 cases) and `repeated_prior_turn` /
`restated_prior_turn` became gradeable facts, so a turn that repeats the previous reply can be
penalized during training rather than discovered at serving. This targets the failure in #942: the
served agent answered `"yes"` and `"what?"` with a byte-identical sentence.

Corpus composition: **360 of 16,239 cases carry `history`** (2.2%).

## Training

| | |
|---|---|
| Hardware | NVIDIA A10, CUDA |
| Steps | 3000 (`SAVE_EVERY=250`) |
| Wall clock | 2269 s (37.8 min), 0.757 s/step |
| Loss | 4.228 → **0.115** (minimum **1.43e-05**) |

## Canary results — 120 held-out cases

| Measure | v14 |
|---|---|
| Parse rate | **98.3%** (118/120) |
| Grader pass rate | **85%** (102/120) |
| Action cases parsed | 69 |

**Required facts most often wrong:** `asked_one_question` (3) · `built_nothing` (2) · `kind` (1).

Every one of these is **kind confusion** — the model choosing Action where Clarify was correct, or
answering where it should have asked. That is the failure class the corpus work targets, and it is
the right thing to still be failing on: it means the remaining errors are about *when to act*
rather than about producing malformed output.

## The acceptance test FAILS — v14 does not fix what it was trained to fix

Run live over `/assist` against v14 on a local node:

```
USER  create me a churn project
v14   Dropped that one. What should the new Project predict, and over which table?
      Right now churn-uc1 is what's loaded, on `customers`.

USER  yes
v14   Dropped that one. What should the new Project predict, and over which table?
      Right now churn-uc1 is what's loaded, on `customers`.      <- IDENTICAL

USER  what?
v14   Hi. churn-uc1 is what's loaded, on `customers`.
```

**Turns 1 and 2 are byte-identical.** #942's acceptance is not met, and the 85% canary pass rate
did not predict it — single-case grading cannot see a repeat, which is the entire reason bite A3
exists separately from A2.

**Worse: the `bare_turn` family appears to have caused a new failure.** *"Dropped that one"* is the
reply the `bare/redirect` cases teach for `"no"` / `"new one"` / `"not that"`. v14 emits it for
`"create me a churn project"` — a substantive request that is not a redirect at all. And `"what?"`
returns `"Hi."`, the greeting family. The model is selecting bare-turn families for asks that are
not bare turns.

### The v13 control run distinguishes the causes

Same three turns, same node, same history, against **v13** — which was trained on the corpus
*without* the `bare_turn` family:

```
USER  create me a churn project
v13   Which pipeline would you like me to base this on? I'd apply it to churn-uc1.

USER  yes
v13   Yes. Studio will list the trained models and their measures on `list_models`.

USER  what?
v13   Yes. Studio will list the trained models and their measures on `list_models`.   <- IDENTICAL
```

Three conclusions, and they separate cleanly:

1. **The repeat is not caused by `bare_turn`, and was not fixed by it.** Both checkpoints repeat.
   v13 repeats at turns 2–3; v14 repeats at turns 1–2. **The bug moved rather than went away.**
   It is the underlying multi-turn weakness — 2.2% of the corpus carries history — not anything
   the new family did or failed to do.
2. **"Dropped that one" IS caused by `bare_turn`.** v13 never produces it; v14 produces it for a
   substantive request. This rules out overfitting as the explanation: v13 was trained the same
   way, to a similar loss, and does not reach for that phrase. **It is corpus-caused, and it is
   my change.**
3. **Turn 1 got worse.** v13 answered a project request with a sensible clarifying question. v14
   answers it with a redirect reply meant for `"no"` / `"not that"`. The family did not just fail
   to help; on the first turn it actively regressed behaviour that was previously correct.

So the diagnosis is not "overfitting" and not "needs more multi-turn cases". It is that **180
cases carrying a phrase that appears nowhere else in 16,239 were enough to capture an ask form
they were never meant for** — high-signal replies with nothing competing for the same surface.
The family needs replies phrased no more distinctively than their neighbours, and negative
controls asserting the redirect reply is WRONG for a substantive ask.

Two candidate causes, now resolved by the control above:

1. **Over-application of a distinctive phrasing.** 180 `bare/*` cases out of 16,239 is 1.1%, but
   *"Dropped that one"* appears nowhere else in the corpus, so it is a high-signal token sequence
   with little to compete with it. The family may need its replies phrased less distinctively, or
   need paired negative controls showing the redirect reply is WRONG for a substantive ask.
2. ~~**Overfitting.**~~ Ruled out as the explanation for the phrasing: v13 trained to a similar
   loss and never reaches for it. Overfitting may still be worth addressing on its own merits —
   the `SAVE_EVERY=250` checkpoints remain uncompared — but it is not what caused this.

**This is a negative result and it stands.** The corpus fix that looked right in the invariants,
admitted through the gate, and scored 85% on the canary does not produce the behaviour it was
written for. The next step is diagnostic — compare intermediate checkpoints and test the
over-application hypothesis — not another corpus family.

## Limitations — read these before quoting the numbers

- **The three-turn walk was run and FAILED** — see the section above. The canary's 85% did not
  predict it, because single-case grading cannot see a repeat.
- **Overfitting is likely.** Minimum loss 1.43e-05 against a final of 0.115 is the signature
  flagged on v13. `SAVE_EVERY=250` means intermediate checkpoints exist and **have not been
  compared**; the best checkpoint is frequently not the last one.
- **120 of 1,739 canary cases were evaluated** (`ZETTA_EVAL_CASES=120`), for time. The figures are
  a sample, not the full canary.
- **No v13 comparison run.** The v13 figures on hand were measured differently, so no delta is
  quoted rather than quoting a misleading one.
- **2.2% of the corpus is multi-turn.** Enough to teach the specific bare turns that failed;
  thin for teaching "attend to the thread" as a general behaviour. Expect v14 to handle `"yes"`
  and `"what?"` and to remain weak on follow-ups nobody enumerated — which is the anticipation
  ceiling that motivates RLVR ([#859](https://github.com/oramasearch/zetta/issues/859)).

## Not published

Corpus v14 has **not** been pushed to the Hub yet (ladder bite A4). A checkpoint may not name a
corpus that was never published — the corpus is the model's provenance.

## Reproduce

```sh
# train (CUDA box)
ZETTA_QWEN_DIR=$HOME/qwen3-0.6b ZETTA_CORPUS=data/corpora/interaction-corpus-v14.jsonl \
ZETTA_SFT_EXAMPLES=16239 ZETTA_SFT_STEPS=3000 ZETTA_SFT_SAVE_EVERY=250 \
ZETTA_CHECKPOINT_OUT=data/checkpoints/base-agent-0.6b-interaction-v14 \
  ./build/tests/zetta-tests --gtest_filter=SftCudaResident.TrainsTheInteractionCorpusOnTheGpu

# evaluate (Mac — the CUDA box cannot evaluate; see docs/agent/gpu-box-runbook.md)
ZETTA_QWEN_DIR=$(pwd)/data/checkpoints/base-agent-0.6b-interaction-v14 \
ZETTA_CANARY=$(pwd)/data/corpora/interaction-corpus-v14.jsonl.canary ZETTA_EVAL_CASES=120 \
  ./build/tests/zetta-tests --gtest_filter=QwenRealCheckpoint.EvaluateBaseAgentOnCanary
```
