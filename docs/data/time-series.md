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

Both live in the replicated catalogue:

| Key | Value |
|---|---|
| `metric:<name>` | `{name, kind, resolution_ms, retention_ms?, created}` |
| `metric_point:<name>:<bucket start>:<series>:<node>` | `{labels, sum, count, min, max, last, last_at}` |

A node folds each point it receives into the bucket key for (metric, bucket, series, **itself**)
with `ReplicationManager::update`. Only that node ever writes that key, so concurrent recording on
several nodes never conflicts and nothing needs ordering. A query reads every node's buckets from
the replicated catalogue and combines them. The bucket start is 16 zero-padded digits, so a query
walks exactly the key range of its time span ([#1227](https://github.com/oramasearch/zetta/issues/1227):
one range of the shared snapshot, no copy).

`last` takes each series' latest reading in a step (across nodes, by time) and sums those across
the series in a group, so a per-pod gauge reads as the cluster total. `avg` is the sum of every
reading over their count.

Retention: `Node::tick` sweeps every ten seconds, and each node removes only **its own** buckets
that ended before the retention cutoff.

Consistency and durability are deliberately weaker than the record store's: a point is not
journaled, so a crash can lose the buckets written in the second before the catalogue was saved,
and another node's points appear in a query within one gossip interval (about 500 ms). Values that
must be exact, such as an invoice line, belong in a [record](record-store.md).

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
range, all-or-nothing batches; gauge aggregates; two nodes recording the same series and bucket
concurrently, summed after they merge; retention removing only the sweeping node's buckets.
`RecordCapabilities.MetricJourney` covers the capabilities.
