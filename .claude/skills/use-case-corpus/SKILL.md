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

## Step 3 — Author threads, not rows

Every case is part of a conversation (`AdmissionCase::history`). A follow-up is the second
thing anyone types, and single-turn data trains a model that answers two different asks with a
byte-identical sentence — which shipped
([#942](https://github.com/oramasearch/zetta/issues/942)).

Include the **bare turns** — `"yes"`, `"go ahead"`, `"what?"`, `"no, a new one"` — each paired
with a specific antecedent, and make the SAME word train toward DIFFERENT behaviour depending
on what precedes it.

### The negative-control rule — the hard part, and where quality is won

**The best negative control is a correct answer to a neighbouring question.** A straw man
("the model outputs gibberish") discriminates nothing and inflates the pass rate. Build
controls from the real confusions:

- the behaviour correct for a **different autonomy class** (runs when A1 required approval),
- the behaviour correct for a **different verb** (builds when asked to monitor),
- the pipeline correct for a **different methodology** (churn's calibrated shape on a
  forecasting ask),
- the reply correct for a **different antecedent** (the previous turn repeated verbatim).

Each of these is fluent, plausible, and wrong — which is exactly what the model will otherwise
produce.

### Facts must be ones the grader computes

`response_solution_facts` is the list. An invented key (`tool_called`,
`abandoned_prior_proposal`) makes the reference **unsatisfiable** and the case silently
untrainable. This has happened twice. `EveryReferencePassesItsOwnGrader` catches it — run it.

### No case may assert an unshipped capability

The corpus is a **claim surface**: `product_knowledge_family` states what the product can do,
in our voice, in the weights, to every customer, forever. Check every product assertion against
`docs/projects/motive-fde-engagement/product-claims.md` (orama-bridge). The corpus currently
says *"You can run Python in a Pipeline step"* while the engine has `PythonFrontend` —
**simulated** Python — and the customer meeting said "simulated Python". **The meeting was more
honest than the training data.** Do not add to that.

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
- the dataset card names its provenance and its gaps honestly.

## Never

- ❌ More than one use case per run. Breadth per run is how you get generic cases.
- ❌ Generating before the four research questions are answered in writing.
- ❌ Retyping the tool list, the interaction kinds, or the pipeline verbs.
- ❌ Straw-man negative controls.
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
- v1.0.0 (2026-08-06): Initial. Per-use-case research, the verb × autonomy coverage matrix, the
  neighbouring-answer rule for negative controls, the claim-surface check, and the
  admission-gate / correctness-gate distinction. Written for
  https://github.com/oramasearch/zetta/issues/983.
