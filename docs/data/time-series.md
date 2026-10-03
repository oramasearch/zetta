# Labelled time series

Refs [#1231](https://github.com/oramasearch/zetta/issues/1231). Built for lake's metering
([lake#348](https://github.com/oramasearch/lake/issues/348)) and request metrics
([lake#384](https://github.com/oramasearch/lake/issues/384)): counters and gauges with labels,
appended and read back summed over a time range. Run metrics ([../ml/run-metrics.md](../ml/run-metrics.md))
stay tied to ML runs; this is the general form.

`zetta::data::MetricStore` (`include/zetta/data/metric_store.hpp`,
`src/core/data/metric_store.cpp`).

## Model

A **metric** is declared once with a kind, a bucket resolution and an optional retention:

- a **counter** takes non-negative increments; a query sums them;
- a **gauge** takes readings; a query aggregates them by `last` (default), `sum`, `avg`, `min` or
  `max`.

A **point** is `{labels, value, at_ms?}`; `at_ms` defaults to now. A **series** is one label set.

## How it works

The definition lives in the replicated catalogue (`metric:<name>` → `{name, kind, resolution_ms,
retention_ms?, created}`). The buckets live in the [record store's](record-store.md) node-local
log, in the engine-internal collection `_metric.<name>` (API collection names cannot start with
`_`), under keys `<bucket start>:<series>:<node>` whose value is `{labels, sum, count, min, max,
last, last_at}`.

A node folds each point it receives into the key for (bucket, series, **itself**). Only that node
ever writes that key, so concurrent recording on several nodes never conflicts and nothing needs
ordering. The record store's anti-entropy brings each node's buckets to the others within one
round (five seconds). The bucket start is 16 zero-padded digits, so a query walks exactly the keys
of its time span. Keeping buckets out of the replicated document matters: a bucket is rewritten on
every point, and the document would keep every rewrite in its history
([replication.md](../cluster/replication.md#what-does-not-belong-in-the-document-1231)).

`last` takes each series' latest reading in a step (across nodes, by time) and sums those across
the series in a group, so a per-pod gauge reads as the cluster total. `avg` is the sum of every
reading over their count.

Retention is the bucket record's expiry (`bucket start + resolution + retention_ms`): an expired
bucket is invisible at once, every node removes its copy within a second, and anti-entropy never
pulls one back.

Consistency and durability are deliberately weaker than a record's: a bucket write is not fsynced
per point (the log is flushed every tick), and another node's points appear in a query within one
anti-entropy round. Values that must be exact, such as an invoice line, belong in a
[record](record-store.md).

## API

| Capability | Kind | Right | Input → output |
|---|---|---|---|
| `create_metric` | action | `metrics:write` | `{metric, kind, resolution_ms? (1000–86400000, default 60000), retention_ms?}` → `{metric, created}`; idempotent for an identical definition, `409 conflict` otherwise |
| `record_metric` | action | `metrics:write` | `{metric, points: [{labels?, value, at_ms?}] (1–1000)}` → `{metric, recorded}`; every point is validated before any is written |
| `query_metric` | read | `metrics:read` | `{metric, from_ms, to_ms, labels?, group_by?, step_ms?, aggregate?}` → `{metric, aggregate, series: [{labels, total, points: [{at_ms, value}]}]}` |

`from_ms` is rounded down to a bucket and is inclusive; `to_ms` is exclusive. `labels` keeps only
series carrying those values; `group_by` names the labels a result series keeps (none: one series
for everything). `step_ms` must be a multiple of the resolution; without it the range is one step.
A query returns at most 10,000 steps, and only steps that have data. `query_metric` also accepts a
JSON-body POST.

Limits: metric names `^[a-z0-9][a-z0-9_.-]{0,127}$`; at most 16 labels, names
`^[a-z_][a-z0-9_]{0,63}$`, values 256 bytes.

## Tests

`MetricStore.*` (`tests/data/test_metric_store.cpp`): counters by label and step, the `[from, to)`
range, all-or-nothing batches; gauge aggregates; retention hiding and the sweep removing old
buckets. `RecordStoreCluster.ANodeThatJoinsLaterCatchesUpByAntiEntropy` records the same series on
two nodes and reads the sum on both after anti-entropy.
`RecordCapabilities.MetricJourney` covers the capabilities.
