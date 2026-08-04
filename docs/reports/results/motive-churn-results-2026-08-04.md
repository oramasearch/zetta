# Motive churn — where we are

**2026-08-04** · engine `feat/dataset-path-table` @ `eb5668e` · suite 1932/1932 ·
diagrams: [pipeline](diagrams/motive-churn-flow.svg) ·
[signal structure](diagrams/motive-dataset-signal.svg)

## Where we are

**The churn pipeline runs end to end and produces a ranked at-risk list with reasons.** It
trains on a 1,500-account communications dataset built to the exact shape of
`fact_customer_communications_events`, so the query that drives it is the query that will
drive the real one.

**The pipeline is the result. The model's accuracy is not yet.**

| | | |
|---|---|---|
| **Held-out AUC** | **0.62** | the honest number — 450 accounts the model never saw |
| In-sample AUC | 0.92 | over the rows it trained on; **not** a performance claim |
| Logistic-regression baseline | 0.756 | same rows, same holdout |
| Accounts scored | 1,500 | ranked by risk |
| Top driver | `escalations_90d` | TreeSHAP mean \|φ\| 0.53 |
| Churn base rate | 17.5% | |

**Read the first three rows together.** The gap between 0.92 in-sample and 0.62 held out is
overfitting: the model has partly memorised the accounts it trained on. And a plain
logistic regression reaches **0.756** on the same split — so the signal in the data
supports a materially better model than the one currently being produced. Tightening the
model's regularization recovers only part of it (0.65), so the configuration needs work
rather than a knob turn.

This is stated first because an in-sample figure quoted as a result is the most common way
a churn model flatters itself, and 0.92 is exactly the kind of number that survives into a
slide unchallenged.

## What the pipeline does today

![pipeline](diagrams/motive-churn-flow.svg)

**Data in.** A read-only query against a saved connection lands rows as an internal
dataset — Parquet, partitioned across the cluster, addressed `database.table`. The source
is done at that point; everything after works from the internal copy.

**One pipeline, with the model as a step inside it.** Connect → featurize → train →
explain → rank. There is no separate "now go train a model": running the pipeline is what
trains it.

- **The column contract** works out which column is the entity, which is the label, and
  which are the features — and **reports every assumption back** instead of burying it in
  a model. Anything inferred can be overridden.
- **The point-in-time cutoff is never guessed.** A cutoff decides which rows a feature may
  see, and therefore whether the model is honest. It applies only when named.
- **Reads happen near the data.** Each node reads only the partitions it holds; rows never
  cross the wire to a coordinator.

**What comes back.** A registered model that is kept and re-scorable; the ranked at-risk
list; and the drivers below. Asked something the data cannot support, it declines rather
than producing a number.

## What drives churn, on this data

TreeSHAP over the trained model — mean |φ|, strongest first:

| Driver | Weight | |
|---|---|---|
| `escalations_90d` | **0.527** | repeat support escalations in the last quarter |
| `billing_disputes_180d` | **0.332** | invoice friction over six months |
| `unresolved_contacts_90d` | **0.306** | contacts that went unanswered |
| `support_events_90d` | 0.264 | overall support volume |
| `days_since_last_sales_touch` | 0.244 | the account stopped taking sales calls |
| `escalation_trend` | 0.205 | last quarter against the one before |
| `nps_score` | 0.156 | |
| `tenure_days` · `seats` | 0.056 · 0.046 | **size, near the bottom — as it should be** |

Two things to read from that ordering.

**No single column carries the model.** The strongest feature scores 0.642 AUC on its own,
and no column exceeds it — so nothing here is a one-alarm dataset. A dataset where a single
column predicts the label teaches nothing, and a model trained on one falls over on the
first account that breaks the pattern.

**Size sits at the bottom.** `seats` and `tenure_days` are near-zero drivers. A large fleet
generates more of everything, and a churn model that quietly ranks by account size is the
most common way this goes wrong.

## Two properties of the source data worth handling up front

Both come from the table's own documentation, and both are reproduced in the demo dataset
so they are handled rather than met live.

