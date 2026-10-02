# Run metrics

Run metrics attach experiment-tracking data to the existing durable `Run`.
They do not create a second experiment entity: `create_experiment` and
`get_experiment` remain the solutions holdout manager, while AutoML search uses
studies.

Every trainer uses the same owner-thread entry points on `Node`:
`log_run_params`, `log_run_metrics`, and `log_run_artifact`. GBDT reports every
boosting round. Native Qwen fine-tuning reports every optimizer step. Other
methodology trainers use the same calls for intermediate and final fit metrics.
The methodology dispatcher adds a step-zero `fit.started` or `fit.completed`
point when a fit has no series yet, so even a runner without an iterative
optimizer has a live, queryable series. If an asynchronous trainer has already
reported a step, the dispatcher preserves that series without a late step-zero
write. Differentially private fits are the exception:
they suppress training curves and publish only their existing final report.

## Stored shape

The replicated `Run.metrics` object contains:

- `params`: one flattened object. Dotted keys such as
  `optimizer.learning_rate` preserve nesting without open JSON objects. Values
  are scalar JSON values.
- `series`: append-only, column-oriented arrays of `steps`, `values`, and
  `times`, one set per metric name.
- `summary`: the final value and step plus the best value and step for every
  metric. Loss, error, latency, perplexity, RMSE, MAE, and MAPE names minimize;
  other metrics maximize.
- `artifacts`: typed JSON chart data for confusion matrices, ROC and PR curves,
  calibration, feature importance, and sample predictions. These are vendored
  data, not image links.

Metric values must be finite, and each metric's step must increase. Caller-
supplied steps remain strict, including explicitly supplied `system.*` values:
a repeated or decreasing step rejects the entire batch without changing any
series. Parameters can be recorded only once. The Run JSON Schema closes every
object, including the per-artifact data shape.

