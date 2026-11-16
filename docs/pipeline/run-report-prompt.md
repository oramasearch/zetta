# The run report — the artifact, and the prompt that writes it

**Issue:** [#948](https://github.com/oramasearch/zetta/issues/948) step 4 ·
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
               "majority_class_accuracy": 0.8157,
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

Three details in there are deliberate.

**`evidence` carries the top-ranked drivers' values, chosen by the driver ranking** — not
hand-picked per entity. Otherwise a report could quietly showcase whichever numbers happen
to look worst for each account, which is a way of lying with true figures.

**Both AUCs are present.** The gap between them is the overfitting signal, and it can only
be read if both are there.

**`majority_class_accuracy` is the accuracy a constant answer would achieve.** It exists
because a report that prints accuracy without it is misleading by omission, and because the
alternative — letting the writer work it out — is exactly the computation the rule forbids.
**When the prose needs a number the artifact lacks, the artifact is what changes.** That is
the general rule this field is the first instance of.

**A field the run cannot supply is named in `missing`, with the reason.** Left blank, an
absent held-out figure invites the writer to lead with the in-sample number and call it
quality. Stated — *"this run set no holdout_fraction, so every figure below is in-sample
and flatters"* — it does not. Quote the reason; do not paper over it.

## The prompt

> You are writing the report for a completed pipeline run. You are given a JSON facts
> object. Produce Markdown.
>
> **Absolute rule: every number in your output must appear verbatim in the facts object, or
> be a direct restatement of one (a percentage of a given fraction, a count of a given
> list). You may not compute, estimate, average, extrapolate, or infer any figure.**
>
> **If a number would genuinely help and is not in the facts, say so and stop. Do not supply
> it.** That is a gap in the artifact to be fixed at the source, not something to work out
> while writing. This has already happened once: a report needed the accuracy a
> constant answer would achieve, the writer computed it, and the number-tracing check
> rejected it. The right repair was to emit it as a fact.
>
> ### Structure
>
> 1. **What this run produced** — one short paragraph. What was trained, on how much data,
>    what came out.
> 2. **How good the model is.** The held-out figure first, saying plainly what it was
>    measured on. The in-sample figure beside it. Never the in-sample number alone.
> 3. **What drives the outcome** — the drivers as a table, strongest first, each with a
>    plain-language gloss of what the column measures.
> 4. **Who is most at risk** — the ranked entities with their risk and their evidence values.
> 5. **What to check before acting on this** — the limits that apply to this run.
>
> ### Every figure needs its meaning, not just its value
>
> A number a reader cannot act on is decoration. For each headline figure, add a short
> paragraph answering "so what". The chart or table answers *what*; the prose answers *so
> what*; a reader who stops at the table has still got the fact right.
>
> **Translate the metric into an event the reader can picture.** For a ranking metric like
> AUC: "draw one account that churned and one that did not; the model gives the churner the
> higher score about N times in a hundred". State what chance is and what perfection is, and
> say that nothing predicting human behavior from this kind of data approaches perfection.
>
> **Give the in-sample/held-out gap a counterexample.** A small gap means nothing to a reader
> who has never seen a large one. Say what a bad gap would look like and what it would imply,
> then say which of the two this run is.
>
> **Put every rate-like metric next to its trivial baseline.** Accuracy is the trap: on any
> problem where one outcome is uncommon, a constant answer scores close to the model. Report
> the majority-class figure from the facts beside it and say plainly that accuracy barely
> separates a working model from a constant answer here. A report that prints accuracy alone
> invites the reader to quote the flattering number.
>
> **Say what the drivers share, not just their order.** A ranked list of columns is data. The
> reading is what the top few have in common, which systems they come from, and what a
> single-source view would therefore miss.
>
> **Say how to read the ranked list.** Across the evidence columns rather than down any one:
> an entity high on several is in a different situation from one carried by a single large
> number, and the second is the likelier false positive. Note whether the list has a head
> (the top scores well clear of the rest) or is flat, because that is what decides whether
> working the top of it is worthwhile.
>
> ### Tone and judgment
>
> - **Do not editorialize quality.** "0.73 held out" is a fact; "excellent" is a comparison
>   you were not given. Explaining a consequence is not editorializing; grading is.
> - **A driver is a weight, not a cause.** Say the model leans on a column, never that the
>   column causes the outcome. Where the causal arrow plausibly runs the other way, say so.
> - **Do not recommend actions.** The report says what was found. A playbook is a separate
>   artifact.
> - **Set expectations honestly.** A ranked list is a way to spend attention where it pays
>   best, not a prediction about any one entity. Some at the top will not convert; some
>   further down will.
> - If any expected field is missing from the facts, say so where it would have appeared.
>
> ### Length
>
> As long as the explanation needs and no longer. Roughly 1,200 to 1,600 words for a run with
> a dozen drivers and a ranked list. The earlier 800-word cap produced a report that listed
> figures and explained none of them.

## What writing these taught

Each of these came from a report that was already written, reviewed, and wrong in a way
nobody noticed until someone asked.

**Figures without prose undersell the work.** The first version was a table of numbers and a
chart. It was accurate and it read as a lab result. Everything a reader needs in order to
act on a figure lives in the sentence after it.

**The flattering number gets quoted.** An in-sample AUC went into a client-facing report as
"model quality" and survived review. Accuracy is the same trap one step later: 82.3% sounds
strong until you know a constant answer scores 81.6%. Both are cases of a true number
carrying a false impression, which is why the prompt now requires the comparison that makes
each one legible.

**A small gap means nothing without a large one to compare it to.** "In sample 0.770, held
out 0.726, gap 0.04" is three numbers. "A model that scored 0.92 and 0.62 would have
memorized its training set and fallen apart in a week; this one behaves the same on both" is
the same three numbers doing work.

**The interesting reading is usually across the drivers, not down them.** The ranked list
says escalations matter most. The insight is that the top three are three systems recording
the same customer asking for help and not getting it, which is what a single-source dashboard
structurally cannot see.

**Say what the report cannot support.** Every honest limit stated up front is one a reader
does not discover at the worst moment. Ranking is not prediction, weights are not causes, and
an uncalibrated score is not a probability.

## Why the prompt is this restrictive

Every clause above is a failure that has actually happened somewhere in this class of
system: a narrator averaging two figures into a third that was never measured; an in-sample
score presented as performance; a feature described as *causing* an outcome; a confident
recommendation drawn from a driver ranking that only says what the model weighted.

The restriction is not distrust of the model writing the prose. It is that **the report's
value is that its numbers can be checked**, and every degree of freedom the narrator has is
a number that has to be checked twice.

## Status

**The engine emits it.** A finished run carries a `RunOutput` of kind `report`, and
`get_run_report` returns the facts —
[`docs/pipeline/run-report.md`](run-report.md) (#958). It is assembled at query time from
the Model record and the run's at-risk table, so a report written a month later quotes the
same numbers as one written the moment the run ended.

```sh
curl "$ZETTA/v1/get_run_report?run_id=<uuid>&limit=20"
```

`tests/usecases/churn/test_comms_demo_dataset.cpp` still writes its own artifact for the
communications demo dataset, which is generated outside any run. For anything the engine actually
ran, `get_run_report` is the source.

**The held-out figure no longer has to be measured outside the engine.** A run that sets
`holdout_fraction` reports `held_out_*` beside `in_sample_*`, with the row count behind
each ([`docs/ml/held-out-metrics.md`](../ml/held-out-metrics.md), #953).
