---
name: use-case-corpus
description: "Author the base agent's multi-turn, tool-calling training corpus across the WHOLE solutions catalogue (every S-n/UC-n), so each solution's pipeline shape is distinct and every case has 32 neighbours to be discriminated from. Carries the scenario-enumeration strategy (five verbs x four autonomy classes x five personas x seven conversation arcs), the trajectory anatomy that teaches building rather than lookup, the neighbouring-solution negative control, the fact vocabulary a reference may require, and the C++ authoring recipe. Use when growing or regenerating the corpus, when a use case answers in the wrong pipeline shape, when a verb like Monitor or Improve has no training data, or when the agent repeats itself across turns. Triggers - 'corpus for the catalogue', 'training data for churn/forecasting/collections', 'the agent gets this use case wrong', 'multi-turn samples', 'cover Monitor and Improve', 'grow the corpus', 'make the agent better'."
---

# use-case-corpus — one module, enumerated, authored as trajectories, admitted

**This file is the roadmap. Follow it in order.** Tables you must not paraphrase from memory —
the fact vocabulary, the tool surface, the C++ recipe, the commands — are in
[reference.md](reference.md). Load it before authoring.

Epic: [#983](https://github.com/oramasearch/zetta/issues/983) ·
generator: [#982](https://github.com/oramasearch/zetta/issues/982) ·
method: **rlvr-training-data** (load first).

---

## 0 · Two things to be honest about before starting

**This is the bootstrap.** *(Decision, Steve, 2026-08-06: bootstrap with SFT now, add RLVR
later.)* What you produce here is hand-anticipated supervised data. It is **not** the coverage
strategy — `run_rollout` + `Sandbox::apply` (`agent/rollout.cpp`) already implement the loop
that generates observation-conditioned trajectories at whatever variety a policy explores;
wiring the neural Qwen policy to it is [#859](https://github.com/oramasearch/zetta/issues/859).
So: **size this as a bootstrap.** Make the policy good enough to explore usefully. Do not try to
hand-cover the variety of ways humans talk — attempting that is how the corpus reached 14,889
cases while missing the word `"yes"`.

**Say which gate you mean.** Two unrelated mechanisms:

| Say | Mechanism | Judges |
|---|---|---|
| **admission gate** | `admit_corpus(vector<AdmissionCase>)` | a **training case** |
| **correctness gate** | `zetta::pipeline::CorrectnessGate` + `Check` | an **ExecutionGraph** before deploy |

This skill runs the **admission gate**, never the correctness gate.

---

## 1 · Scope: the WHOLE catalogue, swept

**Author across every solution in the catalogue — not one at a time.** `S-2`…`S-12`,
`M-10`…`M-21`, `UC-13`…`UC-24` (`plan_templates.cpp`), specified in
`modules/solutions-designs/docs/plan/04-solutions-catalog.md` (⚠️ internal-confidential).

**Breadth is not a cost here — it is where the training signal comes from.** The single
strongest negative control for any case is **the neighbouring solution's pipeline on this ask**:
well-formed, passes the correctness gate, and answers the wrong question. That is precisely what
a model produces when it emits from the use-case *name* rather than from the data — and it is
the defect that once made one pipeline shape serve churn, lead scoring and collections alike.

You cannot construct that control from one use case. With the full catalogue, **every case has
32 neighbours to be discriminated from**, and the corpus teaches the distinctions that matter:

| Solution | Its correctness profile | Confusable with |
|---|---|---|
| churn | point-in-time · leakage-free · **calibrated** | lead scoring (same shape, different label) |
| forecasting | **backtested**, temporal validation, **NOT calibrated** | churn (calibration is wrong here) |
| retrieval / search | indexed, **no supervised label at all** | anything supervised |
| anomaly detection | unsupervised, threshold-governed | classification |
| next-best-action | policy over outcomes, uplift | plain scoring |

**The catalogue is also the research.** Each entry already carries the twelve-field spec, so
per-solution grounding is *reading*, not investigating — which is what makes the sweep tractable.
Reserve deep external research (§2.4) for the handful of solutions you will demo.

**Sweep order.** Do not author 33 solutions to shallow depth, and do not perfect one. Go
breadth-first in three passes:

1. **Pass 1 — shape.** Every solution gets its **Build** trajectory with its *correct and
   distinct* pipeline shape, plus the neighbouring-solution negative control. This is the pass
   that fixes "one shape for every use case", and it must cover the catalogue completely.
2. **Pass 2 — verbs and autonomy.** Every solution gets **Monitor** and **Improve** (absent
   today), and every acting verb gets its **A1/A2 pair**.
3. **Pass 3 — arcs, personas, register.** Depth on the solutions that will be demoed or
   deployed — churn first (#969, Charles) — then outward.

Log what each pass skipped. A silent gap reads as coverage.

## 2 · Research before authoring — four questions, answered in writing

Answer these **per solution**, and record them in `docs/agent/cards/datasets/`. For most of the
catalogue this is reading the spec, not investigating — the catalogue did the work already.
**Research nobody recorded gets redone.**

1. **What is this module?** The twelve-field spec (`04-solutions-catalog.md` §2.6): methodology
   composition · correctness adaptations · business wiring (actions, systems of action, people
   and cadence) · ROI contract · agent operating definition · data plan · capability levels ·
   deployment shapes · governance and failure modes. **A module is not a model.** *"Who acts on
   this score, and in which system?"* is a first-class ask.
2. **What makes it CORRECT here?** (`03-methodology-portfolio.md`.) This is what separates
   use cases that look alike: churn is point-in-time, leakage-free, **calibrated**; forecasting
   is **backtested, not calibrated**; retrieval is indexed with no supervised label at all. Get
   this wrong and you train the agent to build the neighbouring solution, fluently.
3. **What is the engine's real surface?** Re-derive `kAgentTools`, the four `InteractionKind`
   contracts, and the pipeline verbs **from source** ([reference.md](reference.md) §2–3). Never
   retype a tool list — it is wrong the day someone adds a tool.
4. **What does this domain's data actually look like?** Real table and column names, real null
   rates, real leakage traps, real systems of action. Use the **perplexity** skill, or better,
   the customer's own docs — the Motive churn slice is grounded in real columns from their
   schema (`docs/projects/motive-fde-engagement/`), which is why its cases are accurate.

## 3 · Enumerate the scenario space — do not imagine it

**This is the section that answers "how do you anticipate the huge variety of ways people
interact?"** You do not brainstorm. You take the cross-product of six axes, each read off the
product, and you author the cells.

### Axis A — the five verbs (`04-solutions-catalog.md` §2.4)

**Build** · **Run** · **Monitor** · **Explain** · **Improve**.

Today's corpus is almost entirely **Build** and **Explain**. **Monitor** (drift/PSI,
calibration over time, per-segment realized performance, holdout integrity, connector and
schema health) and **Improve** (retrain on trigger, propose a threshold change *with its
predicted ROI effect*, run a designed experiment) are **absent** — and they are where a
deployed customer spends most of their turns. A slice that adds only more Build is not progress.

### Axis B — the four autonomy classes

**A0 draft** · **A1 approval-gated** · **A2 autonomous-under-audit** · **A3 self-tuning**.

**The class changes the correct reply to an identical ask.** Under A1, *"rerun it with the new
threshold"* must produce an approval request; under A2 it must run. Every acting verb needs at
least the A1/A2 pair, or the agent learns one behaviour and acts when it should ask — which
makes governed self-serve unsellable.

### Axis C — five personas, because the same question wants a different answer

| Persona | Asks like | Wants |
|---|---|---|
| **Analyst** | *"can you tell me who's likely to leave"* | the answer, not the method |
| **ML engineer** | *"what's the cutoff column and is this leakage-free?"* | the mechanism, and will check it |
| **Ops owner** | *"it flagged 400 accounts, who's supposed to call them?"* | the business wiring |
| **Executive** | *"is this actually working?"* | the ROI readout, one number, no jargon |
| **Skeptic** | *"how do I know that number isn't overfit?"* | the held-out figure and its caveat |

The skeptic is the most valuable and the least represented. An agent that answers *"how do I
know this isn't overfit"* by quoting the held-out figure **and naming the in-sample gap** is the
difference between a demo and a sale.

### Axis D — seven conversation arcs (the shape of a whole thread)

1. **Happy path** — ask → propose → approve → build → run → read the report.
2. **Blocked by data** — the ask is good, the data cannot support it. The agent must say so
   *before* building, naming which column is missing.
3. **Wrong thing asked** — the user asks for X; the data supports Y and Y is what they actually
   want. Propose Y, explain why, do not silently substitute.
4. **Mid-thread pivot** — three turns about churn, then *"actually, what's our disk usage?"*.
   Answer the new thing without dragging the old context in.
5. **Repair after failure** — a run failed; the follow-up is answerable only from the **tool
   outcome** in the history, not from the prose of the previous reply.
6. **Skeptical interrogation** — four turns of "prove it", each drilling into a number.
7. **Scope refusal** — the ask is out of scope, or in scope and unsupported. Decline, name why,
   offer the nearest thing that *is* supported.

### Axis E — register: how people actually type

Bare turns (`"yes"`, `"go ahead"`, `"what?"`, `"no, a new one"`, `"the second one"`) · lowercase
and typos (`"can u build me a chrun model"`) · frustration (`"that's not what I asked"`,
`"you already said that"`) · compound asks (`"build it and tell me who's worst"`) · thinking
aloud (`"hmm"`, `"ok so"`) · pasted artifacts (a SQL snippet, an n8n node, a stack trace, with
no sentence around it).

**Mine real register rather than inventing it** — `docs/meetings/*charles*` is verbatim from a
real technical buyer. **Known limit:** those are pre-sales and contain **no deployed-operator
register**, so Monitor and Improve register must be *designed* from the solution spec until
someone is deployed and generating it (then #943 captures it).

### Axis F — the failure library (this is where the negative controls come from)

For each cell, name what a plausible-but-wrong agent does. See §5.

### Working the matrix

Full enumeration is 5 × 4 × 5 × 7 = 700 cells before register. **Do not author all of them.**
Author in this priority order, and **log every cell you skip with the reason** — a silent gap
reads as coverage:

1. Every **verb** at least once (fixes the Monitor/Improve hole).
2. Every **A1/A2 pair** for each acting verb (fixes the autonomy hole).
3. Arcs 2, 5 and 7 — **blocked-by-data, repair-after-failure, scope refusal**. These are what
   make an agent trustworthy, and they are the arcs a happy-path corpus never contains.
4. The **skeptic** persona on the module's headline number.
5. Register variants over the cells you already authored — cheap, and it is where the bare-turn
   failure lived.

---

## 4 · Author TRAJECTORIES, not lookups

An AI builder does not answer questions. It **calls a tool, reads what came back, and decides
the next call from it.**

What the corpus teaches today (`tool_use_family`): `"list the models"` → call `list_models` →
report. One ask, one call, one report. That is a **lookup**. Nothing in it is conditioned on an
observation, so it cannot teach building.

### The shape to author

```
USER  "predict which accounts are likely to churn"

  → list_datasets                      # what is even here?
     ← motive.accounts (48 cols, 1.2M rows), motive.comm_events (31 cols, 84M rows)

  → get_dataset motive.accounts        # what does it support?
     ← sfdc_account_id (string, 11.4% null), churn_date (date, 8% non-null),
       last_churn_reason (string), mrr (float), event_updated_datetime_pt (timestamp)

AGENT "`churn_date` gives us a label and `event_updated_datetime_pt` gives us a cutoff, so
       this can be point-in-time. Two things first: `sfdc_account_id` is null on 11.4% of
       rows, and `event_updated_datetime_pt` is the pipeline load date rather than the
       source change date — joining on it would leak the future. Restrict to completed
       rows and drop the null-id rows, or impute them?"

USER  "drop them"

  → connect(motive.accounts)           # the pipeline is shaped by what was READ
  → clean(drop_null: sfdc_account_id, as_of: event_updated_datetime_pt, completed_only)
  → model(gbdt, label: churn_date IS NOT NULL, calibrate: true, holdout: 0.3)
  → index(shap)
  → serve

AGENT "Built. Point-in-time on `event_updated_datetime_pt`, calibrated, 30% held out so the
       quality figure is measured on accounts it never saw. Run it?"

USER  "yes"                            # a bare turn, resolvable only against the proposal
```

### The quality bar, as a test you can run

> **Could a model produce this pipeline without the observations?**

If yes, it is a lookup wearing a trajectory's clothes. **Perturb one fact in the grounding** —
remove `churn_date`, drop the null rate to 0.05%, remove the cutoff column — and the correct
trajectory **must change**. If it does not, the case teaches emission from the use-case *name*,
which is how one pipeline shape once served churn, lead scoring and collections alike.

This is the anti-fabrication signal applied to **building** — the same reason grounded-QA cases
hold the question fixed and vary the context.

### Threads

Every case carries `history` (`AdmissionCase::history`), including prior **tool outcomes**
(`PriorTurn::tools`) — so *"why did that fail?"* is answerable from the call that ran, not from
prose. Include bare turns against specific antecedents, and make the same word train toward
different behaviour depending on what precedes it
([#942](https://github.com/oramasearch/zetta/issues/942)).

---

## 5 · Negative controls — where quality is actually won

**The best negative control is a correct answer to a NEIGHBOURING question.** A straw man
("the model outputs gibberish") discriminates nothing and inflates the pass rate. Build controls
from the real confusions:

| Control | Why it is the right one |
|---|---|
| The **neighbouring solution's pipeline** on this ask | Well-formed, passes the correctness gate, answers the wrong question. Exactly what a model that emits from the use-case name produces. |
| The behaviour correct for a **different autonomy class** | Runs when A1 required approval. |
| The behaviour correct for a **different verb** | Builds when asked to monitor. |
| **The right pipeline reached without the observations** | Guessed correctly. Punishing this is what forces attention onto the schema. |
| The reply correct for a **different antecedent** | The previous turn repeated verbatim (`repeated_prior_turn`). |

## 6 · Hard constraints

- **Only require facts the grader computes.** [reference.md](reference.md) §1 is the list. An
  invented key makes the reference unsatisfiable and the case silently untrainable — twice
  already (`tool_called`, `abandoned_prior_proposal`).
- **Never assert an unshipped capability.** The corpus is a **claim surface**:
  `product_knowledge_family` states what the product can do, in our voice, in the weights, to
  every customer, forever. Check every assertion against
  `docs/projects/motive-fde-engagement/product-claims.md`. The corpus currently says *"You can
  run Python in a Pipeline step"* while the engine has `PythonFrontend` — **simulated** Python —
  and the customer meeting said "simulated Python". **The meeting was more honest than the
  training data.** Do not add to that.
- **Never generate the graders.** A model proposing its own success criterion is how a corpus
  quietly stops measuring anything. Criteria stay in C++.
- **Vocabulary contract** in every user-facing string: **Pipeline** (never "execution graph"),
  **Verification**, **Workspace Profile**, **Saved Version**, **Proposed Solution**,
  **Ask Orama**.

## 7 · Emit, admit, measure

The recipe and commands are [reference.md](reference.md) §4–5. In short: a family function in
`interaction_families.cpp`, registered in `interaction_families(...)`, then:

```sh
cmake --build build --target zetta-tests
./build/tests/zetta-tests --gtest_filter='CorpusInvariants.*:DatasetGenerator.*'
ZETTA_CORPUS_OUT=data/corpora/interaction-corpus-vN.jsonl \
  ./build/tests/zetta-tests --gtest_filter='InteractionContract.WriteCorpusJsonl'
```

Then report — **these numbers are the deliverable, not the case count**:

- **Admission-gate rejection rate.** It says how much verification is doing. **0% means the
  admission gate is not biting** and is a bug in the gate, not a win.
- **Coverage**: cells authored, cells skipped, each skip with its reason.
- **Perturbation result**: of N trajectories perturbed, how many changed. Any that did not are
  lookups and must be rewritten.
- **Diversity**: target entropy and dominant-shape share
  (`docs/agent/corpus-diversity-runbook.md`). A model-authored slice mode-collapses as easily as
  a hand-authored one, and more expensively.

## 8 · Definition of done

Not "cases were generated".

- [ ] **Pass 1 complete: every solution in the catalogue has a Build trajectory with its own
      correct pipeline shape**, and a neighbouring-solution negative control.
- [ ] Every verb covered; every acting verb has its A1/A2 pair.
- [ ] Arcs 2, 5, 7 (blocked-by-data, repair-after-failure, scope refusal) present.
- [ ] Every skipped cell logged **with its reason**.
- [ ] Every trajectory: each call after the first conditioned on an observation.
- [ ] Perturbation test passes on a sample.
- [ ] `admit_corpus` admits every case; each negative control fails **individually**.
- [ ] Every `CorpusInvariants.*` passes — this slice is additive and does not get to lower the
      bar that caught the last four regressions.
- [ ] A held-out probe for this module, written **without** reference to the authored cases,
      answers in the right **kind** and the right **pipeline shape**.
- [ ] The dataset card names its provenance and its gaps honestly.

## 9 · Never

- ❌ Authoring one solution to depth before every solution has its distinct pipeline shape (pass 1).
- ❌ Authoring before the four research questions are answered in writing.
- ❌ Retyping the tool list, the kinds, or the fact keys instead of reading them from source.
- ❌ Straw-man negative controls.
- ❌ A trajectory whose pipeline is derivable from the ask alone.
- ❌ A tool call not conditioned on what the previous call returned.
- ❌ A bare turn with no `history`.
- ❌ Asserting a capability the claims register does not carry as shipped.
- ❌ Bare "gate" — say **admission gate** or **correctness gate**.

## Related

- **rlvr-training-data** — the method (grow by generation, admit by verification). Load first.
- **perplexity** — real-world grounding for research question 4.
- **add-use-case** — building the `UC-n` in the engine; this skill trains the agent on it.
- **zetta-walkthrough** — the follow-up row is the acceptance this corpus is authored against.
- **nibble-dev** — one matrix cell per bite, admitted before the next.

## Version History
- **v2.1.0 (2026-08-06)** — Scope corrected to the **whole catalogue**. "One module per run" was
  wrong: the strongest negative control is the neighbouring solution's pipeline, which is
  unconstructible from a single use case. Breadth is where the discrimination signal comes from —
  every case gets 32 neighbours. Adds the three-pass sweep (shape → verbs/autonomy → depth) and
  the correctness-profile confusion table.
- **v2.0.0 (2026-08-06)** — Rewritten as the roadmap Claude Code follows, not a checklist that
  points elsewhere. Adds the six-axis enumeration strategy (verbs × autonomy × personas × arcs ×
  register × failures) as the systematic answer to "anticipate the variety", the priority order
  for working the matrix, the trajectory anatomy with the perturbation test as its quality bar,
  and the neighbouring-answer rule for negative controls. Tables moved to `reference.md`.
- v1.1.0 (2026-08-06) — Trajectories over lookups.
- v1.0.0 (2026-08-06) — Initial.
