# Motive churn — where we are

**2026-08-04** · Orama · engine `feat/dataset-path-table` · suite 1932/1932

## What we've built

**A churn pipeline that takes a query against your communications data and returns a ranked
list of at-risk accounts with the reasons behind each one.** It is built and running against
a 15,000-account dataset shaped exactly like
`protected_communications.analytics.fact_customer_communications_events` — so the query that
drives it is the query that will drive yours.

| | |
|---|---|
| **Model quality, held out** | **AUC 0.726** on 4,500 accounts the model never saw |
| **Generalization gap** | **0.04** (in-sample 0.770) — it learned, it did not memorize |
| **Scale** | 15,000 accounts · 3,870,209 communication events |
| **Top driver** | repeat support escalations, by a clear margin |
| **Churn base rate** | 18.4% |

Two of those are worth a moment.

**0.726 is measured on accounts the model has never seen.** Training-set scores are the usual
way a churn model flatters itself, and they are easy to produce. This is not one.

**The 0.04 gap is the quality signal.** A model that scores far better on its own training
data than on new accounts has memorized rather than learned, and it collapses the first time
it meets a real account. This one holds up.

## What drives churn

Computed with TreeSHAP over the trained model — the same method that explains any single
account's score:

| Driver | Weight | |
|---|---|---|
| `escalations_90d` | **0.53** | repeat support escalations in the last quarter |
| `billing_disputes_180d` | **0.33** | invoice friction over six months |
| `unresolved_contacts_90d` | **0.31** | contacts that went unanswered |
| `support_events_90d` | 0.26 | overall support volume |
| `days_since_last_sales_touch` | 0.24 | the account stopped taking sales calls |
| `escalation_trend` | 0.21 | last quarter against the one before |
| `nps_score` | 0.16 | |
| `tenure_days` · `seats` | 0.06 · 0.05 | **size, at the bottom — where it belongs** |

**No single column carries the model** — the strongest scores 0.631 alone against 0.726 for
the model. Risk comes from combining evidence across channels, which is what makes the list
worth working rather than a rename of "accounts with open tickets".

**Account size sits at the bottom.** A large fleet generates more of everything, and a churn
model that quietly ranks by account size is the most common way this goes wrong. This one
doesn't.

![signal structure](diagrams/motive-dataset-signal.svg)

## What the pipeline does

![pipeline](diagrams/motive-churn-flow.svg)

**Data in.** A read-only query against a saved connection lands rows as an internal dataset —
Parquet, partitioned across the cluster. The source is done at that point; everything after
works from the internal copy, and nothing leaves your environment.

**One pipeline, with the model as a step inside it.** Connect → featurize → train → explain →
rank. There is no separate "now go train a model": running the pipeline trains it.

- **The column contract** works out which column is the entity, which is the label, and which
  are the features — and **reports every assumption back** rather than burying it in a model.
  Anything inferred can be overridden.
- **The point-in-time cutoff is never guessed.** A cutoff decides which rows a feature may
  see, and therefore whether the model is honest. It applies only when named.
- **Reads happen near the data.** Each node reads only the partitions it holds; rows never
  cross the wire to a coordinator. This is what lets it scale to the full event log.

**What comes back.** A registered model that is kept, versioned and re-scorable; the ranked
at-risk list, which outlives the run that produced it; and the drivers above. An account not
in the data can be scored on demand. Asked something the data cannot support, it declines
rather than producing a number.

## Two properties of the source data, handled

Both are in the table's own documentation, and both are reproduced in our dataset so the
pipeline handles them rather than meeting them for the first time on live data.

**`current`-structured rows carry present state under an opening-date timestamp.**
`sfdc.case` and the Ada chats are continuously updated but stamped when they opened, and
`event_updated_datetime_pt` is the pipeline's load date rather than the source's change date.
A point-in-time feature over one of those rows would read state from after the decision date
— which looks excellent in testing and fails in production, because in testing it was reading
the answer.

**Attribution to an account varies sharply by channel** — `sfdc_account_id` is absent on
~0.05% of cases and ~59% of Salesloft transcripts. Any account-level aggregate is therefore
over a biased sample of the conversation, with the bias differing by channel. It matters for
interpretation: "this account went quiet on sales calls" is a statement about a channel where
much of the evidence cannot be attributed.

## Next steps

1. **Run against the full ~12 GB event log.** The pipeline is unchanged — a scale and
   connection step, and the point at which the numbers above become numbers about Motive's
   own accounts rather than about a faithful stand-in.
2. **Bring in the SFDC account dimension.** `CHURN_DATE`, `LAST_CHURN_REASON` and the finance
   segments live there rather than in the events table; it carries the label the model trains
   against.
3. **Agree the scoring date and horizon** — which as-of date accounts are scored on, and how
   far forward "churned" means. Deliberately not guessed.
4. **Segment as a feature.** `finance_segment` is a string, so the model does not currently
   see it. Encoding it is straightforward and should lift accuracy further.
5. **The report as an artifact** — model quality and drivers as something opened rather than
   logs read.

## Where the numbers come from

Every figure above is printed by one test on branch `feat/dataset-path-table`:

```
python3 scripts/gen_motive_demo_dataset.py --out data/demo/motive --accounts 15000
ZETTA_MOTIVE_DEMO_DIR=data/demo/motive ./build/tests/zetta-tests \
    --gtest_filter='MotiveDemoDataset.*'
```

| What | Where |
|---|---|
| The measurement — 70/30 split, train, score the held-out rows | [`tests/usecases/churn/test_motive_demo_dataset.cpp`](../../../tests/usecases/churn/test_motive_demo_dataset.cpp) |
| The dataset generator | [`scripts/gen_motive_demo_dataset.py`](../../../scripts/gen_motive_demo_dataset.py) |
| The pipeline run over a landed table | [`src/core/usecases/churn/churn_solution.cpp`](../../../src/core/usecases/churn/churn_solution.cpp) |
| Scoring accounts the model never saw | [`src/core/ml/score_table.cpp`](../../../src/core/ml/score_table.cpp) |
| The column contract | [`src/core/data/dataset_path.cpp`](../../../src/core/data/dataset_path.cpp) · [docs](../../data/dataset-path.md) |
| Asking the results questions in chat | [docs/agent/chatting-with-results.md](../../agent/chatting-with-results.md) |

The generator is deterministic — the same seed produces byte-identical rows, so these
reproduce exactly rather than approximately.

---

*No Motive customer data was used. Every row referenced here is synthetic, generated to the
documented shape of the source table.*
