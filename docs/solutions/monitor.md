# Drift and guardrail monitor

**Issue:** [#1106](https://github.com/oramasearch/zetta/issues/1106)  
**Code:** `zetta::solutions::Monitor`, `MonitorSpec`,
`Node::create_monitor` / `find_monitor`  
**State:** `monitor:<uuid>` in the replicated catalog, projected to
`<data_dir>/<uuid>/monitor.json`

Every deployed solution can own one monitor. The monitor records distribution
drift, calibration, realized cohort performance, and business or operational
guardrails without depending on the solution registry, policy engine, holdout
manager, or audit ledger.

## Declaring a monitor

`MonitorSpec` names the solution and declares:

- input histogram baselines: n expected shares and n-1 increasing boundaries;
- an optional score-distribution baseline in the same form;
- PSI and expected-calibration-error thresholds;
- calibration bin count;
- applicable guardrails, each with a minimum, maximum, or both;
- whether a new breach requests retraining.

The guardrail vocabulary is intentionally open. Solutions can declare
`complaints`, `cost`, `margin`, `fairness`, `fatigue`, and `cap_adherence`, plus
solution-specific metrics, without changing the monitor. A declaration with no
bound, non-finite values, duplicate names, or inconsistent histogram dimensions
is refused before state is created.

One solution name maps to one monitor on a node. The UUID remains the durable
identity; `Node::find_monitor` accepts either the UUID or solution name.

## Recording observations

The returned `Monitor` handle exposes owner-thread operations:

- `observe_input(name, values)` bins an input population and computes PSI;
- `observe_scores(scores)` does the same for predicted scores;
- `observe_calibration(scored_labels)` reports expected calibration error and
  Brier score over the supplied labelled population;
- `close_label_window(window, scored_labels)` records accuracy, Brier score,
  and log loss per cohort when that label window closes;
- `observe_guardrail(name, value)` compares one realized metric with its
  declared bounds;
- `acknowledge_retrain()` clears the durable retrain request after an external
  controller accepts it.

PSI normalizes both histograms and applies a small floor before evaluating
`sum((observed - expected) * log(observed / expected))`. This keeps empty bins
finite while making a population that moves into a previously empty bin a large
drift signal. A PSI breaches only when it is strictly above the declared
threshold. A guardrail breaches below its minimum or above its maximum.

Closed label windows replace an existing `(window, cohort)` record, so replaying
the same closure is idempotent. An empty cohort name becomes `all`.

## Breaches and retraining

The replicated value carries the currently active `breaches` array. Each entry
names its kind (`input_psi`, `score_psi`, `calibration`, or `guardrail`), metric,
observed value, declared limit, observation time, and retrain-request state.

When a metric crosses from healthy to breached on the local owner node, the
monitor publishes one JSON event to the internal `topic:solution-events` topic:

```json
{
  "type": "monitor_breach",
  "monitor_id": "...",
  "solution": "renewal-risk",
  "kind": "guardrail",
  "metric": "cost",
  "value": 125.0,
  "maximum": 100.0,
  "retrain_requested": true
}
```

An observation that remains outside bounds updates replicated state without
publishing another event. Recovery removes the active breach; a later crossing
publishes again. `retrain_on_breach` makes the first new breach set
`retrain_requested`; it does not start training itself. A retraining controller
can use the C++ result, replicated flag, or pub/sub event without a dependency
from the monitor back to that controller.

## Replication and ownership

Local mutations capture the entire validated monitor value under
`monitor:<uuid>` before rewriting the disk projection. Peer reconciliation
parses the same schema, materializes or updates the local handle, and never
publishes an event or writes back into the document. All monitor and Node calls
obey the Node owner-thread rule; pub/sub is the existing thread-safe delivery
layer.

## Boundaries

The monitor value is a new schema-as-code boundary, generated as
`schemas/solution_monitor.schema.json`. Breach events use the existing pub/sub
JSON payload boundary and remain node-local like all stage-1 pub/sub events.

Deliberately later: automatic ingestion from solution runs, policy decisions,
holdout assignments, and audit entries. Those foundations call this public
interface when available; this implementation does not depend on their branches.
