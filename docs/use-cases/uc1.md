# UC-1 — Churn Intelligence: the full solution

> **Status:** the first use case for Orama Zetta. Built **simple and generic** first; the agentic-RL extension is deferred. This document is the verbose, end-to-end description of the solution — what it is, why it's shaped this way, the reference implementation, the business case, what's deferred, how it maps to the blueprint, and how it gets built.
>
> **Source of truth** for the issue/blueprint is the wtm `ORAMA-ZETTA` project (artifact `UC-1`, GitHub issue [`oramasearch/zetta#264`](https://github.com/oramasearch/zetta/issues/264)). This doc is the human-readable companion; it does not replace the blueprint.

---

## 1. Executive summary

**Churn intelligence is the first use case in the Orama Zetta catalog.** It is a *generic AI-project type*: any team points it at **its own data** — by declaring the entity it wants to score, the churn label and its horizon, the as-of date, and the available features — and gets back a **trained, explained churn model** plus, optionally, an **action-ready retention playbook**.

The pipeline is deliberately simple, generic, and timeless:

```
featurize (point-in-time)  →  train (XGBoost)  →  explain (SHAP)  →  rank at-risk + per-entity drivers
                                                                          │
                                              ┌───────────────────────────┴───────────────────────────┐
                                     optional: driver → recommended-action → owner     optional: constrained LLM playbook
```

The bar we hold ourselves to: **point at data → a working, explained churn model within ~an hour, deployed and governed.** Even a basic model is useful to most organizations; we ship the simplest correct version first and add sophistication later.

UC-1 is the **exemplar** for the whole catalog. Every future use case (fraud, demand forecasting, lead scoring, …) is the *same shape* with a different label and a different driver→action map. Get this one right and the rest are variations on it.

---

## 2. The product context (why UC-1 is shaped the way it is)

UC-1 only makes sense inside Orama Zetta's product thesis. Five framing decisions drive its design:

1. **AI projects are the central item.** Orama Zetta is about *creating and managing AI/data projects*. The central artifact a user creates, manages, and ships is the **AI project** (`Project` in the domain model). A use case like churn is a **generic AI-project type**; a customer instantiates it on their own data to get a concrete project. (Blueprint: [ADR-25 / #292](https://github.com/oramasearch/zetta/issues/292), [GLO-14 / #294](https://github.com/oramasearch/zetta/issues/294).)

2. **Data-project management — because most AI projects fail.** Per Andrew Ng's data-project discipline, the reason AI projects fail is rarely the model: they start backward ("we have data/AI, what can we do?"), bolt stochastic models onto messy foundations, overpromise, and never reach production. Orama Zetta's real job is the **management discipline** that makes a project *succeed*: **problem → right data → correct-by-construction model → governed deploy → monitor + iterate.** UC-1 runs that whole lifecycle, not a one-shot answer.

3. **Generic-yet-intelligent.** Every use case must balance *real AI intelligence* — a model **trained on the customer's data**, not inference over a rented frontier model — with being *generic enough to reuse* across teams. "Unless you're training a model, you're not learning from your data." The engine is **general AI** (XGBoost today; RL and LLM fine-tuning later), not a single method. (Blueprint: [ADR-26 / #293](https://github.com/oramasearch/zetta/issues/293).)

4. **Use-case-driven development.** Each release is a *consumable slice* of a use case; priorities trace the use case's dependency closure. Churn (UC-1) is the first. (Blueprint: [ADR-23 / #290](https://github.com/oramasearch/zetta/issues/290).)

5. **Correct by construction.** Generating a workflow is easy; generating a *correct* one is the product. Every execution graph is validated (schema, leakage, point-in-time) and must pass a **correctness gate** before it can deploy. That gate is the line between Orama Zetta and "n8n with an LLM bolted on." (Blueprint: [FEAT-3 / #134](https://github.com/oramasearch/zetta/issues/134).)

Product framing in depth: [`docs/projects/orama-platform-2/one-pager.md`](projects/orama-platform-2/one-pager.md) and [`vision-and-strategy.md`](projects/orama-platform-2/vision-and-strategy.md).

---

## 3. What UC-1 produces

For a given dataset, UC-1 produces, in order of increasing optionality:

1. **A calibrated churn-risk score per entity** — `P(churn within horizon)`, calibrated so the number means what it says.
2. **A ranked list of at-risk entities** — ordered by *expected retained value*, not raw probability (a high-probability, low-value account is not the best use of a CSM's hour).
3. **Per-entity reason codes (the "why")** — the top signed SHAP drivers, mapped to human-readable signals (e.g. "Account going dark / adoption decay", "Payment failures (involuntary churn)").
4. **(Optional) A recommended action + owner** — a deterministic driver→play→owner mapping ("Re-onboarding + champion check-in" → CSM; "Dunning + card-update outreach" → Billing).
5. **(Optional) An LLM-written playbook entry** — a concise diagnosis, the single highest-leverage play, the owner, urgency, and talking points, in a strict schema, with the LLM constrained so it *narrates and selects* but never invents numbers.
6. **An outcome log** — every recommendation is logged with `outcome = NULL`; once the label horizon resolves, the outcome is filled in. This is the feedback dataset a later uplift/bandit layer will train on.

The first three are the **generic engine**. The last three are the **use-case skin** (churn-specific driver dictionary, save-rate priors, and the retention-playbook schema) plus the feedback loop. Both are user-toggleable optionality — a team can take just the ranked-with-reasons output, or the full playbook.

---

## 4. The pipeline, stage by stage

### 4.1 Point-in-time discipline (the non-negotiable foundation)

The single most important correctness property: **no leakage**. Every feature for an entity at decision time `t0 = as_of_date` must be computed using only events with `event_ts <= t0`. The realized churn label and any post-`t0` signal live *only* in the training target — never in a feature, never readable by the model at scoring time.

This is enforced in the feature SQL (`churn_features.sql`): every aggregation is windowed against `as_of_date`. The label is defined over a forward horizon (did the entity churn within N days *after* `t0`), with **censoring** for accounts still active at the end of the observation window. This temporal hygiene is what separates a model that looks brilliant offline and fails in production from one that holds up.

A standard set of columns is excluded from the feature matrix as either keys or leaky targets: `account_id`, `as_of_date`, `churned_label`, `subscription_status`, `churn_date`, `cancellation_reason`, `end_date`, plus any precomputed `churn_prob` / `churned_actual`. Categorical columns (`plan_tier`, `industry`, `region`) are typed as categories for the model.

### 4.2 Featurization

The feature families are generic across B2B churn and computed point-in-time:

- **Engagement / adoption** — `days_since_last_login`, `wau_30d`, `wau_pct_change_30d_vs_prev`, `active_days_30d`, `active_seat_ratio`, `distinct_features_used_90d`.
- **Commercial** — `mrr` (→ ARR proxy), `days_to_renewal`, `seat_change_90d`, `recent_downgrade_flag`, `num_integrations`, `tenure_days`.
- **Billing health** — `failed_payments_90d` (involuntary churn — cheap to save, high save rate).
- **Support health** — `open_tickets`, `high_sev_tickets_90d`, `avg_resolution_hours_90d`, `avg_csat_90d`.
- **Relationship** — `days_since_csm_contact`, `qbrs_180d`, `nps_latest`, `email_open_rate_90d`.

These are the *generic* features. A specific customer's instance adds or substitutes domain features from their own data; the engine doesn't care what the columns are, only that they're point-in-time-correct.

### 4.3 The model — XGBoost, calibrated

A gradient-boosted tree classifier (**XGBoost**) is the baseline, and a very good one: it handles mixed numeric/categorical features, missingness, and non-linear interactions out of the box, trains fast, and is the industry-standard first model for tabular churn. The reference trainer (`train_churn_xgb.py`) trains the classifier and persists it to `churn_xgb.json` (loaded by downstream steps when present).

Two properties matter beyond raw accuracy:

- **Calibration** — the score must be a *probability*, not just a ranking, because downstream priority math multiplies by it. (Calibrate post-hoc if the raw scores are over-confident.)
- **Class imbalance** — churners are the minority; handle with class weighting / appropriate evaluation (PR-AUC, calibration, lift@k — not raw accuracy).

The model is the "intelligence." It is deliberately *not* a frontier-LLM guess: a trained model has real predictive power, explains *why*, and compounds as it sees more data. The engine is general, though — XGBoost is the first recipe in the kitchen, not the kitchen; RL agents and fine-tuned LLMs are later recipes for use cases that need them.

### 4.4 Explanation — SHAP reason codes (deterministic)

For each scored entity, **SHAP** (TreeExplainer over the XGBoost model) yields per-feature contributions. The reference takes the **top-k (=5) signed drivers** by absolute contribution and maps each through a **driver dictionary** (`DRIVER_MAP`) to:

- a **human-readable signal** ("Weekly active users falling", "Renewal cliff approaching", "Unresolved open support tickets", …),
- the **direction that is bad** (high/low/any),
- an **owner hint** (CSM / AM / Billing / Support / Marketing),
- a **canonical play** ("Value review / re-enablement", "Early renewal motion / term offer", "Escalate + resolve", …).

A positive SHAP value pushes churn probability up; that sign is preserved in the reason code (`pushes_churn`). This is the deterministic, auditable "why" — the same input always yields the same reason codes.

### 4.5 Prioritization — expected retained value (stays out of the LLM)

Ranking by `P(churn)` alone wastes effort on cheap-to-lose, low-value accounts. The reference ranks by **expected retained value**, computed deterministically in code (so it's auditable and never hallucinated):

```
priority = P(churn) × ARR_proxy × P(save | dominant driver)
         where ARR_proxy = mrr × 12
```

`P(save | dominant driver)` comes from an **editable save-rate prior** (`SAVE_RATE_PRIOR`): involuntary/billing issues are cheap and high-save (`failed_payments` ≈ 0.75); competitive/contraction issues are hard to save (`recent_downgrade` ≈ 0.30); a default of 0.35 covers the rest. These start as priors and are **replaced by learned uplift estimates** once the outcome log has data. The top-N by expected retained value (a capacity constraint — a team can only work so many accounts) go forward to the playbook step.

### 4.6 The playbook — a constrained LLM (narrative + play selection only)

The optional playbook step batches the top-N accounts to a **frontier LLM** under a strict contract: it is a Customer-Success strategist that **uses only the provided drivers and numbers, never invents metrics, never changes the computed priority/ARR/probability**, diagnoses the dominant churn *story* from the signed drivers, picks **one** primary play and the right owner, and (importantly) prioritizes involuntary/billing issues first when drivers conflict. Output must match a strict JSON schema:

```json
{ "account_id", "diagnosis", "primary_driver", "recommended_play",
  "owner", "urgency", "talking_points": [3], "confidence" }
```

Batching (N accounts per call) + forced JSON keeps it fast and cheap at book scale and makes the output drop straight into a CRM. Providers are auto-detected (OpenAI / Anthropic / any OpenAI-compatible endpoint such as vLLM), and there is always a **deterministic rules-engine fallback** that synthesizes a playbook from the driver dictionary alone — so the pipeline runs end-to-end offline, with no API key, and is fully testable.

The division of labor is the whole point: **the model and the math do the quantitative work (probability, drivers, priority); the LLM does only language and play-selection.** That is what makes the output trustworthy.

### 4.7 The outcome log (the feedback loop)

Every recommendation is appended to an **outcome log** with `outcome = NULL` and `action_taken = NULL`. A later job fills in `renewed`/`churned` once the horizon resolves, and records what the owner actually did. This is the dataset that turns the static save-rate priors into **learned save-rate-by-driver** and, eventually, a contextual-bandit / uplift policy for *which* action to take — the bridge from "predict churn" to "act optimally on churn." (That learning layer is deferred; the log is laid down now so the data exists when we build it.)

---

## 5. The reference implementation (the deployable v1)

A working v1 reference exists today (currently local; to be brought into the build). It is intentionally small and dependency-light:

| File | Role |
|---|---|
| `churn_features.sql` | Point-in-time feature engineering in Snowflake — temporal label definition, windowed aggregations (`event_ts <= as_of_date`), censoring of active accounts, the JSONL task-row shape. |
| `train_churn_xgb.py` | Trains the calibrated XGBoost classifier; persists `churn_xgb.json`. |
| `churn_playbook.py` | The deployable v1: load model + scored features → SHAP reason codes → deterministic priority → constrained LLM (or deterministic fallback) playbook → CRM-ready CSV/Parquet + outcome log. |

`churn_env_design.md` is the deeper design document (problem framing, PIT plumbing, reward design, training recipe, artifacts, pitfalls) and includes **§8 — the Retention Investigation Agent**, the deferred agentic-RL extension (see §7 below).

This is the shape every use case shares. To turn it into a *generic* engine, the use-case-specific parts — the **label definition**, the **driver dictionary**, and the **save-rate priors / playbook schema** — become configuration; the featurize→train→explain→rank core is reused unchanged.

---

## 6. Genericity — how any team instantiates it

UC-1 is not "Charles's churn model." It is a **type** any team instantiates by declaring a small contract:

- **Entity** — the unit to score (account, customer, subscription, …).
- **Label** — the churn definition + **horizon** (e.g. "no renewal / cancellation within 90 days") + censoring rule.
- **As-of / time column** — what defines `t0`, so features stay point-in-time-correct.
- **Features** — the columns available (the engine uses whatever is provided; the generic families above are the starting palette).
- **(Optional)** the driver→action map + save-rate priors for *their* retention motions.

Given that contract, the same engine produces the same kind of output on any org's data. The customer's data never leaves their environment (compute-near-data); the model is trained *on their data*, so it learns *their* churn.

---

## 7. Scope — what's in the first cut, and what's deferred

### In scope (the first cut)
The simple supervised solution described in §3–§6: featurize → XGBoost → SHAP → ranked at-risk + drivers, with the optional deterministic action layer and the optional constrained-LLM playbook. Keep it simple; ship it; make it correct, governed, and deployed.

### Deferred — the agentic-RL extension (the Retention Investigation Agent)
A substantial body of design + a runnable prototype exist for a *next* layer: instead of a one-shot scorer, an **agent** that *investigates* churn — calls tools (score, explain, query usage, read support, query billing), corroborates the SHAP hint with real evidence, and submits a diagnosis + retention play that is **verified against labeled history** (RLVR — RL with Verifiable Rewards). It is trained with **GRPO** via either **NeMo Gym + NeMo RL** or **TRL + OpenEnv**, sharing one environment core (synthetic point-in-time data + a tool belt + a composite verifiable reward: calibration skill, driver-match gated on having called the corroborating tool, playbook-quality rubric, minus tool-cost and format penalties).

This is **explicitly out of scope for the first cut.** It is the same churn environment with an agent + verifiable reward layered on top once the simple model is shipped and the outcome log has data. Captured here so the research isn't lost; *do not build it before the simple model ships.*

---

## 8. Business context — Motive / Charles (the first instance)

The first customer-shaped instance of UC-1 is **Motive** (a large fleet-management SaaS), via **Charles** (RevOps/Data lead). He is the bottleneck every data/AI request funnels through — "you're too slow," for two years.

- **The status quo:** Charles runs churn today as **n8n + a local sidecar + Gemini** — LLM pattern-matching that compares active-account conversations against churned-cohort patterns and emits 0–100 risk scores, then files Jira tickets. It works, but it has **no real predictive power** (no trained model, no feature engineering, no calibrated probabilities), and it isn't fast, governed, or self-serve. He is the single point of failure.
- **What he is paying for:** a **fast, governed, downloadable substrate** his developers can run themselves — *"discover my data sources, let me use them, configure them, apply a security model on top… it doesn't have to be jazzy."* Orama Zetta's differentiation on top of that substrate is the **real trained model** (the "learns from your data" thesis).
- **The make-or-break gate:** the **FedEx Freight entity-resolution test** ([TC-4 / #251](https://github.com/oramasearch/zetta/issues/251)) — FedEx appears as 5–6 subsidiaries as separate CRM accounts; a query for "FedEx Freight" must resolve to the right account and not conflate them. This is the concrete correctness bar Charles will judge us on.
- **Data + governance:** Snowflake (communications), Salesforce (accounts), Postgres (working store); access is **key-scoped** ([RULE-3 / #211](https://github.com/oramasearch/zetta/issues/211)); production eventually runs in Motive's own AWS/Kubernetes behind Okta. Data access during the PoC is via a contractor engagement (NDA signed; SOC 2 observation complete).
- **The deal:** ~$40K contingent proof-of-concept (free if it fails the eval gates) → ~$20K/month → earned annual. A **~2–3 week downloadable churn beta** is the evaluation, run head-to-head against the internal Retool churn app.

Detail: [`motive-problem-definition`](projects/motive-problem-definition/) · [`motive-delivery`](projects/motive-delivery/) · [`motive-one-proposal`](projects/motive-one-proposal/). Source meetings: [2026-06-24](meetings/2026-06-24-casey-issac-steve/) · [2026-06-10 (Charles)](meetings/2026-06-10-casey-charles-issac-matt-steve/) · [2026-06-03 (the n8n workflows)](meetings/2026-06-03-casey-charles-issac-juan-matt-nathanael-steve/).

**Important framing:** Charles is the *first instance*, not the spec. UC-1 is the generic churn type; Motive is where we prove it. Don't let Motive-specific detail collapse the use case into a bespoke build.

---

## 9. How UC-1 maps to the blueprint

UC-1 is artifact `UC-1` in the wtm `ORAMA-ZETTA` project ([issue #264](https://github.com/oramasearch/zetta/issues/264)). Its **dependency closure** — the features the use case needs, which drive build priority — is wired as `depends_on`:

| Capability | Blueprint | Role in UC-1 |
|---|---|---|
| Ingest — intent + artifacts | [FEAT-1 / #132](https://github.com/oramasearch/zetta/issues/132) | bring the customer's data + the churn problem in |
| Architect — compile to execution-graph IR | [FEAT-2 / #133](https://github.com/oramasearch/zetta/issues/133) | compile the featurize→train→explain→rank graph |
| Correctness gate — validate before deploy | [FEAT-3 / #134](https://github.com/oramasearch/zetta/issues/134) | schema / leakage / point-in-time validation |
| Learns from your data | [FEAT-6 / #137](https://github.com/oramasearch/zetta/issues/137) | featurize + train the model (the intelligence) |
| One-button deploy | [FEAT-4 / #135](https://github.com/oramasearch/zetta/issues/135) | deploy the project in-firewall |
| Run + monitor + lineage | [FEAT-5 / #136](https://github.com/oramasearch/zetta/issues/136) | execute, score, observe |
| Governed self-service | [FEAT-9 / #140](https://github.com/oramasearch/zetta/issues/140) | key-scoped, audited, devs self-serve |

Plus `relates_to` [RULE-3 / #211](https://github.com/oramasearch/zetta/issues/211) (key-scoped access) and [TC-4 / #251](https://github.com/oramasearch/zetta/issues/251) (entity-resolution correctness). The blueprint grooming that produced this state is epic [#279](https://github.com/oramasearch/zetta/issues/279).

---

## 10. Evaluation & the success bar

- **Correctness gate (release-blocking):** every generated graph compiles AND runs AND produces correct results; validation covers schema, data leakage, and point-in-time integrity.
- **Model quality:** PR-AUC / AUC, **calibration**, and **lift@k** (does the top decile actually contain the churners?) — not raw accuracy. Backtest **out-of-time** (train on earlier `t0`s, evaluate on later ones).
- **The entity-resolution gate:** the FedEx Freight test (TC-4) must pass — correct account scoping, no conflation.
- **The product bar:** a user points at data and gets a working, explained churn model **within ~an hour**, deployed and governed.
- **The compounding bar (later):** the outcome log lets us measure realized save-rate-by-driver and improve the priors → uplift.

---

## 11. How UC-1 gets built — the worktree plan

UC-1 is built via a **wtm worktree plan** — an execution graph over its GitHub issues that launches parallel worktrees.

**Plan #11 — "Churn intelligence (UC-1) — first use case"** (built over UC-1 + its dependency closure). Computed DAG:

```
layer 0  (launch first, in parallel)
   FEAT-1 ingest    FEAT-4 deploy    FEAT-6 train-the-model    FEAT-9 governed
layer 1   FEAT-2 architect                 (← FEAT-1)
layer 2   FEAT-3 correctness gate · FEAT-5 run + monitor   (← FEAT-2)
layer 3   UC-1 churn                        (← FEAT-3, FEAT-4, FEAT-5, FEAT-6, FEAT-9)
```

The **run loop** (being polished in [`homer6/worktree-manager#10`](https://github.com/homer6/worktree-manager/issues/10)): launch a layer → each task runs in its own worktree and opens a PR → **the plan pauses for PR review** → after review/merge, resume → launch the next ready layer → … → UC-1 ships. The **PR-review gate** is the deliberate stop/resume point: a human reviews each completed task before its dependents proceed.

---

## 12. UC-1 as the catalog exemplar

Churn is the template for the entire catalog. The next use case — fraud/anomaly, demand forecasting, lead scoring, ticket triage, credit risk, predictive maintenance, next-best-action — is the **same generic shape** (featurize → train → explain → rank + optional action/playbook) with a different label and driver→action map. Add one with the **`add-use-case`** skill + the [`adding-a-use-case` runbook](runbooks/adding-a-use-case.md): it generates a tailored **Perplexity Deep Research prompt** to gather the domain knowledge, which lands a new `UC-N` artifact in the blueprint and gets its own worktree plan. Each addition reuses the engine and grows the menu.

---

## 13. References

- **Blueprint / issue:** [`oramasearch/zetta#264`](https://github.com/oramasearch/zetta/issues/264) (UC-1) · grooming epic [#279](https://github.com/oramasearch/zetta/issues/279) · the consolidated context comment on #264.
- **Framing decisions:** [ADR-23 use-case-driven](https://github.com/oramasearch/zetta/issues/290) · [ADR-25 AI-projects-central / data-project-management](https://github.com/oramasearch/zetta/issues/292) · [ADR-26 generic AI-project types](https://github.com/oramasearch/zetta/issues/293) · [GLO-14 AI project](https://github.com/oramasearch/zetta/issues/294).
- **Product docs:** [`projects/orama-platform-2/`](projects/orama-platform-2/) (one-pager, vision-and-strategy) · the zetta module conventions [`modules/zetta/docs/orama-zetta.md`](https://github.com/oramasearch/zetta/blob/main/docs/orama-zetta.md).
- **Catalog growth:** [`runbooks/adding-a-use-case.md`](runbooks/adding-a-use-case.md) + the `add-use-case` skill.
- **Build infra:** worktree plan #11 · the run-loop ticket [`homer6/worktree-manager#10`](https://github.com/homer6/worktree-manager/issues/10).
- **Reference implementation:** `churn_features.sql`, `train_churn_xgb.py`, `churn_playbook.py`, `churn_env_design.md` (incl. §8 the deferred Retention Investigation Agent) — to be committed into the build.
