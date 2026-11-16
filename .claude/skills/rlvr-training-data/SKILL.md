---
name: rlvr-training-data
description: Generate verifiable-reward (RLVR) training data — the task/game corpora that train zetta's native agents — following blender-rl's "grow by generation, gate by verification" method. Use when creating or growing a training corpus for the base agent or a fine-tuned fork, synthesizing tasks from the solutions catalog or a deployment's own data, authoring reference solutions + negative controls, running the admission gate, tagging difficulty, holding out a generalization slice, pushing to a HuggingFace dataset, or generating synthetic data (e.g. into Postgres) in a target schema. Grounds every corpus in the methodology portfolio (methodologies, solution modules, the agent's five verbs and four autonomy classes) so it trains the agent to COMPOSE solutions, not answer questions about one. Triggers — "training data", "generate a corpus", "RLVR games", "reference solution", "admission gate", "synthetic data", "curriculum", "push to HF dataset", "train a future model", "multi-turn corpus", "corpus coverage".
---

# rlvr-training-data — build verifiable-reward training corpora

zetta trains its own agents with **RLVR** (reinforcement learning with verifiable
rewards). The agent is only as good as the **games** it practises on: a game is
*trainable* only if a deterministic grader can score it, and a corpus is *trustworthy*
only if it **discriminates** — reference solutions pass, plausible-wrong attempts fail.
This skill is how you produce that corpus, for the shipped base agent or a
fine-tuned fork trained on one deployment's data.

**Reference to reproduce, not embed:** blender-rl, a validated RLVR loop whose dataset
strategy (task generators, an admission gate, a dataset exporter) is the method this skill
distills. Also load **native-ai-dev** (building the model + RLVR
loop the corpus trains) and the module dev conventions (**zetta-dev**).

## The method — grow by generation, gate by verification (five principles)

Non-negotiable, straight from blender-rl's dataset strategy:

1. **Every task ships a validated reference solution.** A task isn't "done" until a
   reference scores high through the **live** grader *and* plausible-wrong attempts score
   materially lower. **Discrimination is the product, not the pass.**
2. **Difficulty is a data field, not a folder.** Each task carries `difficulty` / tier /
   skill-tags; the curriculum is a *query* over that metadata, re-sliceable without
   re-authoring.
3. **Rewards co-evolve with difficulty.** Harder tasks add harder *checks*, not just a
   longer prompt. A tier that reuses the old checks isn't harder.
4. **Grow by generation, gate by verification.** Author cheaply (templates, frontier-model
   assistance) but admit only what passes the reference + discrimination gate. **Volume
   without gating poisons the reward signal.**
5. **Hold out to measure generalization.** Some difficulty must never appear in training,
   or you can't tell learning from memorization.
6. **A turn is part of a thread, not a standalone row.** The second thing a person types
   is usually meaningless on its own — `"yes"`, `"what?"`, `"go ahead"`, `"the second one"`.
   A corpus of self-contained (ask, reply) pairs teaches the model to classify by surface
   form, and it then answers two different asks with a byte-identical sentence. Every case
   carries the turns it depends on (`AdmissionCase::history`), and the SAME word must train
   toward DIFFERENT behavior depending on what precedes it — `"yes"` after a concrete
   proposal builds; `"yes"` after an either/or question must still ask which one. Without
   that pair the model passes by memorizing the word, which is wrong exactly when it matters.
7. **Coverage is enumerated from the product, not from imagination.** Every ask form the
   corpus omits is claimed by the nearest family, and the model then behaves *correctly for
   the wrong family* — fluent, grounded, and answering a different question, so every
   aggregate metric stays green. This has shipped more than once (tool use, greetings,
   bare turns, ungrounded blending). Enumerate from the surfaces below rather than
   from what occurs to you while writing families.

## The zetta mapping (the native contract)

| blender-rl | zetta (`zetta::agent`, `zetta::ir`) |
|---|---|
| Task = prompt + grader contract in `info` | `Task` — description + `initial_state` + `tool_names` + `required` `EndState` + `grader()` (`EndStateGrader`) |
| Reference solution scores ≥ 0.95 | reference `EndState` **passes** the task's grader |
| Plausible-wrong attempts score low | `negative_controls` — each **fails** the grader |
| The admission gate | `admit_corpus(vector<AdmissionCase>)` — rejects untrainable tasks, rejected references, accepted negatives, or a corpus with **no** negatives |
| The reference *script* that builds the artifact | the reference **execution graph** (`ir::ExecutionGraph`) + the `Connect→Clean→Model→Index→Serve→Act` mutation **trajectory** that builds it |
| JSONL rows, grader contract in `info` | `Task`/`AdmissionCase` JSONL; the corpus loads as a HF dataset |

