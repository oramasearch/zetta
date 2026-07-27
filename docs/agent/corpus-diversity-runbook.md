# Runbook — diagnosing and fixing corpus mode collapse

**When to use this.** Training loss goes to ~0 but held-out performance is poor, or the agent
gives the same answer to different questions. Both are symptoms of one cause: **the corpus does
not contain enough to learn.** This runbook is the procedure that found and fixed it once, written
so the next person does not have to rediscover it.

**The rule this exists to enforce: low loss is not evidence of learning.** A model that reaches
0.0001 on a corpus with 8.7 bits of target entropy has memorized ~430 outcomes. That is the
expected result, not a success. Before reading a loss number as progress, measure what there was
to learn.

Related: [`cards/datasets/base-agent.md`](cards/datasets/base-agent.md) ·
[`cards/models/base-agent.md`](cards/models/base-agent.md) ·
[`training-runbook.md`](training-runbook.md) · `tests/dataset/test_corpus_invariants.cpp`

---

## 0. Symptoms that point here

| Symptom | What it usually means |
|---|---|
| Training loss → ~0 within one epoch | The target set is small enough to memorize |
| Held-out grader far below training loss | Memorization, not generalization |
| Different asks produce identical output | One dominant target has swallowed the family |
| The agent answers a question it was not asked | It is reproducing a corpus row verbatim |

**Do not respond by training longer, adding epochs, or scaling the model.** All three make
memorization deeper. Confirm the corpus first — the measurements below take minutes.

---

## 1. Measure before changing anything

Every number here comes from the corpus alone; none needs a GPU.

```sh
cmake --build build --target zetta-tests
ZETTA_CORPUS_OUT=/tmp/corpus.jsonl ./build/tests/zetta-tests \
  --gtest_filter='InteractionContract.WriteCorpusJsonl'
./build/tests/zetta-tests --gtest_filter='CorpusInvariants.*'
```

The four that matter, in order of diagnostic power:

1. **Target entropy** — `-Σ p log₂ p` over rendered targets. This is the size of the problem the
   model is being asked to solve. 8.7 bits means ~430 effective outcomes.
2. **Dominant share** — what fraction of action cases share the single most common pipeline shape.
   Above ~45%, emitting that shape unconditionally is a winning strategy.
3. **Distinct targets / cases** — a crude proxy for the above two. Useful, but it *falls* when you
   add genuinely useful context variation, so never optimize it directly.
4. **Repeated asks against varying context** — if an ask repeats against identical grounding, that
   is duplication; against different grounding it is the anti-fabrication signal working.

Write the numbers down before you change anything. The fix is judged on the delta.

---

## 2. Find where the mass is

Mode collapse is always concentrated. Locate it before designing a fix.

```python
# pipeline shapes, per family
shapes = Counter(tuple(c['tool'] for c in a['tool_calls'])
                 for a in actions)
by_family[task_id.split('/')[0]][shape] += 1
```

A family with **one shape and thousands of cases** is the problem. In the case this runbook came
from, three families held 2,862 of 3,139 action cases at one shape each, while the 40 use-case
templates — which had genuinely distinct pipelines — had one case each.

Also list the most repeated *messages*. A constant string across a thousand cases is the cheapest
thing a model can learn, and it will learn it.

---

## 3. Fix in this order

Ordered by how much diversity they add per unit of work. Stop when the rules pass.

### 3a. Make the target depend on the input (highest value)

If a family's reply is a constant, the model learns the constant. Derive the message from what the
case actually does — the step that was added, the table it ran against, the capability that was
called. This also makes the product better: a confirmation that does not say what was done is not
one the user can check.

### 3b. Give every synthetic Project a real use case

The deepest fix. If contexts are interchangeable, every family answers from the same skeleton and
every pipeline looks alike. Carrying the solution template on the context means a change requested
on a forecasting Project produces the *forecasting* pipeline, and on a retrieval Project the
indexed one — from one change, across every family that touches a Project.

### 3c. Audit templates against their own descriptions

Read each template's description and ask whether its facts deliver what it promises. This found
real defects: a claims-triage solution that promised to "route the risky ones for review" had no
`act` step, and a fraud solution that promised to "flag for review" had none either. Fixing them
improved both correctness and shape diversity — the two are not in tension.

Where a solution acts on the world, gate it (`approval_gated`). A fraud model that auto-blocks
loses good customers.

### 3d. Raise the context count

Cheapest axis. Ensure there are at least as many contexts as solution templates, or part of the
catalogue never appears as a Project at all.

### 3e. Techniques from the literature not yet applied here

Reported in the tool-calling corpus literature (ToolBench, APIGen, ToolMind, xLAM, τ-bench) and
worth reaching for if the above is not enough:

- **Distractor tools / varying tool inventories.** Present tools that are *not* right for the ask,
  so the model must select rather than recall. Widely used; we do not do this yet.
- **Query-style axes** (APIGen): simple, multiple, parallel, parallel-multiple tool calls.
- **Multi-turn tool results**, so a later call depends on what an earlier one returned.
- **Rejection sampling / execution checking.** Our admission gate is already the semantic checker;
  executing the emitted code would add the execution checker.
- **DPO after SFT**, with mode-collapsed outputs as the rejected side. A refinement on top of a
  diverse corpus, not a substitute for one.

---

## 4. Re-measure, then retrain

Re-run `CorpusInvariants.*`. Record the delta:

```
distinct targets   715  -> 2111
target entropy    8.74  -> 10.65 bits
dominant shape   57.4%  -> 34.8%
```

Only then retrain, and evaluate on the canary with
`QwenRealCheckpoint.EvaluateBaseAgentOnCanary`. Write a dated report to
[`../reports/`](../reports/README.md) — required, and the loss number alone is not evidence.

---

## 5. Thresholds, and how much to trust them

`test_corpus_invariants.cpp` enforces entropy ≥ 9.5 bits and dominant shape ≤ 45%.

**These are locally calibrated, not standards.** The published work on tool-calling corpora argues
for diversity qualitatively and consistently, but reports no entropy figure and no distinct-output
ratio to compare against. Treat both as a ratchet: they encode "measurably better than the corpus
that produced a 47% grader rate," and they should be raised when the corpus improves, not tuned
down when a change makes them fail.

**If a rule fails, fix the corpus — do not move the threshold.** The one legitimate exception is a
rule that measures the wrong thing: the distinct-message *ratio* falls when you add genuinely
useful contexts, because the denominator grows. When a metric punishes a real improvement, replace
the metric with one that measures the risk directly (entropy and dominant share), and say why in
the commit.

---

## Version history

- **v1.0.0 (2026-07-27)** — Written from the base-agent v3 → v4 corpus work: loss 0.0001 with 47%
  held-out grader pass, traced to 8.74 bits of target entropy and 92% of action cases on two
  pipeline shapes.