Automatic CPU, memory, GPU, and throughput samples use the trainer's step as
their natural step. Each automatic metric allocates
`max(last_step + 1, natural_step)` independently within its Run; its first sample
uses the natural step. Allocation reads the persisted series and commits on
the same owner thread as the metric store. Background producers enter through
`Node::run_on_owner`, so an initial fit point and a trainer sample queued at the
same natural step both succeed. Different training/validation series can also
share a natural step without colliding on the automatic gauges. No volatile
counter is needed: restart and replication retain the last allocated steps.
Step exhaustion rejects the whole batch without wrapping. This fixes the
automatic-gauge collision exercised by Study trial Runs (#1205).

DP Runs carry `metrics_suppressed: "differential_privacy"` at the top level of
`get_run` and `get_run_metrics`. This reason survives replication and restart.
M-20 records its public hyperparameters (excluding the protected noise seed)
and the trainer's existing final released metrics, including epsilon/delta
spend. It adds no training loss, row counts, throughput, system gauges, or
fit-status points. The UI can explain why there is no learning curve.

Each series keeps at most 10,000 points. Crossing the cap compacts it to 5,000
points with Largest-Triangle-Three-Buckets while forcing the first, last, global
minimum, and global maximum points into the retained set. Read-time
`max_points` uses the same extrema-preserving selection. Metrics are part of the
Run's CRDT value and disk projection, so they replicate and restart with the Run.

## Coalesced trainer captures

A trainer that logs every round passes `coalesce` to `Node::log_run_metrics`
(the GBDT train job does, [#1217](https://github.com/oramasearch/zetta/issues/1217)).
The batch is validated and applied to the in-memory Run at once — `get_run`,
`get_run_metrics` and the live topic see every point — but the Run's
replicated capture and `run.json` are written at most once per
`Run::kCaptureInterval` (one second). `Node::tick` writes a pending capture
once its interval has passed, `Node::stop` writes all of them, and any other
change to the Run (its status, outputs, lineage) captures the whole value
including the pending points. While a capture is pending, reconciliation does
not replace the Run with its older replicated value.

The CRDT keeps every captured value in its history. Capturing the whole Run on
every round made the document grow with the square of the rounds and made the
owner thread's once-a-second save of it slower all the time a trainer ran.
Callers that log occasionally, including the `log_run_metrics` capability,
still capture every batch. A Study's per-round intermediate values coalesce
the same way ([studies](studies.md#captures-while-a-trial-trains)).

## Live batches

Every successful metric write publishes JSON batches to the exposed, ephemeral
topic `run-metrics.<run>`, grouped by their actual stored step:

```json
{
  "run": "6bc2b146-9ad0-4a39-8e66-6ddcdb941221",
  "step": 12,
  "values": {"train.loss": 0.418, "train.tokens_per_second": 284.5},
  "time": "2026-09-28T18:20:11.042Z"
}
```

The batch also includes available CPU, memory, and GPU gauges when their stored
steps match. Gauges advanced past the trainer's step appear in additional
batches at their allocated steps, with the same timestamp. A trainer's row or
token rate is copied to `system.throughput.per_second`. Explicit caller values
take precedence over automatic samples. A UI or agent tails the topic through
the existing `receive_events` capability. MCP uses the same bounded long poll
with `wait_ms` and `max`; it does not hold a thread per waiter. No JSON shape or
capability schema changes are introduced by automatic step allocation.

## Capabilities

All operations are registry capabilities. The MCP tool name is identical to the
capability name. Actions require their exact orchestration operation (included
in `pipelines:manage`); reads require `api:query`.

### `log_run_params`

REST: `POST /v1/log_run_params`. MCP tool: `log_run_params`.

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","params":{"trainer":"qwen","optimizer.learning_rate":0.00001}}
```

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","params":{"trainer":"qwen","optimizer.learning_rate":0.00001}}
```

### `log_run_metrics`

REST: `POST /v1/log_run_metrics`. MCP tool: `log_run_metrics`. `time` is
optional and defaults to the node's current UTC timestamp.

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","step":12,"values":{"train.loss":0.418,"train.learning_rate":0.00001}}
```

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","step":12,"topic":"run-metrics.6bc2b146-9ad0-4a39-8e66-6ddcdb941221"}
```

### `log_run_artifact`

REST: `POST /v1/log_run_artifact`. MCP tool: `log_run_artifact`.

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","artifact":{"type":"feature_importance","name":"gain","time":"2026-09-28T18:20:15.000Z","data":{"features":["age","spend"],"values":[0.25,0.75]}}}
```

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","artifact":{"type":"feature_importance","name":"gain","time":"2026-09-28T18:20:15.000Z","data":{"features":["age","spend"],"values":[0.25,0.75]}}}
```

### `get_run_metrics`

REST: `GET /v1/get_run_metrics?run=<id>&names=["train.loss"]&from_step=10&max_points=500`.
MCP tool: `get_run_metrics`.

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","names":["train.loss"],"from_step":10,"max_points":500}
```

```json
{"run":"6bc2b146-9ad0-4a39-8e66-6ddcdb941221","params":{"trainer":"qwen"},"summary":{"train.loss":{"final":0.418,"final_step":12,"best":0.418,"best_step":12,"direction":"min"}},"artifacts":[],"series":[{"name":"train.loss","points":[{"step":12,"value":0.418,"time":"2026-09-28T18:20:11.042Z"}]}]}
```

Omit `names` for all series, `from_step` for the beginning, or `max_points` for
all stored points.

### `compare_runs`

REST: `GET /v1/compare_runs?runs=["run-a","run-b"]&names=["train.loss"]`.
MCP tool: `compare_runs`.

```json
{"runs":["run-a","run-b"],"names":["train.loss"]}
```

```json
{"runs":["run-a","run-b"],"metrics":[{"name":"train.loss","points":[{"step":12,"values":[{"run":"run-a","value":0.418,"time":"2026-09-28T18:20:11.042Z"},{"run":"run-b","value":0.451,"time":"2026-09-28T18:20:12.019Z"}]}]}]}
```

Alignment is by exact step. A missing run/step pair is absent rather than
invented or interpolated.

## Trainer contract

The GBDT job binds to the graph Run before execution, records its flattened
hyperparameters once, and logs globally reduced training loss and row throughput
after each committed tree. The native Qwen `fine_tune_qwen_step` helper logs
loss, learning rate, gradient norm, and scored-token throughput after each
optimizer update. Logging failure fails the trainer call instead of silently
claiming a fully tracked run.

Installing a metric hook must leave model bytes and privacy spend identical.
The GBDT metric pass runs only when a hook is enabled; distributed participants
agree on that choice before training. M-20 forces `TrainHooks::suppress_metrics`
even when a hook is installed, before computing or reducing anything derived
from private training rows. Additional private-data metrics would require their
own privacy mechanism and budget; this API does not release them. Tests cover
Gaussian and Laplace model-byte/spend equivalence, absence of private curves,
and unchanged non-DP training reductions with full per-tree reporting.

This work belongs to the methodology epic
[#984](https://github.com/oramasearch/zetta/issues/984) and implements
[#1137](https://github.com/oramasearch/zetta/issues/1137).
Automatic sample ordering regressions live in `tests/pipeline/test_run.cpp`:
duplicate natural steps, queued initial/trainer samples, independent Run and
metric ordering across restart, strict caller rejection, step exhaustion, and
live-event/stored-step agreement (#1205).
