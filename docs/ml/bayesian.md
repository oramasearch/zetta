# Bayesian uncertainty

Implemented for [#1056](https://github.com/oramasearch/zetta/issues/1056),
methodology M-17.

This slice provides native uncertainty estimates for three cases:

- split-conformal intervals around numeric point predictions;
- split-conformal prediction sets around class probabilities; and
- partial-pooling posterior estimates for grouped numeric observations.

All three implementations are C++26. They use no external ML runtime, hosted
model, pretrained checkpoint, or downloaded weights.

## Inputs and outputs

`split_conformal_regression` accepts observed values, point predictions, an
explicit `UncertaintySplit`, and nominal coverage. It returns one interval per
point prediction, the finite-sample residual quantile, empirical hold-out
coverage, and mean width.

`split_conformal_classification` accepts integer class labels and one class
probability vector per row. It returns a set of class ids per row, empirical
hold-out coverage, and mean set size.

`fit_hierarchical_means` accepts `{key, group, value}` observations. The model
is a normal likelihood with group means drawn from a shared normal population.
Native Gibbs sampling produces each group's posterior mean and credible
interval. A posterior predictive interval is evaluated against rows excluded
from fitting. Small groups borrow more information from the population than
large groups.

`run_bayesian` takes a resolved `database.table` through the
`ResolvedDataset` contract. It writes:

- a `bayesian` Model with the sampled posterior as its weights body;
- `bayesian_results.posterior_<run>` with per-group posterior means, credible
  intervals, predictive intervals, sample means, and fitting row counts; and
- a Run whose lineage names the source table and whose numeric metrics include
  nominal coverage, empirical hold-out coverage, mean interval width, fitting
  rows, hold-out rows, excluded rows, and group count.

## Distributed execution

`run_hierarchical_job` is the SPMD entry point. Each member computes count,
sum, and sum-of-squares statistics over its local rows. `JobRound::allreduce`
combines those vectors. Every member then samples from the same global
posterior under the same seed, so all members produce identical per-group
estimates. Hold-out coverage totals are also allreduced.

The table runner currently reads through `QueryService`; partition-only tables
therefore use the existing distributed select path when a cluster is supplied.
The statistics job remains available independently for compute-near-data
callers that already hold owned partition rows.

## Correctness rules

- Fitting, calibration, and coverage measurement use disjoint row indexes.
  `UncertaintySplit::valid` rejects overlap, out-of-range indexes, or an empty
  subset.
- Conformal quantiles are learned only from calibration rows. Coverage is
  measured only on hold-out rows and is reported beside nominal coverage.
- Hierarchical parameters are fit only on fitting rows. Posterior predictive
  coverage is measured only on content-hash hold-out rows.
- The split and Gibbs chain are deterministic under `seed`.
- Conformal coverage requires exchangeable observations. The hierarchical
  hold-out diagnostic uses the same assumption for interpreting its measured
  coverage. The run records this assumption; it is never presented as an
  unconditional guarantee.
- Rows with missing or invalid entity, group, or value cells are excluded and
  counted. A run with no measurable hold-out fails.

## Metrics

The report fields are:

- `nominal_coverage`: requested probability mass;
- `empirical_coverage`: fraction of hold-out observations inside the reported
  interval or prediction set;
- `mean_interval_width`: mean posterior predictive width for grouped means;
- `train_rows`, `held_out_rows`, and `excluded_rows`; and
- `groups`.

Classification also exposes mean prediction-set size through its direct API.

## Verification

Run:

```sh
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Conformal*:*Bayes*:*Coverage*:*Hierarch*'
```

The tests check synthetic conformal coverage, reject overlapping calibration
and fitting rows, show that a small group shrinks toward the population more
than a large group, and compare the posterior produced across two real cluster
members.

## Boundaries

The core engine adds no new stored-data schema. Model weights use the existing
verified weights body, metrics use the existing numeric Model map, table rows
use the existing Table storage boundary, and distributed statistics use
`CollectiveMessage`. The registry adapter extends the generated OpenAPI schema
with M-17 run controls and report fields.

Registry compilation and the control-surface run adapter are documented in the
methodology registry page.

## Running it

Land a table with integer entity ids, a numeric observation, and a group column.
Compile M-17 with hints that bind those roles:

```json
{
  "project": "demand uncertainty",
  "methodology": "M-17",
  "hints": {
    "database": "datasets",
    "table": "store_demand",
    "entity_column": "id",
    "label_column": "demand",
    "group_column": "store",
    "nominal_coverage": 0.9
  }
}
```

The resulting pipeline reads `datasets.store_demand` directly. Call `run` with
the project and graph; `dataset_path` is optional because the connect step owns
the source. Optional M-17 controls are `group_column`, `nominal_coverage`,
`holdout_fraction`, `seed`, `burn_in`, and `posterior_draws`.

The run completes with a `bayesian` Model and a
`bayesian_results.posterior_<run>` table. `get_run_report` returns the source,
excluded-row count, output-table ref, empirical versus nominal coverage, mean
interval width, hold-out row count, and the exchangeability assumption.