A task with **no required facts is untrainable** (nothing to verify) — the constraint
Auto-Suggest enforces before a task may enter the curriculum.

## What the corpus must cover — enumerate from the portfolio

**We are building an AI agent that builds AI systems.** That is the product thesis, and it
is what the corpus has to teach: **methodologies are the agent's tools, and the use-case
catalog is its action space.** The coverage checklist is
[`docs/agent/training-coverage-checklist.md`](../../../docs/agent/training-coverage-checklist.md).

A corpus that teaches "answer questions about a churn model" is not a thin version of that
— it is a different product. Enumerate coverage across four axes, none of which the engine's own `plan_templates.cpp`
(33 ids) captures on its own:

**1. The agent's five verbs** (the agent operating definition). Every module is agent-operated across: **Build** (compose the graph, discover
features inside the leakage blocklist, propose label/window definitions, configure
connectors) · **Run** (scoring cadences, push into systems of action, execute policy
decisions inside caps, manage holdout randomization) · **Monitor** (drift/PSI, calibration,
per-segment realized performance, holdout integrity, connector and schema health) ·
**Explain** (reason codes per score, driver reports, "what changed since last cycle", the
assembled outcome readout) · **Improve** (retrain on schedule and trigger, propose
threshold/policy changes *with their predicted effect*, run designed experiments).

Today's corpus is almost entirely **Build** and **Explain**. Monitor and Improve are absent,
and they are where a deployed agent spends most of its turns.

**2. The four autonomy classes** — **A0 draft** (recommend only, a human
executes) · **A1 approval-gated** (execute after a named human approves *this instance*) ·
**A2 autonomous-under-audit** (execute inside the policy envelope) · **A3 self-tuning**
(adjust thresholds inside pre-approved bounds, logging predicted and realized effect).
The class changes the correct reply for an identical ask: under A1, "rerun it with the new
threshold" must produce an approval request, not a run. A corpus blind to autonomy trains an
agent that acts when it should ask — the failure that makes governed self-serve unsafe.

**3. The 21 methodologies and the 11 components.**
M-1…M-21 across six families (tabular decision core · identity & structure · time &
vigilance · action & decision · language/documents/perception · the data supply chain), and
C-1…C-11 as the shared components the agent composes (connectors · entity resolution ·
point-in-time feature layer · labeling/outcome capture · training · tuning · explanation ·
evaluation harness · serving · monitoring/retraining · index/retrieval).

**Every generated Project must BE a real module with its real pipeline shape.** This already
fixed a serious bug once: without it, 92% of action cases taught two pipeline shapes and the
trained agent emitted the same pipeline for churn, lead scoring and collections alike. A
forecasting Project produces the **forecasting** pipeline — backtested, no calibration — not
the churn skeleton with the nouns swapped.

**4. The solution anatomy** (twelve fields). A module is not
a model: it is methodology composition + correctness adaptations + business wiring (actions
and decision policy, systems of action, people and cadence) + outcome contract + the agent's
operating definition + data plan + capability levels + deployment shapes + governance and
failure modes. Asks land on **all** of it. "Who acts on this score, and in which system?" is
a first-class ask, not an aside.

### The reward is the measured outcome, and the gate does not relax for the agent

Two constraints bind corpus authoring directly:

- **The metric the solution is measured on is the metric the agent learns on.** Where a
  case can express the solution's measured outcome signal, that is the reward, not a proxy
  for it. An `Improve` case that proposes a threshold change must carry its **predicted
  effect** as a checkable fact, because that is what the agent is actually optimizing.
- **"One gate for three authors"** — human-authored,
  compiled-from-intent, and agent-initiated graphs clear the *same* correctness gate;
  `author_kind` is recorded and never consulted by any rule. This is why agent autonomy is
  governable at all, and it means a corpus case must never train the agent to expect
  leniency, shortcut a check, or treat its own proposal as pre-verified.

### The vocabulary is load-bearing, not stylistic