**`current`-structured rows carry present state under an opening-date timestamp.**
`sfdc.case` and the Ada chats are continuously updated but stamped when they opened, and
`event_updated_datetime_pt` is the pipeline's load date rather than the source's change
date. A point-in-time feature over one of those rows reads state from after the decision
date — which looks excellent in testing and fails in production, because in testing it was
reading the answer.

**Attribution to an account varies sharply by channel** — `sfdc_account_id` is absent on
~0.05% of cases and ~59% of Salesloft transcripts. Any account-level aggregate is therefore
computed over a biased sample of the conversation, with the bias differing by channel. It
matters for interpretation: "this account went quiet on sales calls" is a statement about a
channel where much of the evidence cannot be attributed.

## Next steps

1. **Close the generalization gap.** A plain logistic regression beats the current model by
   0.14 AUC on the same split, so the configuration — not the data — is the limit. This is
   the work that turns a working pipeline into a usable model.
2. **Run against the full ~12 GB event log.** The pipeline is unchanged; this is a scale
   and connection step, and it is also the only way to learn what churn accuracy is
   actually achievable here.
3. **Bring in the SFDC account dimension.** `CHURN_DATE`, `LAST_CHURN_REASON` and the
   finance segments live there rather than in the events table — it carries the label the
   model trains against.
4. **Agree the scoring date and horizon.** Which as-of date accounts should be scored on,
   and how far forward "churned" means. Deliberately not guessed.
5. **The report as an artifact** — model quality and drivers as something opened rather
   than logs read. Everything it needs now exists.
6. **Close one open defect.** After a run finishes, our API layer stops serving read
   requests. The pipeline itself is verified; this blocks the live click-through and is a
   single characterised defect with a reproduction, not a design problem.

## Where this comes from

Every number above is printed by one test, on branch `feat/dataset-path-table`:

```
python3 scripts/gen_motive_demo_dataset.py --out data/demo/motive --accounts 1500
ZETTA_MOTIVE_DEMO_DIR=data/demo/motive ./build/tests/zetta-tests \
    --gtest_filter='MotiveDemoDataset.*'
```

| What | Where |
|---|---|
| **The measurement** — 70/30 split, train, score the held-out rows, compute AUC | [`tests/usecases/churn/test_motive_demo_dataset.cpp:161`](../../../tests/usecases/churn/test_motive_demo_dataset.cpp) |
| **Why the in-sample figure is in-sample** — each node scores its own training rows | [`src/core/ml/gbdt_train_job.cpp:307`](../../../src/core/ml/gbdt_train_job.cpp) |
| **The dataset generator**, and the reasoning for every choice in it | [`scripts/gen_motive_demo_dataset.py`](../../../scripts/gen_motive_demo_dataset.py) |
| **The pipeline run over a connector-landed table** | [`src/core/usecases/churn/churn_solution.cpp:453`](../../../src/core/usecases/churn/churn_solution.cpp) |
| **Scoring rows the model never saw** | [`src/core/ml/score_table.cpp:127`](../../../src/core/ml/score_table.cpp) |
| **The column contract** — what is inferred, and what is never inferred | [`src/core/data/dataset_path.cpp`](../../../src/core/data/dataset_path.cpp) · [docs](../../data/dataset-path.md) |
| **Asking the results questions in chat** | [docs/agent/chatting-with-results.md](../../agent/chatting-with-results.md) |

The generator is deterministic — the same seed produces byte-identical rows, so these
numbers reproduce exactly rather than approximately.

The test also recomputes the in-sample AUC with the same arithmetic it uses for the
holdout and asserts the two agree with what the engine reports (0.919417 both). Without
that check the held-out figure would not be comparable to anything.

---

*No Motive customer data was used. Every row referenced here is synthetic, generated to the
documented shape of the source table. Engine work:
[zetta#947](https://github.com/oramasearch/zetta/issues/947) ·
[#948](https://github.com/oramasearch/zetta/issues/948) ·
[#952](https://github.com/oramasearch/zetta/issues/952).*
