# Causal inference and uplift (M-6)

M-6 answers “who should receive treatment?” rather than “who will have the
outcome?”. It is a native C++26 implementation over the existing distributed
histogram GBDT stack. It uses no Python process, foreign ML runtime, or hosted
model API.

## Inputs

The resolved table must contain:

- an integer entity key;
- a binary treatment column in `{0,1}`;
- a binary observed-outcome column in `{0,1}`; and
- one or more numeric features measured before treatment.

The treatment and outcome roles are explicit. Neither is admitted as a feature.
When feature timestamps are present, a feature dated after treatment is refused.
An outcome-named feature, an outcome-derived feature, or an explicitly flagged
target leak is also refused.

## Learners and selection

The T-learner fits two binary-logistic GBDTs: one outcome model for treated rows
and one for control rows. Its conditional average treatment effect is the
difference between the two predicted outcome probabilities.

The X-learner reuses those outcome models to impute treatment effects, fits one
squared-error GBDT per arm, then combines their predictions with the fitted
propensity. This is useful when treatment arms are unbalanced.

Both learners are evaluated on a deterministic held-out split stratified by
treatment arm. The runner selects the higher held-out Qini coefficient, using
AUUC as the tie-breaker, and reports the metrics for both learners. A seed fixes
the split, tie order, and bootstrap resampling.

## Distribution

Each of the five component GBDTs—propensity, two outcome models, and two effect
models—builds statistics over the member’s owned partitions. `JobRound`
allreduces bin edges, label totals, and gradient histograms. Held-out scored rows
are gathered once for Qini evaluation and the targeting policy, then the global
result is broadcast. The raw training rows stay with their partition owners.

`run_uplift_on_table` is the public resolved-dataset entry point. It accepts a
`zetta::data::ResolvedDataset` and `UpliftRunOptions`, records a train job, and
writes the model and output tables with run lineage. The registry runner
currently refuses a multi-member table launch because the cross-member launch
message for M-6 is not implemented; direct distributed jobs over owned
partitions are supported and tested.

## Positivity and causal claims

A native GBDT classifier estimates `P(treatment=1 | features)`. Profiles with an
estimated propensity outside `[0.05,0.95]` are flagged, counted, and never
recommended. Their treatment-effect rows remain in the output so the exclusion
is inspectable. If no held-out profile is overlap-eligible, held-out metrics are
zero. If no profile at all is overlap-eligible, no profile is recommended.

When fitted propensities are far from constant, the run is marked
observational. Its report states: “causal claims assume no unmeasured
confounding.” This is an assumption, not a claim the data can verify.

## Outputs and report

Each successful run creates:

- an `uplift` model containing all five serialized GBDTs;
- `uplift_results.effects_<run>` with rank, entity, estimated uplift,
  propensity, observed arm and outcome, overlap eligibility, recommendation,
  and targeting fraction;
- `uplift_results.qini_<run>` with targeting fraction, incremental gain, and
  average uplift; and
- a report output resolved by `get_run_report`.

The report includes T- and X-learner AUUC, Qini coefficient, and uplift at the
targeting fraction; selected learner; a deterministic 95% bootstrap interval;
treated, control, and held-out arm sizes; propensity minimum, maximum, mean, and
standard deviation; overlap exclusions; output rows; dataset source and notes;
and the observational assumption when it applies.

## Running it

Compile an M-6 graph with table and role hints, approve it, then run it. If
`dataset_path` is omitted, M-6 reads the connect step’s `source.table`.

```json
{
  "project": "targeting",
  "methodology": "M-6",
  "hints": {
    "source": "experiments",
    "table": "campaign_population",
    "entity": "customer_id",
    "treatment": "received_offer",
    "outcome": "converted",
    "features": ["tenure", "prior_orders"]
  }
}
```

The `run` capability also accepts `targeting_fraction`, `holdout_fraction`,
`bootstrap_samples`, `seed`, `treatment_column`, and `outcome_column`.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Uplift*:*Causal*:*Qini*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:*GraphCheck*:ActionJourneys.*:Uc1Journey.*'
```

The tests cover planted heterogeneous effects, Qini above random, binary-role
refusals, post-treatment and outcome-like feature refusals, overlap exclusion,
determinism, owned-partition distribution versus a single-node oracle, registry
compilation, table selection from the graph, output materialization, and report
read-back.

M-6 is selected by methodology ID or the causal/uplift model kinds. S-11 belongs
to M-8 as the catalogue's primary next-best-action methodology; uplift is only
a supporting component there (#1082).
