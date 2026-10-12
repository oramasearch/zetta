# M-20 synthetic data and differentially private GBDT

M-20 implements privacy-preserving learning and synthetic tabular data for
[#1059](https://github.com/oramasearch/zetta/issues/1059), in the methodology
portfolio tracked by [#984](https://github.com/oramasearch/zetta/issues/984).
It is native C++26: no Python process, foreign ML runtime, hosted model, or
pretrained checkpoint is used. Weight, tokenizer, vocabulary, and checkpoint
licences therefore do not apply.

Registry runs enforce the [feature plan](../data/featurization.md) before creating
a Run, launching distributed training, or publishing any table. Reports record
the native empirical-marginal/copula sources and configured DP clipping bounds;
entity and utility label are excluded as predictors. Transform-first data and
selected leakage return `invalid_input`. Privacy accounting, output disclosure
checks, and the native trainer are unchanged.

The public model kind is `synthetic_private`. One run produces:

- an evaluated synthetic table containing the feature columns and label, but
  never the source entity column;
- a `Model` of kind `synthetic_private` whose weights are the native GBDT JSON;
  and
- a report with fidelity, privacy-evaluation, utility, and per-run privacy
  accounting metrics.

## Inputs

The connect step must identify a landed `database.table`. Dataset resolution
uses the same `ResolvedDataset` contract as table-backed churn runs. The
contract requires a binary label and at least one numeric, Boolean, or scalar
categorical feature. An entity column is used for dataset identity but is
excluded from synthesis. An optional group role describes a data boundary;
it is discovery metadata and is not a model feature unless it also appears in
the resolved feature list.

The compile-time model step states these correctness inputs:

| Attribute | Meaning | Default |
|---|---|---|
| `epsilon`, `delta` | requested total DP budget for the GBDT release | `4`, `1e-6` |
| `maximum_epsilon`, `maximum_delta` | maximum budget this graph permits | `8`, `1e-5` |
| `noise` | `gaussian` or `laplace` histogram mechanism | `gaussian` |
| `clip_min`, `clip_max` | public feature bounds used for sensitivity | `0`, `1` |
| `privacy_evaluation` | requires DCR and membership testing before publication | `true` |

`run` can choose a budget no larger than the graph maximum and can set
`synthetic_rows`, `seed`, `num_trees`, and `max_depth`. A zero synthetic row
count in the native API means the global source row count. A seeded run is
deterministic for the same rows, contract, parameters, and cluster membership.

## Gaussian copula

The native copula fits empirical marginals and dependence without retaining a
central copy of the source table:

1. Numeric columns use global minimum/maximum and a 64-bin empirical
   distribution. Sampling interpolates within the selected quantile bin.
2. Scalar categorical values are serialized to stable JSON values, sorted,
   and assigned ordered bins. Sampling decodes the selected value back to its
   original JSON type.
3. Each row is transformed to normal scores from its marginal bin probability.
   Per-participant first and second moments are summed to obtain the global
   normal-score correlation matrix.
4. A Cholesky factor correlates seeded standard-normal draws; the empirical
   inverse marginals turn those draws into synthetic rows.

Each participant reads only partition bodies it owns. Category dictionaries,
marginal counts, normal-score moments, evaluation values, and GBDT histograms
travel through `JobRound` collectives. No source row is broadcast. The
temporary TSTR model is fit independently on the identical synthetic table at
each participant; participants then score their local real rows and combine
only score/label pairs for the global utility AUC.

The synthetic generator itself is not a differential-privacy mechanism. Its
output is called an **evaluated synthetic table**, never a private table. The
evaluation gate below is an empirical disclosure-risk check, not a formal DP
guarantee.

## Privacy evaluation and utility

Publication requires both empirical checks to pass:

- **Distance to closest record (DCR):** the minimum mixed-type distance from
  any generated row to any source row. Numeric differences are normalized by
  their observed range; categorical differences are 0 or 1. The default
  threshold is strictly greater than `1e-9`, which also rejects an exact copy.
- **Membership AUC:** rows are split deterministically into copula members and
  held-out non-members. A second synthetic sample is fit only on members. The
  attack score is negative DCR to that sample. Tied ranks receive their average
  rank and the reported AUC is oriented to the stronger of an attack and its
  inverse, so it lies in `[0.5, 1]`. The default maximum is `0.65`; `0.5` is
  chance.

If either check fails, the runner clears the generated rows, fails the run,
and writes neither output table nor model.

Utility is **train on synthetic, test on real (TSTR)**. A native GBDT is trained
on the synthetic rows and scored on the real rows held by each participant;
the report records the global binary ROC-AUC as `tstr_auc`.

Fidelity is reported as:

- `marginal_distance`: mean total-variation distance between source and
  synthetic empirical marginals; and
- `correlation_distance`: root-mean-square element distance between source and
  synthetic normal-score correlation matrices.

Lower fidelity distances are better. DCR must be above its threshold,
membership AUC should remain near `0.5`, and higher TSTR AUC is better.

## Differentially private GBDT mechanism

The released model uses the existing binary-logistic histogram GBDT with fixed
public feature edges `[clip_min, clip_max]`. Every feature value is clipped to
that interval. For one add/remove-adjacent row, the logistic gradient satisfies
`|g| <= 1` and the Hessian satisfies `0 <= h <= 0.25`.

Let `F` be the feature count and
`R = 1 + num_trees * (max_depth + 1)` be the conservative upper bound on
released vectors: one base-rate vector plus one histogram vector per possible
tree level. The per-vector allocation is
`epsilon_i = min(epsilon / R, 1)`. Gaussian runs allocate
`delta_i = delta / R`.

- The base `[sum(label), count]` has add/remove L2 sensitivity `sqrt(2)` and
  L1 sensitivity `2`.
- A row contributes one `(gradient, Hessian)` pair per feature to one open
  node, so a histogram vector has L2 sensitivity
  `sqrt(F * (1 + 0.25^2))` and L1 sensitivity `1.25 * F`.
- Gaussian noise uses
  `sigma = sensitivity_L2 * sqrt(2 * ln(1.25 / delta_i)) / epsilon_i`.
- Laplace noise uses
  `scale = sensitivity_L1 / epsilon_i` and spends zero delta.

Noise is added after the global histogram sum and before split selection. The
same seeded noisy result is used by all participants, so they grow the same
model. Basic sequential composition reports only the mechanisms actually
executed:
`epsilon_spent = mechanisms * epsilon_i` and, for Gaussian noise,
`delta_spent = mechanisms * delta_i`. Both are at or below the requested
per-run budget. A request above either graph maximum is refused before any
output is written.

This is central DP for the released model. Cluster participants can observe
aggregate protocol values, and the deterministic seed must be protected as
release state. Releasing another model, including a rerun with another seed,
incurs another budget and must be composed by the operator. M-20 reports each
run; it does not maintain a cross-run privacy ledger.

Run metrics (#1137) never compute or reduce per-tree loss, counts, or throughput
on private training rows. The DP entry point forces metric suppression even if
a caller installs `TrainHooks::on_metrics`: model bytes, released metrics, and
epsilon/delta spend are identical with and without that hook. The Run records
`metrics_suppressed: "differential_privacy"` and logs only the trainer's existing
final report through `get_run_metrics` and its live topic. No extra system or
fit-status points are added. Public parameters are recorded once; the protected
noise seed is excluded. See [run-metrics.md](run-metrics.md).

## Running it

Land a table, create a project and privacy/synthetic-data intent, then compile
that intent explicitly with M-20; S-10 retains its primary M-1 lookup:

```json
{
  "project": "privacy",
  "input_id": "<captured-intent-id>",
  "methodology": "M-20",
  "name": "private synthesis",
  "hints": {
    "source": "customer_data",
    "table": "training",
    "epsilon": 4.0,
    "delta": 0.000001,
    "maximum_epsilon": 8.0,
    "maximum_delta": 0.00001,
    "noise": "gaussian",
    "clip_min": 0.0,
    "clip_max": 1.0
  }
}
```

The compiled graph is M-20's own
`connect -> clean -> model(synthetic_private) -> serve` pipeline. Run it with
the graph's table or an explicit `dataset_path`:

```json
{
  "project": "privacy",
  "graph": "private synthesis",
  "synthetic_rows": 1000,
  "seed": 1059,
  "num_trees": 12,
  "max_depth": 2
}
```

`get_run` returns the terminal run and its three output references.
`get_run_report` returns the source table and excluded-column notes, output
references, and all metrics. The synthetic table is stored under
`synthetic_private_results`; the model is registered and carries the run id,
feature ordering, metrics, and GBDT weights. Run lineage records the source
dataset and every pipeline step.

## Correctness rules

The graph gate and runner enforce these rules:

1. Epsilon, delta, maximum budget, mechanism, and public clipping bounds must be
   explicit and valid. Gaussian noise requires `0 < delta < 1`.
2. The requested run budget must not exceed the graph maximum, and reported
   spend must not exceed the request.
3. Synthetic publication requires `privacy_evaluation: true`, positive DCR,
   and membership AUC at or below the configured maximum.
4. A label and at least one feature must resolve from a landed table. The
   source entity identifier is never copied to output.
5. The same seed is reproducible. Every participant executes collective rounds
   in the same order and trains the DP model over only its owned source rows.

## Verification

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Synthetic*:*Copula*:*Privacy*:*DifferentialPriv*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
./build/tests/zetta-tests
```

The focused tests cover marginal/correlation preservation, categorical values,
exact-copy refusal, membership evaluation, budget refusal, epsilon-dependent
model utility, multi-member owned-partition training, graph checks, registry
compile/run dispatch, output tables, lineage, and run-report readback.
