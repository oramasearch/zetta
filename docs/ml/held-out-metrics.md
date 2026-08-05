# Held-out metrics

**Issue:** [#953](https://github.com/oramasearch/zetta/issues/953) ·
**Code:** `src/core/ml/gbdt_train_job.cpp`, `include/zetta/ml/gbdt.hpp`,
`include/zetta/ml/gbdt_train_job.hpp` ·
**Tests:** `tests/ml/test_gbdt.cpp` (`GbdtHoldout.*`)

A training run can hold a fraction of its rows **out** of training and measure the
model on them separately. `GbdtParams::holdout_fraction` asks for it; it defaults
to `0`, so no existing run's numbers move.

## Why this exists

Before it, the only metrics a run produced were **in-sample**: `run_train_job` had
each node score its own local rows and gathered the pairs, and there was no
train/validation split anywhere in the training path. Those metrics are written
onto the Model record and surfaced as "model quality" by `system.models`,
`get_model`, the chat agent's grounding, and the GUI.

An in-sample AUC of 0.868 reached a client-facing report as the headline model
quality figure. The held-out figure for the same run was 0.726. Neither number was
wrong; the label on the first one was.

## What it measures

| Figure | Measured on |
|---|---|
| `metrics` (`in_sample`) | the rows the trees were **fit** on |
| `held_out` | the rows **no node trained on** |

**In-sample means the fit rows, not every local row.** With no holdout of either
kind those are the same set and the numbers are unchanged. Once rows are held
back, scoring them under the name "in sample" would make the label false and mix
the two populations the split exists to keep apart. A consequence worth knowing:
with calibration on, `train_rows + held_out_rows` is **less** than the row count,
because the calibration holdout fit neither the trees nor the evaluation.

The **gap** between the two is the overfitting signal, and it is only readable
when both are present — which is why `has_held_out` exists rather than a zeroed
`held_out`. A zero AUC reads as a model that ranks backwards; absent and zero must
not look alike.

## How rows are chosen

By `row_is_holdout` — an FNV-1a content hash of the row, the same function
calibration has always used. Two properties follow:

- **Sharding-invariant.** Which rows are held out does not depend on how the rows
  were spread over nodes, so a run's headline metric does not change when the
  cluster resizes. `GbdtHoldout.HoldoutIsShardingInvariant` pins this.
- **Disjoint from the calibration holdout.** Evaluation is decided first and wins;
  calibration then draws from what is left. A row that fit the calibrator has
  influenced the model's probabilities and is therefore not an unseen row.

The hash **input** is unchanged from what calibration used. Perturbing it — an
early attempt added `1.0` to the label — changes *which* rows calibrate, and so
the calibrated probabilities, for every run that never asked for an evaluation
holdout.

## How it travels the cluster

Both sets of (probability, label) pairs ride the **one existing** metrics gather,
length-prefixed: the first element of each node's payload is the count of
in-sample doubles, and everything after that offset is held-out.

The obvious alternative — a second gather/broadcast guarded by
`holdout_fraction > 0` — is a hang waiting to happen. A node that disagreed about
whether a holdout was asked for would sit in a collective its peers never enter,
and the run would block to the timeout with no error naming the cause. One
unconditional collective cannot desync, whatever any node believes about the
parameter. `holdout_fraction` still round-trips through `to_json`/`from_json`
regardless, because the parameter has to survive the launch payload.

The broadcast carries nine scalars: the three in-sample metrics, a measured flag,
the three held-out metrics, and the two row counts.

## What a reader sees

The Model's `metrics` keeps its legacy top-level `auc` / `log_loss` / `accuracy`
— every existing model record has them, and they always meant in-sample.
Alongside them go prefixed keys that **name** what each figure was measured on:

```jsonc
{ "auc": 0.77, "log_loss": 0.41, "accuracy": 0.82,        // legacy: in-sample
  "in_sample_auc": 0.77, "in_sample_log_loss": 0.41,
  "in_sample_accuracy": 0.82, "in_sample_rows": 10500,
  "held_out_auc": 0.73, "held_out_log_loss": 0.44,
  "held_out_accuracy": 0.81, "held_out_rows": 4500 }
```

**Which figure to quote is answered by presence.** When `held_out_auc` is there,
that is the one. When it is absent no holdout was asked for, and the only numbers
available are the flattering ones.

### Every value must be a number

`model_schema()` declares `metrics` as `additionalProperties: {"type": "number"}`.
A nested object or a string flag is rejected — and rejected at the **receiving**
peer. The root registers the model, replication carries it, and it then fails to
materialize anywhere else. The symptom is *"the model does not resolve on every
member"*, raised by a cluster test a long way from the code that wrote the value,
while every single-node test stays green.

An earlier draft of this change put `in_sample` and `held_out` in as nested
objects with a `headline_metric` string. It passed the whole single-node ML suite
and failed 7 runs out of 7 on `Testkit.DistributedCasesRunOnARealCluster`.
`GbdtHoldout.RegisteredModelMetricsSurviveTheEntityBoundary` now catches that
class in two milliseconds by parsing the registered model back through its own
boundary schema.

## Control surface

`run` takes `holdout_fraction` (0–1). Without it the run's only metrics are
in-sample. `GbdtParams`' own schema carries the field too, so any caller that
supplies params directly can set it.

## Boundaries

No new JSON format. `gbdt_params.schema.json` gains one optional number;
the collective wire grows from three scalars to nine.

## Later slices

- Emitting the run's **facts artifact** (both figures, both row counts) as a
  `RunOutput` of kind `report` — [#958](https://github.com/oramasearch/zetta/issues/958).
- A per-row explanation beside the held-out score.
