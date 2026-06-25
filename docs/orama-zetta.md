# Orama Zetta — product + working conventions

> **Read this first** for *what we're building and the rules that govern it.* The rest of `docs/` is the engineering reference for the `zetta` codebase.

## What Orama Zetta is

**Orama Zetta turns a question into running, deployed intelligence.** Describe a data or AI problem in plain language — or hand over an artifact you already have (an n8n workflow, a SQL script, a notebook) — and Orama Zetta architects a correct pipeline, deploys it onto infrastructure *you* control, runs it, and **learns from your data**. *The data engineer, the AI engineer, and the platform engineer — in a bottle.* **Intelligence Velocity for Big Thinkers.**

`zetta` (this repo + binary) is the **codename**; **Orama Zetta** is the **product**. Lead with what it does for the user — *not* "a database." Canonical framing: the [one-pager](https://github.com/oramasearch/amaro-bridge/blob/main/docs/projects/orama-platform-2/one-pager.md).

## Working decisions (mirror the blueprint ADRs)

These are the locked product/process decisions (recorded as `adr` artifacts in the wtm `ORAMA-ZETTA` blueprint; epic [`#279`](https://github.com/oramasearch/zetta/issues/279)):

1. **Naming** — the product is **Orama Zetta**; the older name **"ZettaDB" is retired**. This is a rename only — *no functional change*. "Orama Zetta" is the current **working** name (may change).
2. **Value-first messaging** — lead with the outcome, not "a database." *Kitchen/meals:* the engine is the kitchen; the **use cases are the consumable meals** — the emphasis is on the meals.
3. **Use-case-driven development** — each release is a **consumable product** (a working slice of a use case). Backlog **priorities trace from a use case's dependency closure**, not abstract release order. **Churn (UC-1, the Motive case) is the first use case** — bar: a customer gets a working production churn model within ~an hour.
4. **Backlog grooming** — a tracking issue is a **request to produce an artifact** (software / docs / a report). If it produces none, **close it (kept + linked, never deleted)**. Keep buildable work + risks + releases open.
5. **AI projects are the central item; manage them to success.** Orama Zetta is about **creating and managing AI/data projects** — and *most AI projects fail* (Andrew Ng), so the product's real job is the discipline that makes them succeed: problem → right data → correct-by-construction model → governed deploy → monitor + iterate. The central, user-managed artifact is the **AI project** (`Project`), composed of **optional components** the user controls. A **use case** (e.g. churn intelligence) is a **generic AI-project type** any team instantiates on its own data; balance **real AI intelligence** (a model *trained on the data* — e.g. featurize → XGBoost → SHAP) with being **simple, generic, reusable**. The engine is **general AI** (any model type — XGBoost, RL, LLM), not one method. **Churn (UC-1) is the first use case.**

## The blueprint

Source of truth is the **worktree-manager (wtm) project `ORAMA-ZETTA`** → projected to the [`zetta-docs`](https://github.com/oramasearch/zetta-docs) site + the [`oramasearch/zetta`](https://github.com/oramasearch/zetta/issues) issues. **Don't hand-edit the generated docs or issues** — edit the blueprint, then re-sync (the `zetta-sync` skill).

## Links

- **One-pager (product framing):** https://github.com/oramasearch/amaro-bridge/blob/main/docs/projects/orama-platform-2/one-pager.md
- **Grooming epic:** https://github.com/oramasearch/zetta/issues/279
- **2026-06-24 alignment call:** https://github.com/oramasearch/amaro-bridge/tree/main/docs/meetings/2026-06-24-casey-issac-steve
