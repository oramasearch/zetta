# Label efficiency (M-19)

M-19 combines active learning and weak supervision in native C++26. It ranks
rows for human review, learns probabilistic labels from declared rules, and
writes both results as queryable tables. The implementation is deterministic
under `seed` and uses no Python, hosted service, or foreign ML runtime.

The public implementation is split into:

- `include/zetta/ml/label_efficiency.hpp` and
  `src/core/ml/label_efficiency.cpp`: the distributed trainer;
- `include/zetta/ml/label_efficiency_runner.hpp` and
  `src/core/ml/label_efficiency_runner.cpp`: the resolved-table runner and
  output writer.

## Inputs

The runner takes a `zetta::data::ResolvedDataset`. Its contract must identify:

- an integer entity column;
- one or more numeric feature columns; and
- a binary reference label column.

The reference labels support offline evaluation and simulate labels returned by
reviewers as rows are acquired. A seeded entity hash reserves `gold_fraction`
of rows as the small gold evaluation set. The remaining labels are hidden from
the weak label model and are revealed to the active learner only when it selects
that row. The split depends only on entity, seed, and fraction, so repartitioning
does not change membership.

Labelling functions are declared threshold rules over feature columns:

```json
{
  "name": "large_balance",
  "column": "balance",
  "operator": "greater_equal",
  "value": 1000,
  "label": 1,
  "otherwise_label": 0
}
```

Operators are `less`, `less_equal`, `greater`, `greater_equal`, and `equal`.
Omitting `otherwise_label` makes a non-match an abstention.

## Active learning

The learner starts with `initial_labels` deterministic random acquisitions. It
fits a native logistic model, then fills a queue of exactly `queue_size` while
retraining after each acquisition. Candidate score is a weighted combination
of:

- margin or normalized entropy uncertainty; and
- k-center distance from already selected rows in normalized feature space.

The report compares the resulting gold-set accuracy against a seeded random
sampler at the same labelling budget. It also reports how many labels the active
curve saved before reaching its final accuracy, when the random curve reaches
that accuracy at all.

## Weak supervision

Each labelling function emits a noisy binary vote or abstains. The native label
model uses an independence assumption and expectation-maximization to estimate
class prevalence and each function's accuracy. It writes a probability for
every non-gold row that received at least one vote, together with the names of
the contributing functions.

The label-model metric is accuracy against only the reserved gold rows. Gold
labels are not used to estimate labelling-function accuracy.

## Outputs and metrics

Every successful run creates two tables in `label_efficiency_results`:

- `label_queue_<run>`: `rank`, `row`, `uncertainty`, `diversity`, and `score`;
- `weak_labels_<run>`: `row`, `probabilistic_label`, and
  `contributing_functions`.

The registered `label_efficiency` model records estimated function accuracies
and these numeric metrics:

- `active_accuracy_at_budget`;
- `random_accuracy_at_budget`;
- `accuracy_gain_at_budget`;
- `labels_saved` and `labelling_budget`;
- `label_model_gold_accuracy`; and
- `gold_rows` and `unlabelled_rows`.

`get_run_report` returns those metrics, the source table, the number of excluded
gold rows, and both output table references.

## Correctness rules

Two rules are enforced by the trainer and output writer:

1. weak labels are never treated as gold; and
2. the gold set is disjoint from the label queue and weak-label table.

The seeded membership rule is shared with the output tests, which assert that no
emitted row belongs to gold. The registered model records `gold_split:
"disjoint"` and `weak_labels_are_gold: false` with its feature metadata.

## Distribution and current boundary

Training is SPMD over owned row shards. Normalization statistics, logistic
gradients, evaluation counts, label-model sufficient statistics, and gold
accuracy are combined with `JobRound::allreduce`. Each acquisition gathers one
bounded candidate per participant and broadcasts the selected row. Weak labels
remain on the participant that owns the row.

The table runner currently refuses a multi-member roster. The trainer is
distributed and covered by a two-member partition test, but publishing all
local weak-label shards as one durable table needs a coordinated table writer.
Refusal prevents a local shard from being reported as the global result.

## Running it

Resolve a connector-landed table with `zetta::data::resolve_dataset`, populate
`LabelEfficiencyRunOptions::params`, then call
`run_label_efficiency_on_dataset`. The asynchronous wrapper is available for an
interactive owner that needs to poll the resulting `Run` while work continues.

## Weights and licences

M-19 uses no pretrained checkpoint, tokenizer, vocabulary, or model config.
There are no external weights or weight licences to record, and tests perform no
network downloads.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Active*:*Weak*:*LabelModel*'
./build/tests/zetta-tests
```

The planted active-learning test requires the uncertainty-plus-diversity queue
to reach the target accuracy with fewer labels than random sampling. The
synthetic weak-supervision test requires the label model to recover planted
function accuracies and checks the gold/output disjointness rule.
