# Learning-to-rank (M-9)

M-9 trains a native LambdaMART model for grouped queues: leads owned by one
representative, work items in one queue, or results for one query. It optimizes
NDCG at the operating cutoff directly and writes a table that downstream SQL,
Studio, and agents can consume.

The implementation is C++26 and uses the existing histogram GBDT tree builder.
LambdaMART supplies one gradient and Hessian per row; bin statistics and tree
statistics are combined across participants with `JobRound::allreduce`. It has
no Python process, foreign ML runtime, hosted model, or third-party trainer.

## Inputs

A partitioned `database.table` supplies:

- an integer entity column identifying the item being ranked;
- a required integer group column identifying the indivisible queue or query;
- a finite, non-negative graded relevance label; and
- one or more numeric features known when ranking happens.

The group, entity, label, and optional time column are never features. A group
must be owned by exactly one participant. The distributed job checks ownership
before training and refuses a table where the same group appears on multiple
participants.

## Objective and split

Within each group, LambdaMART compares rows with different relevance. Each
pair's weight is the absolute change in NDCG@k if their current positions were
swapped. Logistic pair derivatives become the GBDT gradient and Hessian; the
shared histogram trainer then grows the tree using global reductions.

The seed and group identifier deterministically assign whole groups to training
or validation. No row-level split exists in this methodology. Re-running the
same data and parameters therefore selects the same held-out groups and builds
the same model, apart from insignificant floating-point reduction ordering
across different participant counts.

## Outputs

The runner registers a `rank` model and writes
`rank_results.ranked_<graph-prefix>`. Every row contains:

| Column | Meaning |
|---|---|
| `group` | queue/query identifier |
| `entity` | item identifier |
| `score` | LambdaMART raw score; larger ranks first within a group |
| `rank` | one-based position within the group |
| `relevance` | observed graded label used for evaluation |

The Run records the input table, model, ranked-queue table, report, graph steps,
and dataset lineage.

## Metrics

The registered model and `get_run_report` expose metrics measured only on held
out groups:

- `ndcg_at_k`: mean normalized discounted cumulative gain at the configured
  cutoff; and
- `yield_at_k`: mean sum of relevance among the first k rows of each group, the
  value captured at the operating budget.

The report also names `cutoff`, training and held-out row counts, training and
held-out group counts, the input table, excluded/inference notes when present,
and ranked-queue rows.

## Correctness rules

The `rank_correctness` graph check blocks a run unless the model step has:

- a non-empty `group_column`;
- `split_strategy: group`, which keeps every group on one side of the split;
  and
- `feature_availability: ranking_time`, which declares that no after-outcome
  information is used.

Runtime validation additionally requires aligned columns, non-negative finite
labels, integer group/entity values, at least one feature, both training and
validation groups, and one-participant ownership per group.

## Running it

Compile M-9 explicitly or through solution S-12. Dataset hints become the
connect and model configuration:

```json
{
  "project": "lead-priority",
  "methodology": "M-9",
  "hints": {
    "source": "sales",
    "table": "leads",
    "entity": "lead_id",
    "group": "rep_id",
    "label": "relevance",
    "features": ["intent", "fit", "value"],
    "cutoff": 5
  }
}
```

Run the compiled graph without `dataset_path` to read its own connect step, or
provide an explicit table path to override it. Poll `get_run`, then call
`get_run_report` with the returned run id.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Rank*:*Lambda*:*NDCG*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
./build/tests/zetta-tests
```

The focused suite proves whole-group splits, deterministic learning,
improvement over a pointwise GBDT baseline, distributed equivalence, graph
refusals, and the registry-driven API journey.