**use case** = the problem · **methodology**
= the arranged AI methods · **solution** = the methodology applied correctly *and wired to
the business action*. Cardinality: many use cases ≈ many solutions ≫ few methodologies. A
reply that calls a methodology a solution has told the user the work is done when the
business wiring — the part that realizes the value — has not been started. Combine with the
product's user-facing contract (**Pipeline**, **Verification**, **Workspace Profile**,
**Saved Version**, **Proposed Solution**, **Ask Orama**) from the root `CLAUDE.md`.

## Workflow

### Author one task (the atom)
1. **State the required end-state first** — the *facts* a correct solution must produce
   (a churn pipeline: connect · leakage-free clean · **gbdt + SHAP** model · calibrated
   serve/act). If you can't name the checkable facts, the task isn't trainable yet.
2. **Reference solution** — build the artifact (the execution graph) so its facts pass the
   grader. Ground the facts in a **real** `ir::ExecutionGraph`, not hand-written strings,
   wherever you can.
3. **Negative controls** — plausible-wrong solutions that each fail ≥ 1 required fact (an
   LLM-only pipeline, a leaky one, a no-calibration one). Discrimination lives here.
4. **Gate it** — `admit_corpus` must admit the case; assert each negative control
   *individually* fails the grader. Red-then-green, like any nibble.
5. **Tag difficulty** + decide whether it's held out.

### Grow the corpus
- **From a solutions catalog** (the base agent): per use case, ground the real-world data
  shape (**Perplexity**) → generate N tasks + reference trajectories (a frontier model via
  **OpenRouter**) → **run every one through the admission gate before it enters the
  corpus** → push to a **private HF dataset**. All creds/config via **env vars**
  (`HF_TOKEN`, `HF_DATASET_REPO`, `OPENROUTER_API_KEY`, `PERPLEXITY_API_KEY`); nothing is
  contacted except when a script is run.
- **From a deployment's data** (a fork): synthesize data in that deployment's **exact schema**
  (a frontier model → local Postgres) with realistic signal, so the fork trains near
  real-shaped data with **no live-source dependency**.

### Build the corpus machinery with nibble-dev
Bottom-up, most-concrete first: the discriminating task → facts from a real graph → the
reference trajectory → JSONL (de)serialization → difficulty / held-out → the generators.
Each a red → green → build → atomic-commit bite. See **nibble-dev**.

## Never
- ❌ A task with no verifiable required facts (untrainable).
- ❌ A corpus with no negative controls (the gate catches nothing).
- ❌ Volume without gating (poisons the reward signal).
- ❌ A generator that pushes to HF **before** the admission gate.
- ❌ A corpus of standalone (ask, reply) pairs for a conversational surface — a follow-up is
  the second thing anyone types, and single-turn data cannot teach it.
- ❌ A bare turn with no `history`. Worse than omitting it: the case's correct answer is not
  derivable from its own input, so it teaches the model to answer the word.
- ❌ Declaring facts the grader does not compute. `response_solution_facts` is the list; an
  invented key (`tool_called`, `abandoned_prior_proposal`) makes the reference unsatisfiable
  and the case silently untrainable. `EveryReferencePassesItsOwnGrader` catches it — run it.
- ❌ Generating the graders. A model proposing its own success criterion is how a corpus
  quietly stops measuring anything. Criteria stay in C++.
- ❌ A Project that is not a real module from the catalog, or a pipeline shape borrowed from
  a different methodology.
- ❌ Hardcoded secrets — creds are env vars. Foreign models/services are **authoring aids
  only**, never in the train/infer path (the model is the product).

## Related skills
- **native-ai-dev** — build the model / RLVR loop the corpus trains (load with this).
- **nibble-dev** — the atomic-commit discipline for the corpus machinery.
- **new-connector** / **new-input** — real data sources / input formats a task may consume.
- **zetta-walkthrough** — walks the trained agent's surfaces; its follow-up row (three turns,
  no two consecutive replies byte-identical) is the acceptance this corpus is authored against.

## Version History
- v2.0.0 (2026-08-06): Grounded the corpus in the methodology portfolio — the agent's
  five verbs and four autonomy classes, M-1…M-21 + C-1…C-11, the twelve-field solution
  anatomy, outcome-as-reward, and "one gate for three authors". Added principles 6
  (a turn is part of a thread) and 7 (enumerate coverage from the product), after a
  bare-turn failure shipped a model that answered `"yes"` and `"what?"` identically.
- v1.0.0 (2026-07-21): Initial — distilled from blender-rl's dataset strategy + the
  base-agent training-corpus build (a churn use case as the first sample).
