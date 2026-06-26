# Use cases — the Orama Zetta catalog + how to add one

This folder is the **use-case catalog** for Orama Zetta and the guide for adding to it. A **use case** is a *generic AI-project type* — any team instantiates it on **its own data** → an **AI project** (the central, user-managed item). **Churn ([`uc1.md`](uc1.md))** is the worked exemplar — the simplest version of the whole loop; **copy its structure** for a new use case.

> The process tooling lives in **`amaro-bridge`**: the [`add-use-case` skill](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/add-use-case/SKILL.md) (generates the Perplexity prompt), the [canonical runbook this mirrors](https://github.com/oramasearch/amaro-bridge/blob/main/docs/runbooks/adding-a-use-case.md), [`zetta-sync`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/zetta-sync/SKILL.md), [`prune-backlog`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/prune-backlog/SKILL.md), and the wtm worktree-plan tools. The blueprint (source of truth) is the wtm `ORAMA-ZETTA` project → [issues on `oramasearch/zetta`](https://github.com/oramasearch/zetta/issues).

## Catalog

| Use case | Doc | Status |
|---|---|---|
| **UC-1 — Churn intelligence** | [`uc1.md`](uc1.md) | exemplar; building — platform skeleton (plan #11) + native distributed-compute full pass (plan #14) |
| Fraud / anomaly · demand forecasting · lead / opportunity scoring · ticket triage + routing · collections / credit risk · predictive maintenance · next-best-action | _to research_ | candidates |

## What every use case is

The reusable shape, shared by all of them:

**featurize (point-in-time) → train (XGBoost) → explain (SHAP) → rank at-risk + per-entity drivers**

plus *optional, user-managed* components — a **recommended-action** layer (driver → action → owner) and a constrained **LLM playbook** (writes the narrative + picks the play, never invents numbers). The engine is **general AI** (gradient-boosted trees, RL, LLM — any model), not one method. Crucially, these methods are **re-implemented natively as distributed C++** over zetta's accelerator interface (CPU + GPU when available) and run **cluster-parallel** — zetta does **not** run Python / XGBoost or any foreign runtime in the database (see [ADR-17](https://github.com/oramasearch/zetta/issues/17) · the [engine docs](../execution.md)). Framing: [`../orama-zetta.md`](../orama-zetta.md) (decision 5).

## Principles (don't skip — these are why use cases ship vs. stall)

- **Generic-yet-intelligent.** Balance *real AI intelligence* (a model trained on the data) with being *generic enough to reuse* across teams.
- **Data-project management** (Andrew Ng — *most AI projects fail*). Start from the **problem**, get the **right data**, keep generation separate from execution, ship **correct-by-construction, governed, monitored**. A use case you can't state in one sentence has no ROI.
- **Keep it simple.** Even a basic model is useful. Ship the simplest correct version first; the optional components and fancier models come later.

## How to add a use case

Adding a use case is a **repeatable pipeline**, not a one-off creative act. It produces four artifacts that must stay in lockstep:

1. a **blueprint artifact** — `UC-N`, the *source of truth*, in the wtm `ORAMA-ZETTA` project;
2. a **solution doc** — `docs/use-cases/ucN.md`, mirroring [`uc1.md`](uc1.md) section-for-section;
3. a set of **GitHub issues** — the use case **plus its dependency closure**, on [`oramasearch/zetta`](https://github.com/oramasearch/zetta/issues);
4. a **worktree plan** — the dependency DAG that actually builds it.

Churn ([`uc1.md`](uc1.md)) is the worked exemplar; **read it end-to-end and copy its structure.** The steps below are deliberately verbose — do them in order, because each step's output feeds the next.

> **The principle that governs everything below:** a use case is a *generic AI-project type*, and **the engine is shared**. zetta's native, distributed, accelerator-backed compute engine — the [DDF](../execution.md), the `ComputeJob` coordinator + work-stealing scheduler, the native histogram-GBDT / TreeSHAP / `Model`, the [correctness gate](../gate.md), [deploy](../deployments.md), [run](../runs.md), and [governance](../access-control.md) — is **built once and reused by every use case.** A new use case contributes only its **skin**: the *entity*, the *label*, the *point-in-time features*, and the *driver → action → owner* map. It does **not** bring a new engine, and it does **not** introduce Python or any foreign runtime — every algorithm is re-implemented as **native distributed C++** over the accelerator interface (CPU + GPU when available; see [ADR-17](https://github.com/oramasearch/zetta/issues/17)). If you find yourself special-casing one customer's exact tables, stop: that's *bespoke creep*, the most common way a use case dies.

### Pre-flight — orient before you type

- Read [`../orama-zetta.md`](../orama-zetta.md) (product framing + the locked working decisions) and **[`uc1.md`](uc1.md)** (the fully-worked exemplar you will mirror).
- Skim the root [`CLAUDE.md`](../../CLAUDE.md) **Architecture** section so you know **what already exists to reuse** — the ExecutionGraph IR, the gate + its checks (schema/leakage/point-in-time, entity resolution), the DDF, the ComputeJob / compute engine, deployments, runs, principals/keys/audit. The honest expectation: **most of what a new use case needs is already built.** Your work is the skin plus the wiring, not a green-field system.
- Confirm the use case is *generic*: phrase it so **any** team could run it on **its own** data. If it only makes sense for one company, it isn't a use-case type yet.

### 1. Frame the problem in exactly one sentence

This is the gate that filters out use cases with no ROI. Write a single sentence that pins down four things — and if you can't, **stop here**: you don't have a use case yet, you have a wish.

- **Entity** — the unit you score (an *account*, a *shipment*, a *device*, a *ticket*). Everything downstream — features, predictions, actions — is *per entity*. Be exact about grain: "account" and "account-month" are different problems.
- **Label** — the *target* + the *horizon* + the *censoring rule*. "Churned" means nothing until you say *churned within 90 days of the as-of date, excluding accounts with < 90 days of history.* Write the exact, SQL-shaped definition.
- **Point-in-time discipline** — what is knowable **as of t0** (the as-of date), and the **leakage traps** specific to this domain. Every feature must be computable from data with `event_ts <= as_of`. List the seductive features that secretly encode the future (e.g. "number of cancellation tickets" when a cancellation *is* the label).
- **Decision** — the *action* the prediction drives and *how success is measured*. A prediction nobody acts on has no value. Name the action's owner and the metric (retained ARR, prevented fraud loss, deflected tickets).

Capture this sentence verbatim — it becomes the `UC-N` summary and the opening line of `ucN.md`.

### 2. Generate the deep-research prompt, then run it

You don't invent the feature set or the model from memory — you research how the problem is actually solved, then adapt.

- Run the [`add-use-case`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/add-use-case/SKILL.md) skill (in `amaro-bridge`). It takes your one-sentence frame and emits a **tailored Perplexity Deep Research prompt** — a long, structured query covering label definitions, canonical point-in-time features, model families, explanation methods, evaluation metrics, data sources, and known pitfalls for *this* domain.
- Run that prompt in **Perplexity Deep Research / Reasoning** mode (not a quick search — you want the multi-source dossier).
- Save the result to **`docs/use-cases/<slug>/research.md`** in this repo (e.g. `docs/use-cases/fraud/research.md`) with a provenance header (source + date + "research dossier"). This is the evidence base everything downstream cites.

### 3. Scope the AI-project shape — separate the engine from the skin

From the dossier, write down the concrete shape, and **tag every piece as reused engine vs. new skin**:

- **Label definition** *(skin)* — the exact target / horizon / censoring from step 1.
- **Point-in-time features** *(skin)* — the canonical feature list, each tagged with its source table + the as-of rule. These become a [DDF](../execution.md) featurization graph (join → groupby-aggregate → as-of window), built on the engine's existing DDF ops.
- **Model** *(engine)* — default to the native distributed **histogram-GBDT** already in the engine; note an alternative only if the dossier shows the default is wrong for this domain.
- **Explanation** *(engine)* — native **TreeSHAP** → per-entity drivers; reused as-is.
- **Driver → action → owner map** *(skin)* — the deterministic mapping from a dominant driver to a recommended action and its owner. This and the label are the *only* genuinely use-case-specific logic.
- **Evaluation bar** *(skin)* — the metric + threshold the model must clear to be worth shipping.
- **Data sources + pitfalls** *(skin)* — the systems the data comes from and the domain-specific leakage / identity traps (e.g. the FedEx-Freight **entity-resolution** trap in churn — does this domain have an analogue? If so, it's a make-or-break eval, like [TC-4](https://github.com/oramasearch/zetta/issues/251)).

Litmus test: if you're specifying more than the label + features + action-map, ask whether you're rebuilding something the engine already provides.

### 4. Land it in the blueprint (the source of truth)

The blueprint — not the issues, not the docs — is authoritative; everything else is generated from it.

- Add a `use_case` artifact **`UC-N`** in the wtm `ORAMA-ZETTA` project. Mirror UC-1's body: simple, generic, "any team on its own data," with the advanced/agentic parts explicitly called out as **deferred**.
- Wire its **dependency closure** with `depends_on` relations to the capabilities it needs — ingest (FEAT-1), the architect/compile path (FEAT-2), the **correctness gate** (FEAT-3 — the line between a real product and "n8n with an LLM bolted on"), the model engine (FEAT-6), deploy (FEAT-4), run + monitor (FEAT-5), and governed self-serve (FEAT-9) — plus any use-case-specific eval (mirror UC-1's link to TC-4).
- If the use case needs a capability that **doesn't exist yet**, add it as its **own** artifact (a `feature` / `arch_element`) and `depends_on` it — never bury a missing subsystem inside the use case.
- Keep it **simple and generic**. First instances are not bespoke builds; the engine is shared.

### 5. Write the solution doc + the durable issue context

- Produce **`docs/use-cases/ucN.md`**, mirroring [`uc1.md`](uc1.md) *section-for-section* (what it is · the pipeline · the skin · the data · the evaluation bar · what's deferred). Add a row to the **Catalog** table at the top of this README.
- Post a **consolidated context comment** on the `UC-N` GitHub issue. This matters: the issue *body* is **generated from the blueprint and is regenerated** on every sync, so any durable working context (business case, decisions, what's deferred, the build plan) must live in a **comment**, never in the body. UC-1's pinned comment is the model.

### 6. Sync the blueprint → docs + issues

Re-run [`zetta-sync`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/zetta-sync/SKILL.md). It projects the blueprint to the [zetta-docs](https://github.com/oramasearch/zetta-docs) site and to GitHub issue **titles and bodies**. After this, the three surfaces — blueprint, docs site, issues — must read the same. **Never hand-edit a generated issue body or doc page;** edit the artifact and re-sync.

### 7. Prune + prioritize the backlog

- Close any **non-artifact** tracking issues (kept + linked, never deleted) — a tracking issue that produces no software / doc / report is noise. See [`prune-backlog`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/prune-backlog/SKILL.md).
- Set `p1`–`p4` by the use case's **dependency order**, not abstract release order — priorities trace from `UC-N`'s closure (decision 3 in [`../orama-zetta.md`](../orama-zetta.md)).

### 8. Build it via a worktree plan — *you* build the graph, not the LLM

This is where the use case actually gets built: as an **execution graph over its issues**, launched into parallel worktrees. The single most important rule: **the dependency graph is your reasoning, written by hand — do not hand it to a context-blind LLM.**

1. **Create the plan.** `create_worktree_plan` over `UC-N` + its dependency closure. Be aware this **kicks off an automatic, context-blind LLM generation** (OpenRouter) that *guesses* the DAG from issue titles. Treat that output as a throwaway placeholder — you will overwrite it.
2. **Decompose to irreducibility.** Read the **code** (what *exists* vs. what's *missing*), each issue's **full** body + comment thread, and the blueprint. Break every epic down until each leaf is a **single, atomic, independently-verifiable change** — one type/schema, one op, one check, one panel. "Build a subsystem" is never a leaf; file its parts. Use `breakdown_node` / `create_issue` (with `plan_id`) to file the atomic issues.
3. **Build the graph by hand.** Decide what *actually* blocks what — a **hard** blocker only (B must be substantially done before A can start) — and write the whole edge set yourself with **`set_plan_dependencies`** (`from` *depends on* `to`). **Maximize parallelism:** leave independent work unlinked so it runs concurrently; avoid long chains. Do **not** use `generate_worktree_plan` / `expand_worktree_plan` / `analyze_worktree_plan` — those delegate the reasoning to the blind LLM and exist only for the GUI's no-agent button.
4. **Set dependencies LAST.** Adding a node (`create_issue --plan_id`, and likely `add_plan_nodes`) **re-triggers the LLM generation**, which clobbers your edges. So finish adding *every* node first, then make `set_plan_dependencies` the final write. If you must add a node afterwards, immediately re-assert the full edge set.
5. **Review the computed layers** with `get_worktree_plan`: layer 0 should be the foundational interfaces / entities (no deps), the use case's end-to-end test should sit near the last layer, and the critical path should be the genuine data dependency — nothing artificial.
6. **Run → review → resume.** Drive the layer-by-layer loop (`launch_plan_layer (layer 0)` → each task lands in its own worktree + opens a PR → the plan **pauses for PR review** → resume → next layer), per [`homer6/worktree-manager#10`](https://github.com/homer6/worktree-manager/issues/10). The PR-review gate is the deliberate stop/resume point. The churn plans are the reference: **#11** (the platform skeleton) and **#14** (the native distributed-compute full pass).

## Verify

- The use case reads in **one sentence**; its `UC-N` body is simple/generic (any team, its own data), and the **skin** (label + features + action map) is clearly separated from the **reused engine**.
- `docs/use-cases/<slug>/research.md` (the dossier) and `docs/use-cases/ucN.md` (mirrors [`uc1.md`](uc1.md)) both exist; `ucN.md` is in the Catalog table; and the `UC-N` issue has a **context comment** (not body).
- The blueprint is the source: after `zetta-sync`, the docs site + issue titles/bodies all match — **no hand-edited generated content**.
- A worktree plan exists with a **sane, hand-built DAG**: foundational interfaces in layer 0, independent work fanned out in parallel, the use case's end-to-end test in (near) the last layer, and **no LLM-guessed edges left in it**.
- **No Python / foreign runtime is introduced** — the use case is the *skin*; the heavy compute reuses the native distributed engine.

## Anti-patterns (the Andrew-Ng failure modes — don't ship these)

- **Backward start** — "we have data/AI, what can we do?" → no defined problem → no ROI.
- **Leakage** — features that peek past t0; the model looks great offline and fails in production.
- **Overpromising** — a use case that "automates everything" instead of a specific, measurable win.
- **Bespoke creep** — building it for one customer's exact tables instead of a generic type any team instantiates.
- **Model in the hot path** — a stochastic model wired into a critical workflow with no correctness gate.
