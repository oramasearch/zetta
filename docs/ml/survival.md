# Survival / time-to-event

Implemented for [#1044](https://github.com/oramasearch/zetta/issues/1044), M-5
in the [methodology portfolio](methodologies.md).

Survival analysis answers when an event is likely, including subjects whose
event has not yet been observed. The native trainer uses discrete-time hazards
on the existing histogram GBDT. It expands each subject into one row per stated
interval, through the interval containing its event or censoring time. Only an
observed event labels the final row as `1`; every earlier row and every row from
a censored subject is `0`. No row is created after an observed event.

The GBDT learns the conditional hazard `h_k`. Prediction converts the hazards
to a non-increasing survival curve:

`S(t_k) = product(j <= k, 1 - h_j)`

The restricted expected time is the area under that step curve through the
configured horizon. Median survival is the first horizon where survival is at
most 0.5, or the configured final horizon when the curve does not cross 0.5.

## Inputs

The table contract requires:

- an integer entity id;
- a numeric subject start time;
- a positive numeric duration from start to event or censoring;
- a binary event indicator (`1` observed event, `0` censored);
- a numeric feature timestamp; and
- one or more numeric features measured at that timestamp.

The runner takes a `zetta::data::ResolvedDataset`. The dataset contract supplies
the entity, event-indicator, start-time, and feature columns. Runner parameters
name the duration and feature-timestamp columns. A feature timestamp later than
the subject's start time is refused before training.

`SurvivalParams` states the interval width, maximum interval count, newest-time
validation fraction, deterministic seed, and the underlying `GbdtParams`.

## Distributed training

Each member reads only the partition bodies it owns with
`ddf::load_local_partitions`. Person-period expansion happens on that local
subject shard. The existing GBDT combines global bin ranges, label counts, and
gradient histograms with `JobRound::allreduce`, producing identical trees on all
members. Prediction summaries are gathered for censoring-aware evaluation and
the global curves are broadcast back. Raw input rows do not move between
members.

The public `train_survival` function accepts injected collectives for the same
single-node and multi-node algorithm. `run_survival_on_dataset` uses the node's
live `JobCoordinator`, registers a `survival` model, writes result tables, and
records run lineage.

## Outputs

The survival table contains one row per entity and horizon:

- `entity`
- `horizon`
- `hazard`
- `survival`
- `expected_time`
- `median_survival`

The interval-calibration table contains each interval's predicted mean hazard,
observed event rate, and at-risk count. The registered model carries the GBDT
weights, feature drivers, and numeric metrics. The run points to the model,
both tables, and its report artifact, with the resolved dataset as its lineage
source.

## Metrics

- Censoring-aware concordance compares an observed earlier event only with a
  subject known to remain event-free longer. Ties receive half credit.
- IPCW integrated Brier score evaluates survival probability at every horizon,
  weighting observed events and known survivors by the Kaplan-Meier estimate of
  the censoring distribution.
- Calibration by interval compares mean predicted hazard with the observed
  event rate among subjects at risk at the interval start.

All headline metrics use the newest subjects held out by start time. The split
is never a random row split, and person-period rows from one subject cannot land
on both sides.

## Correctness rules

- Censoring is never labeled as an event.
- Expansion stops at an observed event and creates no later row.
- A feature timestamp after the subject start is refused.
- Validation holds out the newest subjects by start time.
- Survival curves are non-increasing because each step multiplies by
  `1 - hazard`, where hazard is a probability.

The runner enforces these rules on data. `SurvivalCensoringCheck`,
`TemporalValidationCheck`, and `DataLeakageCheck` enforce the graph declarations
before a run starts.

## Running it

Land the cohort as a partitioned table, then compile M-5 and run the stored
graph. Unless `dataset_path` is supplied explicitly, M-5 reads the database and
table named by its connect step.

```jsonc
// compile_intent
{"project":"maintenance", "methodology":"M-5",
 "hints":{"source":"datasets", "table":"survival_subjects"}}

// run
{"project":"maintenance", "graph":"survival graph",
 "dataset":{"entity_column":"entity", "label_column":"event",
            "time_column":"start_time", "as_of":1700000000,
            "feature_columns":["temperature","load"]},
 "duration_column":"duration", "feature_time_column":"feature_time",
 "interval_width":30, "max_intervals":12, "validation_fraction":0.2}
```

Poll `get_run`, then call `get_run_report`. The report returns censoring-aware
concordance, IPCW integrated Brier score, the source table, inferred/excluded
dataset facts, and references for the survival and interval-calibration tables.

## Verify

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Survival*:*Hazard*:*Concordance*'
```

The focused tests prove censor labels, monotone curves, ranking from a known
feature-driven hazard, deterministic output, and equality between a single-node
run and subject shards trained across multiple members.

## Boundaries

No foreign runtime or hosted model is used. The trainer reuses the validated
GBDT weights boundary, model/run entities, dataset contract, partition bodies,
and collective messages; it adds no new JSON boundary. Event-time plus censor-
time input can be normalized to duration plus event indicator before this API.
Cox proportional-hazard ratios are outside this discrete-time methodology.
