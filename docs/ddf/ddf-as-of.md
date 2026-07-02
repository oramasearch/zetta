# DDF window / as-of (point-in-time) op (`zetta::ddf::as_of_aggregate`)

The **DDF window / as-of operator** is the first op over the
[DDF DataFrame](ddf.md) substrate (**ARC-12**,
[#384](https://github.com/oramasearch/zetta/issues/384); part of the native
distributed accelerated-compute pass for UC-1 churn,
[#264](https://github.com/oramasearch/zetta/issues/264)). It is the
**leakage-free featurization primitive**: for each entity, reduce the values of
only the rows whose timestamp falls inside a time window — at or before an
`as_of` cutoff (and, optionally, at or after a `window_start` lower bound) — so a
feature computed "as of *t0*" can never see an event that happened after *t0*.

```
include/zetta/ddf/as_of.hpp        the AsOfAggregate spec + AsOfResult + as_of_aggregate()
src/core/ddf/as_of.cpp             the host-side reference op + the spec's JSON boundary
schemas/as_of_aggregate.schema.json the aggregation-spec JSON boundary (draft-07)
tests/ddf/test_ddf_as_of.cpp           acceptance (synthetic + the churn fixture), windowing, JSON, errors
```

## Why this is *the* churn primitive

A churn feature like "number of payment failures **before** the decision date" or
"total support-ticket severity in the **trailing 90 days**" is exactly an as-of /
windowed group-by: group rows by entity, keep only the rows inside the time
window, reduce their values. The hard time cutoff is what prevents the classic ML
**leak** — a post-cutoff event sneaking into a feature and inflating the apparent
signal. The [synthetic churn fixture](../use-cases/churn-dataset.md) deliberately plants this
trap (a handful of events after its `as_of_date` of `2026-03-01`); this op is what
drops them.

## What it operates on

The DataFrame is **numeric** ([`zetta::compute::DataType`](../compute/compute-device.md) —
float / int / uint only), so the op works over three numeric columns of one frame:

- an **integer entity-key** column — the group key (an integer dtype, so grouping
  is exact);
- a numeric **timestamp** column — e.g. epoch seconds, or any monotonic encoding
  of time;
- a numeric **value** column — what is reduced per entity.

An upstream *featurize* step converts string ids / ISO-8601 timestamps into those
numeric columns (the test does this projection over the real churn events); the op
itself stays purely numeric.

## Using it

```cpp
#include "zetta/ddf/as_of.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/compute/cpu_device.hpp"

zetta::compute::CpuDevice device;
auto loaded = zetta::ddf::load_data_frame(table.store(), schema, device);

zetta::ddf::AsOfAggregate spec{
    .entity_column = "entity",   // integer key
    .time_column   = "ts",       // numeric timestamp
    .value_column  = "value",    // numeric value to reduce
    .as_of         = 1'700'000'000.0,   // upper cutoff (inclusive)
    // .window_start = 1'690'000'000.0, // optional lower cutoff -> a trailing window
    .op            = zetta::compute::ReduceOp::Sum,
};

zetta::ddf::AsOfResult result = zetta::ddf::as_of_aggregate(loaded.frame, spec);
if (!result.ok()) { /* result.status.message explains why */ }

// One row per distinct entity, ascending key order:
result.column.entities;   // std::vector<std::int64_t>
result.column.values;     // the per-entity reduction over qualifying rows
result.column.counts;     // the per-entity number of qualifying rows
```

## The window

A row **qualifies** iff `time <= as_of` **and** (when `window_start` is set)
`time >= window_start`:

| `window_start` | window | use |
| --- | --- | --- |
| absent | `(-inf, as_of]` | pure point-in-time as-of (all history up to *t0*) |
| set | `[window_start, as_of]` | a trailing window (e.g. the last 90 days before *t0*) |

`as_of` is the upper bound and is **inclusive** — a row exactly at the cutoff is
kept (matching the issue's `event_ts <= as_of`). A `window_start` above `as_of` is
an empty window and is rejected at the boundary.

## The result is dense

Every distinct entity present in the frame appears in the result, in **ascending
key order** (deterministic). An entity whose every row is in the future still
appears, with `count == 0` and the reduction's **identity** value (`Sum → 0`,
`Product → 1`, `Minimum → +inf`, `Maximum → -inf`) — so callers get a feature row
per entity and can tell "no qualifying events" apart via the count. The four
reductions are the [`ComputeDevice`](../compute/compute-device.md) `ReduceOp` set:
`sum` / `product` / `minimum` / `maximum`.

## Errors (non-throwing)

Like the rest of the compute seam the op never throws across the zetta-core
boundary — it returns a failing
[`ComputeStatus`](../compute/compute-device.md#error-model-computestatus):

| Situation | Result |
| --- | --- |
| a named column is absent | `InvalidArgument` |
| the entity column is not an integer dtype | `InvalidArgument` |
| `window_start` is above `as_of` (empty window) | `InvalidArgument` |
| a device read-back fails | that device's status |

## The spec JSON boundary

`AsOfAggregate` carries a validated JSON boundary — the same hand-written,
non-throwing `to_json` / `from_json` pattern as
[`DataFrameSchema`](ddf.md#the-schema-json-boundary) and
[`ScalingPolicy`](../compute/scaling-policy.md), mirroring
`schemas/as_of_aggregate.schema.json` (draft-07). The JSON form is:

```json
{ "entity_column": "account", "time_column": "event_ts",
  "value_column": "amount", "as_of": 1700000000,
  "window_start": 1690000000, "op": "sum" }
```

`from_json` returns `std::nullopt` (never throws) unless the document is an object
with non-empty string `entity_column` / `time_column` / `value_column`, a numeric
`as_of`, a known `op` (`sum` / `product` / `minimum` / `maximum`), an optional
numeric `window_start` no greater than `as_of`, and no unknown keys — so a
ComputeJob (`ddf` kind, later) can carry an as-of spec as JSON that fails fast if
malformed.

## This slice — and what's next

This is the **host-side reference** op: it reads the three columns back from the
frame's device and does the grouped, masked reduction in host code (correctness
over throughput, the way [`CpuDevice`](../compute/cpu-device.md) is the numerical reference
for [`CudaDevice`](../compute/compute-device.md)). Pushing the group-by onto the device's
`sort_by_key` + segmented-scan operators is a later optimization **behind the same
API**. Joins / groupby-aggregate and the [`ComputeJob`](../compute/compute-jobs.md) `ddf`
kind that schedules these ops over the
[work-stealing scheduler](../compute/work-stealing-scheduler.md) are later issues against the
same substrate.

See also [ddf.md](ddf.md) (the DataFrame substrate),
[compute-device.md](../compute/compute-device.md) (the device seam + `ReduceOp`),
[churn-dataset.md](../use-cases/churn-dataset.md) (the timestamped fixture with the leakage
trap), and [architecture.md](../architecture/architecture.md).
