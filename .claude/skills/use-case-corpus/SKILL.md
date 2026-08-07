---
name: use-case-corpus
description: "Research ONE use case (UC-n / S-n) to the depth where you can author accurate training cases for it, then generate a multi-turn corpus slice covering the agent's five verbs and four autonomy classes, and admit it through admit_corpus. Use when growing the base agent's corpus for a specific solution module, when a use case answers in the wrong pipeline shape, or when a verb (Monitor, Improve) has no training data. Triggers - 'corpus for UC-n', 'training data for churn/forecasting/collections', 'research a use case', 'the agent gets this use case wrong', 'cover Monitor and Improve', 'grow the corpus'."
---

# use-case-corpus — one use case, researched, then a corpus slice for it

Tracking: [zetta#983](https://github.com/oramasearch/zetta/issues/983) (the epic) ·
[zetta#982](https://github.com/oramasearch/zetta/issues/982) (the generator).
Load **rlvr-training-data** first — it owns the method; this skill owns the *per-use-case*
research and coverage discipline that makes the output accurate rather than merely fluent.

## Why one use case at a time

A prompt spanning all 33 solution templates produces generic cases — the same failure as the
hand-authored corpus, reached faster. **Depth per use case is the entire point.** A churn case
that names `sfdc_account_id` and `LAST_CHURN_REASON` is grounded in a real schema; one that
names `customers.churn_flag` is invented, and the model learns to invent.

One run of this skill = **one `UC-n`/`S-n`**, researched, covered, admitted, committed.

## Vocabulary — two different gates, never "the gate"

| Say | Mechanism | Judges |
|---|---|---|
| **admission gate** | `admit_corpus(vector<AdmissionCase>)` (`agent/admission_gate.cpp`) | a **training case** |
| **correctness gate** | `zetta::pipeline::CorrectnessGate` + `Check` (`pipeline/gate.hpp`) | an **ExecutionGraph** before deploy |

This skill runs the **admission gate**. It never runs the correctness gate. Conflating them
borrows a safety argument about graphs and applies it to training data, where it does not hold
([#983](https://github.com/oramasearch/zetta/issues/983)).

## Step 1 — Research, in this order

Do not write a single case until all four are answered in writing. Skipping straight to
generation is how the corpus got here.

**1. What this module IS** — `modules/solutions-designs/docs/plan/04-solutions-catalog.md`
(⚠️ internal-confidential), the twelve-field spec: methodology composition · correctness
adaptations · business wiring (actions and decision policy, systems of action, people and
cadence) · ROI contract · agent operating definition · data plan · capability levels ·
deployment shapes · governance and failure modes. **The module is not the model.** "Who acts
on this score, and in which system?" is a first-class ask.

**2. Its methodology and correctness profile** — `03-methodology-portfolio.md`, the `M-n`
composition and what must be true for it to be *correct* here. This is what separates
use cases that look alike: forecasting is **backtested, not calibrated**; churn is
**point-in-time, leakage-free, calibrated**. A case that gets this wrong teaches the agent to
build the neighbouring solution.

**3. The engine's real surface — rendered from source, never retyped.** The live tools
(`kAgentTools` in `capabilities.cpp`), the four `InteractionKind` contracts, and the pipeline
construction verbs (`connect` · `clean` · `model` · `index` · `serve` · `act`,
`solution_graph.hpp`). A retyped tool list is wrong the day someone adds a tool, and a case
calling a tool that does not exist is a case that trains a hallucination.

**4. The real world** — use the **perplexity** skill. What are the *actual* table and column
names in this domain, the actual data shapes, the actual leakage traps, the actual systems of
action? For a customer engagement, prefer their own documentation over research: the Motive
churn work is grounded in real column names from their schema
(`docs/projects/motive-fde-engagement/`), which is why its cases are accurate.

Write the answers into `docs/agent/cards/datasets/` alongside the corpus. **Research nobody
recorded is research that gets redone.**

## Step 2 — Build the coverage matrix before generating

Coverage is **enumerated, not imagined** — that is the whole correction this skill exists to
make. For the use case in hand, the matrix is mechanical:

**Five verbs** (`04-solutions-catalog.md` §2.4) — **Build** · **Run** · **Monitor** ·
**Explain** · **Improve**. Today's corpus is almost entirely Build and Explain; a slice that
adds only more Build is not progress.

**Four autonomy classes** — **A0 draft** · **A1 approval-gated** · **A2 autonomous-under-audit**
· **A3 self-tuning**. The class **changes the correct reply to an identical ask**: under A1
*"rerun it with the new threshold"* must produce an approval request; under A2 it must run.
Every verb that acts needs at least the A1/A2 pair, or the agent learns one behaviour and acts
when it should ask.

**Register** — how people actually type. Mine `docs/meetings/*charles*` for verbatim asks from
a real technical buyer rather than inventing them. **Known limit:** those transcripts are
pre-sales and contain no deployed-operator register, so **Monitor and Improve register must be
designed from the solution spec** until someone is deployed and generating it (then #943
captures it).

Any cell you deliberately skip gets **logged as skipped**. A silent gap reads as coverage.

## Step 3 — Author TRAJECTORIES. This is the whole skill.

An AI builder does not answer questions. It **calls a tool, reads what came back, and decides
the next call from it.** That is the behaviour we are training, and the corpus contains almost
none of it.

What `tool_use_family` teaches today:

```
ask: "list the models on this node"
  -> call list_models
  -> report the result
```

One ask, one call, one report. That is a **lookup**. It cannot teach building, because nothing
in it is *conditioned on an observation*.

### What an exceptional case looks like

A trajectory, where every call after the first is decided by what the previous one returned,
and the final pipeline is **derivable only from what the schema actually showed**:

```
USER  "predict which accounts are likely to churn"

  -> list_datasets                     # what is even here?
     <- motive.accounts (48 cols, 1.2M rows), motive.comm_events (31 cols, 84M rows)

  -> get_dataset motive.accounts       # what does it support?
     <- sfdc_account_id (string, 11.4% null), churn_date (date, 8% non-null),
        last_churn_reason (string), mrr (float), event_updated_datetime_pt (timestamp)

AGENT "`churn_date` gives us a label and `event_updated_datetime_pt` gives us a cutoff, so
       this can be point-in-time. Two things first: `sfdc_account_id` is null on 11.4% of
       rows, and `event_updated_datetime_pt` is the pipeline load date rather than the
       source change date - joining on it would leak the future. Restrict to completed
       rows and drop the null-id rows, or impute them?"

USER  "drop them"

  -> connect(motive.accounts)          # NOW the pipeline is shaped by what was read
  -> clean(drop_null: sfdc_account_id, as_of: event_updated_datetime_pt, completed_only)
  -> model(gbdt, label: churn_date IS NOT NULL, calibrate: true, holdout: 0.3)
  -> index(shap)
  -> serve

AGENT "Built. Point-in-time on `event_updated_datetime_pt`, calibrated, 30% held out so the
       quality figure is measured on accounts it never saw. Run it?"

USER  "yes"                            # a bare turn, resolvable only against the proposal
```

Four properties make that case worth training on, and all four are checkable:

1. **Every call after the first is conditioned on an observation.** `get_dataset` was called
   *because* `list_datasets` returned a candidate; `clean` carries `drop_null` *because*
   `get_dataset` reported 11.4% nulls.
2. **The pipeline is underivable from the ask alone.** "Predict churn" does not tell you to
   drop null ids or which column is the cutoff. **This is the anti-fabrication signal applied
   to building** — the same reason grounded-QA cases vary the context. A case whose pipeline
   could be emitted without reading the schema teaches the model to emit from the use-case
   *name*, which is how one pipeline shape ended up serving churn, lead scoring and
   collections alike.
3. **It surfaces the domain's real trap before building**, not after the run fails. The traps
   are in the research from step 1 and in the customer's own docs.
4. **It ends in a bare turn** that only resolves against the proposal.

### The quality bar, stated as a test

> **Could a model produce this pipeline without the observations?**
> If yes, the case is a lookup wearing a trajectory's clothes. Rewrite it so the schema
> decides something.

Concretely: change one fact in the grounding — make `churn_date` absent, make the null rate
0.05%, make the cutoff column missing — and the correct trajectory **must** change. If it does
not, the case is not teaching building.

### The catalogue is the action space

Each `S-n` has a **specific pipeline shape**, and the trajectory must arrive at *that* one:
churn is point-in-time, leakage-free, calibrated; forecasting is **backtested, not
calibrated**; retrieval is indexed with no supervised label at all. Getting this wrong is not
a near miss — it trains the agent to build the neighbouring solution fluently.

So the strongest negative control for any trajectory is **the neighbouring solution's pipeline
on this ask**. It is well-formed, it passes the correctness gate, and it answers the wrong
question — which is exactly what the model produces when it emits from the use-case name
rather than from the schema.

### Threads, and the bare turn

Every case is part of a conversation (`AdmissionCase::history`), carrying prior tool outcomes
(`PriorTurn::tools`) — so *"why did that fail?"* is answerable from the **call that ran**, not
from the prose. Include the bare turns (`"yes"`, `"go ahead"`, `"what?"`, `"no, a new one"`),
each against a specific antecedent, and make the same word train toward different behaviour
depending on what precedes it ([#942](https://github.com/oramasearch/zetta/issues/942)).

### The negative-control rule

**The best negative control is a correct answer to a neighbouring question.** A straw man
discriminates nothing and inflates the pass rate. Build them from the real confusions:

- the **neighbouring solution's pipeline** (calibration on a forecasting ask),
- the behaviour correct for a **different autonomy class** (runs when A1 required approval),
- the behaviour correct for a **different verb** (builds when asked to monitor),
- **the pipeline emitted without the observations** — the right shape reached by guessing,
- the reply correct for a **different antecedent** (the previous turn repeated verbatim).

### Facts must be ones the grader computes

`response_solution_facts` is the list. An invented key makes the reference unsatisfiable and
the case silently untrainable — twice already. `EveryReferencePassesItsOwnGrader` catches it.

### No case may assert an unshipped capability

The corpus is a **claim surface** — `product_knowledge_family` states what the product can do,
in our voice, in the weights, to every customer. Check every assertion against
`docs/projects/motive-fde-engagement/product-claims.md`. The corpus says *"You can run Python
in a Pipeline step"*; the engine has `PythonFrontend` — **simulated** Python — and the customer
meeting said "simulated Python". **The meeting was more honest than the training data.**

## Step 4 — Admit, measure, report

1. **Run the admission gate.** `admit_corpus` must admit every case; assert each negative
   control fails **individually**.
2. **Report the rejection rate as the headline.** It says how much verification is doing. A 0%
   rejection rate means the admission gate is not biting and is a bug in the gate, not a win.
3. **Run every corpus invariant** (`CorpusInvariants.*`). This slice is additive; it does not
   get to lower the bar that caught the last four regressions.
4. **Measure diversity** — target entropy and dominant-shape share, per
   `docs/agent/corpus-diversity-runbook.md`. A model-authored slice mode-collapses as easily as
   a hand-authored one, and more expensively.
5. **Update the dataset card** — what generated this slice (model id, prompt version, sampling
   parameters, accept/reject counts), and what it still does not cover.

## Definition of done

Not "cases were generated". The slice is done when:

- every matrix cell is covered **or logged as skipped, with the reason**;
- the admission gate admits every case and the rejection rate is reported;
- every existing invariant passes;
- a held-out probe for this use case — written **without** reference to the cases authored —
  answers in the right **kind** and the right **pipeline shape**;
- **the perturbation test passes**: for a sample of trajectories, change one fact in the
  grounding (remove the label column, drop the null rate to 0.05%, remove the cutoff) and the
  correct trajectory changes. If it does not, those cases teach emission from the use-case
  name, not from the data, and they are the reason one pipeline shape once served churn, lead
  scoring and collections alike;
- **every multi-call trajectory has each call after the first conditioned on an observation** —
  a trajectory whose calls are a fixed script is a lookup with extra steps;
- the dataset card names its provenance and its gaps honestly.

## Never

- ❌ More than one use case per run. Breadth per run is how you get generic cases.
- ❌ Generating before the four research questions are answered in writing.
- ❌ Retyping the tool list, the interaction kinds, or the pipeline verbs.
- ❌ Straw-man negative controls. The strongest control is the **neighbouring solution's
  pipeline on this ask** — well-formed, correctness-gate-passing, and answering the wrong
  question.
- ❌ A trajectory whose pipeline is derivable from the ask alone. That is a lookup wearing a
  trajectory's clothes, and it trains emission from the use-case name.
- ❌ A tool call that is not conditioned on what the previous call returned.
- ❌ A case asserting a capability the claims register does not carry as shipped.
- ❌ Bare "gate" — say **admission gate** or **correctness gate**.
- ❌ Generating the graders. A model proposing its own success criterion is how a corpus
  quietly stops measuring anything. Criteria stay in C++.

## Related

- **rlvr-training-data** — the method (grow by generation, admit by verification). Load first.
- **perplexity** — real-world grounding for step 1.4.
- **add-use-case** — building the `UC-n` in the engine; this skill trains the agent on it.
- **zetta-walkthrough** — the follow-up row is the acceptance this corpus is authored against.
- **nibble-dev** — one matrix row per bite, admitted before the next.

## Version History
- v1.1.0 (2026-08-06): Rewrote Step 3 around **trajectories**. The corpus's tool use was one
  ask → one call → one report, which is a lookup and cannot teach building. An exceptional case
  is a trajectory where every call after the first is conditioned on an observation and the
  pipeline is **underivable from the ask alone** — the anti-fabrication signal applied to
  building. Added the perturbation test as the quality bar, and the neighbouring solution's
  pipeline as the strongest negative control.
- v1.0.0 (2026-08-06): Initial. Per-use-case research, the verb × autonomy coverage matrix, the
  neighbouring-answer rule for negative controls, the claim-surface check, and the
  admission-gate / correctness-gate distinction. Written for
  https://github.com/oramasearch/zetta/issues/983.
