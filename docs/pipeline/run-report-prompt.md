# The run report — the artifact, and the prompt that writes it

**Issue:** [#948](https://github.com/oramasearch/zetta/issues/948) step 4 ·
**Example output:** [`docs/reports/results/motive-churn-run-report-2026-08-04.md`](../reports/results/motive-churn-run-report-2026-08-04.md) ·
**Runbook:** [regenerating-the-run-report.md](../runbooks/regenerating-the-run-report.md)

A run produces a **report**: how good the model is, what drives the outcome, and which
entities are most at risk. An artifact someone opens — not run logs they read.

## The rule that makes it trustworthy

**The engine computes every number. The narrator may only arrange them.**

This is the sanctioned narrative layer from ADR-17: it "writes narrative and picks a play
from numbers the engine already computed — it never produces a number, and nothing depends
on it." A narrator that can compute is a narrator that can be *wrong* about a figure, and a
report with one invented number in it is worth less than no report, because now every
number needs checking.

So the pipeline emits a **facts artifact** and the prompt below turns that — and only that —
into prose.

## The facts artifact

Written at the end of a run. Everything a report needs and nothing it would have to derive:

```jsonc
{
  "dataset": { "accounts": 15000, "events": 3870209, "churn_rate": 0.1843,
               "features": ["escalations_90d", "..."] },
  "model":   { "id": "...", "held_out_auc": 0.7259, "in_sample_auc": 0.7697,
               "accuracy": 0.8231, "log_loss": 0.4081,
               "train_rows": 10500, "holdout_rows": 4500 },
  "drivers": [ { "feature": "escalations_90d", "weight": 0.5275 }, … ],
  "at_risk": [ { "rank": 1, "entity": 8336, "risk": 0.7657,
                 "evidence": { "escalations_90d": 19, "unresolved_contacts_90d": 26,
                               "billing_disputes_180d": 37 } }, … ]
}
```

Two details in there are deliberate.

**`evidence` carries the top-ranked drivers' values, chosen by the driver ranking** — not
hand-picked per entity. Otherwise a report could quietly showcase whichever numbers happen
to look worst for each account, which is a way of lying with true figures.

**Both AUCs are present.** The gap between them is the overfitting signal, and it can only
be read if both are there.

## The prompt

> You are writing the report for a completed pipeline run. You are given a JSON facts
> object. Produce Markdown.
>
> **Absolute rule: every number in your output must appear verbatim in the facts object, or
> be a direct restatement of one (a percentage of a given fraction, a count of a given
> list). You may not compute, estimate, average, extrapolate, or infer any figure. If
> something would be useful and is not in the facts, say it is not available — never supply
> it.**
>
> Structure:
>
> 1. **What this run produced** — one short paragraph. What was trained, on how much data,
>    and what came out.
> 2. **How good the model is.** State the held-out figure first and say plainly what it was
>    measured on. State the in-sample figure beside it and say what the gap between them
>    means: a large gap is memorisation, a small one is generalisation. Never present the
>    in-sample number alone.
> 3. **What drives the outcome** — the drivers as a table, strongest first, each with a
>    plain-language gloss of what the column measures. Say whether any single driver
>    dominates.
> 4. **Who is most at risk** — the ranked entities with their risk and their evidence
>    values. Name the entity key exactly as given; do not invent business names for them.
> 5. **What to check before acting on this** — the limits that apply to this run.
>
> Rules of tone and judgement:
>
> - **Do not editorialise the quality.** Report the figures and what they mean structurally.
>   "0.73 held out" is a fact; "excellent performance" is a claim you cannot support without
>   a comparison you were not given.
> - **A driver is not a cause.** Say a feature carries weight in the model, never that it
>   *causes* churn.
> - **Do not recommend actions.** The report says what was found. What to do about it is the
>   reader's call, and a playbook is a separate artifact.
> - If any expected field is missing from the facts, say so where it would have appeared.
>
> Length: under 800 words. A report someone reads before a meeting, not a document they
> schedule time for.

## Why the prompt is this restrictive

Every clause above is a failure that has actually happened somewhere in this class of
system: a narrator averaging two figures into a third that was never measured; an in-sample
score presented as performance; a feature described as *causing* an outcome; a confident
recommendation drawn from a driver ranking that only says what the model weighted.

The restriction is not distrust of the model writing the prose. It is that **the report's
value is that its numbers can be checked**, and every degree of freedom the narrator has is
a number that has to be checked twice.

## Status

The facts artifact is emitted today by
`tests/usecases/churn/test_motive_demo_dataset.cpp`. Emitting it from the run itself — as a
`RunOutput` of kind `report` alongside `model` and `dataset` — is the remaining engine work
for #948 step 4.
