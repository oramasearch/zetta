# Online and streaming learning

**Methodology:** M-21 · **model kind:** `streaming` · **Issue:**
[#1060](https://github.com/oramasearch/zetta/issues/1060)

M-21 makes a prediction for each event, records that prediction for evaluation,
and only then updates a native online logistic-regression model when the label is
available. It uses Zetta pub/sub topics for input and scores, maintains
event-time window features per entity, periodically persists the model, and
writes a queryable score table and run report.

The implementation is C++26 with no third-party ML runtime, embedded language,
or hosted model API. It uses no pretrained weights, tokenizer, vocabulary, or
external checkpoint, so there is no weights licence or checkpoint revision to
record.

## Input contract

The runner resolves the connect step's `source` and `table` through
`zetta::data::resolve_dataset`. Every row is an event with:

| Role | Requirement |
|---|---|
| entity | required integer key; windows are independent per entity |
| event time | required numeric column named explicitly in the graph |
| features | one or more required numeric event columns |
| label | optional binary `0` or `1` outcome |
| label time | optional numeric availability time; defaults to event time |

Columns outside that contract are excluded and named in the run report. A label
is useful for learning only when its label time is at or after its event time.

## Event-time correctness

The runner tracks the greatest event time observed on each owned partition. Its
watermark is `maximum observed event time - allowed_lateness`. An event whose
time is behind the prior watermark is excluded and counted as
`late_events_excluded`; an event on the watermark is accepted. Accepted work is
released in event-time order across participants.

For each accepted event, the runner maintains a deque for that entity covering
`window_duration`. The model features are the current window count and the sum
of each event feature in the window. The event is scored before any label at
that action time updates the model.

The following rules are enforced by both the graph gate and runtime behavior:

- `event_time_ordered` is true and future work is buffered until its watermark;
- `allowed_lateness` is present and non-negative;
- labels with `label_time < event_time` are excluded, never used early;
- evaluation is prequential: test first, then train on the released label; and
- calibration is not part of this time-ordered methodology.

## Model and distribution

The model is binary logistic regression trained by stochastic gradient descent.
Defaults are a `0.05` learning rate and `0.001` L2 regularization. Initial
weights are tiny values generated deterministically from `seed`.

Each participant owns its local event rows and entity windows. Participants
gather the set of action times, allreduce the gradient partials at each time,
and gather metric partials. Every member therefore reaches identical weights
without moving source rows. `persist_every` controls how many released labels
trigger a weights write; the final model is always persisted.

`run_streaming_train_job` is the owned-partition SPMD entry. The registry's
bounded table replay runs once on the submitting member after the cluster query
materializes the resolved table; score publication and artifact writes remain
single-owner operations.

The optional Hoeffding-tree variant is not implemented. Online logistic
regression is the runnable M-21 algorithm.

## Outputs and report

A successful run produces:

- a registered `streaming` Model with native JSON weights and parameter facts;
- one JSON score event per accepted input on the exposed output topic;
- `streaming_results.scores_<run-prefix>`, with event id, entity, event time,
  probability, and model version;
- lineage naming the resolved table, input topic, and streaming operations; and
- a `get_run_report` artifact with the read source, excluded columns, topics,
  output table, and numeric metrics.

Metrics cover the most recent `metric_window` released labels:

- `prequential_log_loss` and `prequential_accuracy` use predictions made before
  their labels trained the model;
- `drift_loss_delta` is newer-half mean loss minus older-half mean loss;
- `evaluated_labels`, `events_read`, and `events_scored` are global counts;
- `late_events_excluded` and `early_labels_excluded` state what was refused; and
- `watermark` reports the minimum finite participant watermark, the point every
  participant has passed.

## Running it

Compile M-21 with the table and column contract in the graph:

```json
{
  "project": "fraud",
  "methodology": "M-21",
  "name": "stream decisions",
  "hints": {
    "source": "transactions",
    "table": "events",
    "entity_column": "account_id",
    "time_column": "event_time",
    "label_column": "fraud",
    "label_time_column": "label_time",
    "feature_columns": ["amount", "velocity"],
    "input_topic": "transactions.events",
    "output_topic": "transactions.scores",
    "allowed_lateness": 5.0
  }
}
```

Approve and gate the graph, then call `run` with its project and graph. The
optional `streaming` object on `run` overrides learning rate, L2, window length,
allowed lateness, metric window, persistence interval, seed, label-time column,
and topic names. Poll `get_run` and then read `get_run_report`; consume score
events with `receive_events` on the output topic.

One run is a bounded replay of its resolved table: it publishes those rows to
the input topic, consumes them through a subscription, writes its artifacts,
and terminates. Long-lived topic consumers and incremental restart offsets are
outside this change.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Stream*:*Online*:*Prequential*'
./build/tests/zetta-tests --gtest_filter='*Methodology*:*GraphCheck*:ActionJourneys.*'
./build/tests/zetta-tests
```

The focused tests cover convergence, deterministic replay, label timing,
prequential metric order, allowed lateness, periodic persistence, multi-member
gradient allreduce, score publication/readback, table lineage, registry compile,
gate refusal, and the API journey.
