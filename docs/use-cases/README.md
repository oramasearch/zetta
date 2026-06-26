# Use cases — the Orama Zetta catalog + how to add one

This folder is the **use-case catalog** for Orama Zetta and the guide for adding to it. A **use case** is a *generic AI-project type* — any team instantiates it on **its own data** → an **AI project** (the central, user-managed item). **Churn ([`uc1.md`](uc1.md))** is the worked exemplar — the simplest version of the whole loop; **copy its structure** for a new use case.

> The process tooling lives in **`amaro-bridge`**: the [`add-use-case` skill](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/add-use-case/SKILL.md) (generates the Perplexity prompt), the [canonical runbook this mirrors](https://github.com/oramasearch/amaro-bridge/blob/main/docs/runbooks/adding-a-use-case.md), [`zetta-sync`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/zetta-sync/SKILL.md), [`prune-backlog`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/prune-backlog/SKILL.md), and the wtm worktree-plan tools. The blueprint (source of truth) is the wtm `ORAMA-ZETTA` project → [issues on `oramasearch/zetta`](https://github.com/oramasearch/zetta/issues).

## Catalog

| Use case | Doc | Status |
|---|---|---|
| **UC-1 — Churn intelligence** | [`uc1.md`](uc1.md) | exemplar; building (worktree plan #11) |
| Fraud / anomaly · demand forecasting · lead / opportunity scoring · ticket triage + routing · collections / credit risk · predictive maintenance · next-best-action | _to research_ | candidates |

## What every use case is

The reusable shape, shared by all of them:

**featurize (point-in-time) → train (XGBoost) → explain (SHAP) → rank at-risk + per-entity drivers**

plus *optional, user-managed* components — a **recommended-action** layer (driver → action → owner) and a constrained **LLM playbook** (writes the narrative + picks the play, never invents numbers). The engine is **general AI** (XGBoost, RL, LLM — any model), not one method. Framing: [`../orama-zetta.md`](../orama-zetta.md) (decision 5).

## Principles (don't skip — these are why use cases ship vs. stall)

- **Generic-yet-intelligent.** Balance *real AI intelligence* (a model trained on the data) with being *generic enough to reuse* across teams.
- **Data-project management** (Andrew Ng — *most AI projects fail*). Start from the **problem**, get the **right data**, keep generation separate from execution, ship **correct-by-construction, governed, monitored**. A use case you can't state in one sentence has no ROI.
- **Keep it simple.** Even a basic model is useful. Ship the simplest correct version first; the optional components and fancier models come later.

## How to add a use case

**Pre-flight:** read [`../orama-zetta.md`](../orama-zetta.md) and **[`uc1.md`](uc1.md) — the fully-worked exemplar; copy its structure.**

1. **Frame the problem in one sentence** — **entity** (the unit you score) · **label** (target + horizon + censoring) · **point-in-time discipline** (what's knowable as of t0; the leakage traps) · **decision** (the action it drives + how success is measured). *If you can't say it in a sentence, stop — no ROI yet.*
2. **Generate the deep-research prompt + run it** — run the [`add-use-case`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/add-use-case/SKILL.md) skill → it emits a tailored **Perplexity Deep Research** prompt → run it in Deep Research mode → save the dossier to `docs/use-cases/<slug>/research.md` (in this repo).
3. **Scope the AI-project shape** from the dossier: label definition · canonical point-in-time features · model (XGBoost + alts) · explanation (SHAP) · driver→action→owner map · evaluation bar · data sources · pitfalls. Mark the **generic engine** vs the **use-case skin** (the label + the action map are the skin).
4. **Land it in the blueprint** (wtm `ORAMA-ZETTA`, the source of truth) — add a `use_case` artifact `UC-N` (mirror UC-1's simple/generic body) and wire its **dependency closure** with `depends_on` to the features it needs (ingest, featurize/train, correctness gate, deploy, governed self-serve). Keep it **simple and generic** — first instances are not bespoke builds.
5. **Write the solution doc + the issue context** — produce **`docs/use-cases/ucN.md`** mirroring [`uc1.md`](uc1.md) section-for-section, add it to the **Catalog** table above, and post a **consolidated context comment** on the `UC-N` issue (the issue *body* is blueprint-generated and gets regenerated, so durable context lives in a **comment**).
6. **Sync** — re-run [`zetta-sync`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/zetta-sync/SKILL.md): blueprint → the [zetta-docs](https://github.com/oramasearch/zetta-docs) site + GitHub issues (titles **and** bodies).
7. **Prune + prioritize** — close non-artifact issues (kept + linked) and set `p1`–`p4` by the use case's dependency order. See [`prune-backlog`](https://github.com/oramasearch/amaro-bridge/blob/main/.claude/skills/prune-backlog/SKILL.md).
8. **Build it via a worktree plan** — `create_worktree_plan` over `UC-N` + its dependency closure → `generate_worktree_plan` → review the DAG (`get_worktree_plan`) → `launch_plan_layer (layer 0)`, then drive the run → PR-review → resume loop ([`homer6/worktree-manager#10`](https://github.com/homer6/worktree-manager/issues/10)). (Mirror plan #11 for churn.)

## Verify

- The use case reads in one sentence; its `UC-N` body is simple/generic (any team, its own data).
- `docs/use-cases/ucN.md` exists (mirrors [`uc1.md`](uc1.md)), it's in the Catalog table, and the `UC-N` issue has a context comment.
- The blueprint is the source: docs site + issue titles/bodies all match after the sync.
- A worktree plan exists with a sane DAG (independent work in layer 0; the use case itself in the last layer).

## Anti-patterns (the Andrew-Ng failure modes — don't ship these)

- **Backward start** — "we have data/AI, what can we do?" → no defined problem → no ROI.
- **Leakage** — features that peek past t0; the model looks great offline and fails in production.
- **Overpromising** — a use case that "automates everything" instead of a specific, measurable win.
- **Bespoke creep** — building it for one customer's exact tables instead of a generic type any team instantiates.
- **Model in the hot path** — a stochastic model wired into a critical workflow with no correctness gate.
