# Explaining one row — "why is THIS one at risk?"

**Issues:** [#948](https://github.com/oramasearch/zetta/issues/948) step 6 ·
[#960](https://github.com/oramasearch/zetta/issues/960) item 5 ·
**Capability:** `explain` ·
**Code:** `include/zetta/ml/explain_row.hpp`, `src/core/ml/explain_row.cpp` ·
**Tests:** `tests/usecases/churn/test_churn_table_run.cpp`

A run's **drivers** are the population ranking: escalations matter most, billing
disputes second. That list is identical for every account, so it cannot answer the
question a person asks the moment they open the at-risk list — *why is the top one
there?* Two accounts with the same score can be there for opposite reasons, and the
population ranking hides exactly that.

`explain` gives the per-account answer: the exact signed contribution of every feature
for one row.

## Reading it

```jsonc
// an account in the data
GET /v1/explain?model=<id>&dataset_path=churn_demo.accounts&entity=8336

// an account that is not in the data at all
POST-equivalent input: { "model": "<id>", "row": { "failed_payments": 9, … } }
```

```jsonc
{ "entity": 8336,
  "base_value": -1.42,      // what an account with nothing notable would score
  "raw": 1.13,              // this account's raw score
  "risk": 0.756,            // the same prediction as a probability
  "residual": 0.0,
  "contributions": [        // strongest influence first
    { "feature": "escalations_90d",   "value": 19, "contribution":  1.31 },
    { "feature": "logins_30d",        "value": 41, "contribution": -0.42 },
    …
  ] }
```

## Nothing is stored

`tree_shap` is a pure function of `(model, features)` — no device, no I/O, no shared
state — and one row costs a walk of the trees. So the explanation is **recomputed from
the registered Model** whenever it is asked for, exactly as
[`score_table`](gbdt-score.md) recomputes the ranking. A stored copy would be a second
version of a number that can drift from the model that produced it.

The engine already computes these during a run's `explain` step, for every entity at
once. Those live in the step executor's memory and die with the run, which is why this
exists.

## Three properties that make the answer trustworthy

**It is exact, and it says so.** SHAP is additive in the model's additive output, so:

```
raw == base_value + Σ contributions
```

`residual` carries the leftover, so a caller can *check* that identity rather than
trust it. The tests assert it to 1e-6.

**Raw-score space, not probability.** Probabilities are not additive, so contributions
that summed to a risk would be arithmetic nobody could verify. The risk is reported
beside them instead. This is why `contribution` is a logit-space number and not a
percentage.

**Every feature appears, ordered by |contribution|.** A feature that did nothing for
this row sorts last rather than being dropped — *"this column did not matter for this
account"* is an answer, and its absence would read as an oversight.

## Two rules carried over from scoring

**The model's feature order is the truth.** A model trained on
`[failed_payments, logins_30d]` explained against some other ordering attributes each
contribution to the wrong column — a wrong answer that looks exactly like a right one.

**A missing feature is a hard failure, never a zero-fill.** Zero is a real value to a
GBDT, so a missing column would be explained as *"this account has none of these"*
rather than reported as missing.

## What a contribution is not

**It is not a cause.** It says how much this account's value for that column moved the
model's output relative to a typical account. Whether the column *causes* the outcome
is a question the model was never asked, and the causal arrow often runs the other way
(an account on its way out raises more escalations).

## Placement

Reading an entity from a dataset searches **this node's own partitions**, matching
`score_table` — the row never crosses the wire. On a cluster, an entity owned by
another node reports not-found, and the error says so in those terms rather than
"no such account", because otherwise someone goes looking for a data problem that is
not there.

## Boundaries

No new JSON format and no new storage. `explain` is a read capability (`GET`), on the
REST surface, the MCP tool list, and the agent's read-tool allowlist — so chat can
answer *"why is the top one at risk?"*, which is the whole point.

## Later slices

- Explaining an entity owned by another node (a scatter-gather, or routing the request
  to the owner).
- Explaining many rows in one call; the batch path exists inside a run and is not
  exposed.
