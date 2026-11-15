# DDF point-in-time featurization (`zetta::ddf::point_in_time_featurize`)

**Point-in-time featurization** turns raw account + event data into a leakage-free
**feature table** for training — the shape of the churn `churn_features.sql`,
expressed as a DDF graph (**ARC-12 / FEAT-6**,
[#388](https://github.com/oramasearch/zetta/issues/388); part of the native
distributed accelerated-compute pass for UC-1 churn,
[#264](https://github.com/oramasearch/zetta/issues/264)). It is the consumer the
leaf DDF ops were built toward: it composes the
[as-of / window op](ddf-as-of.md) (#384), the [groupby-aggregate op](ddf-groupby.md)
(#383), the [filter op](ddf-ops.md) (#381), and the [equi-join op](ddf-join.md)
(#382) into one featurization.

```
include/zetta/ddf/featurize.hpp          FeaturizeSpec / WindowedFeature / FeatureTable + point_in_time_featurize()
include/zetta/ddf/featurize_job.hpp      run_featurize_job() — the distributed `featurize` ComputeJob
include/zetta/ddf/local_featurize_job.hpp run_near_data_featurize_job() — compute-near-data over a partitioned table (#532)
src/core/ddf/featurize.cpp               the single-node op (as-of + group-by + filter + join; the static carry-through) + the spec JSON boundary
src/core/ddf/featurize_job.cpp           the distributed op (distributed_join + distributed_group_by on a JobRound)
src/core/ddf/local_featurize_job.cpp     load the local partitions, then drive run_featurize_job (compute-near-data)
schemas/featurize_spec.schema.json       the featurization-spec JSON boundary (draft-07)
tests/ddf/test_ddf_featurize.cpp             JSON boundary, single-node (churn + static) + the churn fixture (leakage assertion), distributed
tests/ddf/test_ddf_featurize_partitions.cpp  the near-data featurize over a partitioned loopback cluster (#532)
```

## No leakage — the non-negotiable property

The single most important correctness property of the churn pipeline is **no
leakage**. Every feature for an entity at decision time `t0 = as_of` must be
computed from only the events with `time <= as_of`; the realized churn label and
any post-`t0` signal live *only* in the training target, never in a feature, never
readable by the model at scoring time. A churn feature like "failed payments in the
trailing 90 days" or "total support severity before `t0`" is exactly a **windowed
group-by**: group rows by entity, keep only the rows inside the time window, reduce
their values. Doing it with a hard `time <= as_of` cutoff is what stops the classic
leak where a post-cutoff event sneaks into a feature and inflates the apparent
signal.

The [synthetic churn fixture](../use-cases/churn-dataset.md) plants this trap: a handful of
events after its `as_of_date` of `2026-03-01` (a fourth `payment_failed` for
`acct_contoso_office` on `2026-04-05`, a second `support_ticket` for `acct_globex` on
`2026-03-20`, a `login` for `acct_contoso_freight` on `2026-03-15`). The acceptance
test featurizes the fixture and asserts every feature matches a leakage-free ground
truth computed independently — and that the windowed values are strictly below the
leaky (no-cutoff) values where the trap fired.

## What it operates on

Both inputs are numeric [DataFrames](ddf.md) ([`zetta::compute::DataType`](../compute/compute-device.md)
— float / int / uint only):

- the **events** frame — an integer **entity-key** column (the join key), a numeric
  **timestamp** column, the numeric **value** column features reduce, and an optional
  numeric **event-type code** column (so a feature can count just `payment_failed`
  rows);
- the **accounts** (dimension) frame — the same integer entity key plus the static
  numeric columns carried through (`mrr`, `seats`, `tenure_days`, …) and the optional
  **label** column.

An upstream *featurize* step encodes string ids / ISO-8601 timestamps / categorical
event types into those numeric columns (the test does exactly this projection over
the churn fixture); the op itself stays purely numeric.

## The spec

A `FeaturizeSpec` is a small graph: the entity/time keys, the single `as_of` cutoff,
the windowed `event_features`, the static `account_features`, and the optional
`label_column`.

```cpp
#include "zetta/ddf/featurize.hpp"

zetta::ddf::FeaturizeSpec spec;
spec.entity_column = "entity";   // integer key, present in BOTH frames
spec.time_column   = "ts";       // numeric timestamp in the events frame
spec.as_of         = t0;         // the single point-in-time cutoff (time <= as_of)

spec.event_features = {
    // count of payment_failed events before t0 (involuntary-churn signal)
    {.name = "failed_payments", .agg = AggregateOp::Count,
     .type_column = "type", .type_value = /*payment_failed*/ 3.0},
    // total support severity before t0
    {.name = "support_severity", .agg = AggregateOp::Sum, .value_column = "value",
     .type_column = "type", .type_value = /*support_ticket*/ 4.0},
    // logins in the trailing 30 days [t0-30, t0]
    {.name = "recent_logins", .agg = AggregateOp::Count, .window_start = t0 - 30,
     .type_column = "type", .type_value = /*login*/ 1.0},
};
spec.account_features = {"mrr", "seats", "tenure_days"};  // static, joined by entity
spec.label_column     = "churned";                        // the training target y
```

Each `WindowedFeature` aggregates the rows of an entity whose timestamp falls inside
`[window_start, as_of]` (always `<= as_of`), optionally restricted to one event
type. The `agg` vocabulary is the [groupby-aggregate](ddf-groupby.md) set
(`count` / `sum` / `min` / `max` / `mean`).

## Using it (single node)

```cpp
auto result = zetta::ddf::point_in_time_featurize(events_frame, accounts_frame, spec);
if (!result.ok()) { /* result.status.message explains why */ }

const zetta::ddf::FeatureTable& table = result.table;
table.entities;          // std::vector<std::int64_t> — one row per account, ascending
table.feature_names;     // account_features ++ event-feature names (the design matrix X)
table.feature("failed_payments");  // const std::vector<double>* — that feature's column
table.label_name;        // "churned"
table.labels;            // the target y, per row

// Materialize a device-backed feature DataFrame for training:
auto frame = table.to_data_frame(device);   // entity (Int64) + features (Float64) + label
```

### How the ops compose

| Step | Op | Role |
| --- | --- | --- |
| restrict to one event type | [`filter`](ddf-ops.md) | keep only e.g. `payment_failed` rows |
| windowed `sum` / `min` / `max` | [`as_of_aggregate`](ddf-as-of.md) | the leakage-free reduction (`time <= as_of`) |
| windowed `count` / `mean` | `filter` + [`group_by`](ddf-groupby.md) | window the rows, then aggregate per entity |
| attach the account dimension | [`hash_join`](ddf-join.md) | join the event features onto `mrr` / `seats` / … by entity |

## Left-join semantics, dense output

The feature table has **one row per entity in the accounts frame** (the universe to
score), in ascending key order. An account with no qualifying events still appears,
with its event features filled to **0** — the SQL `COALESCE(agg, 0)`. A `min` / `max`
over an empty window also collapses to 0 (not the reduction's `±inf` identity), so
every feature is a real number a model can consume.

## Static mode — engineered columns, no windowed events (#532)

Not every dataset is event-shaped. The **saas-sales-conversations** dataset (UC-1 at
scale) is a flat per-conversation table of **already-engineered numeric columns**
(`customer_engagement`, `sales_effectiveness`, `conversation_length`, …; the 3072
`embedding_*` columns are dropped at load) plus an `outcome` label. Featurizing it is
not a windowed aggregation — it is a **carry-through**: each row is one entity, the
engineered columns are the features, and `outcome` is the target.

A **static** `FeaturizeSpec` expresses this — `event_features` is empty, so there are
no events to read and no join:

```cpp
zetta::ddf::FeaturizeSpec spec;
spec.entity_column    = "conversation_id";   // the per-row entity key
spec.account_features = {"customer_engagement", "sales_effectiveness",
                         "conversation_length"};  // carried through as X
spec.label_column     = "outcome";           // the target y
// (no event_features; time_column optional — see below)
```

**Point-in-time, "where applicable".** When the spec sets a `time_column`, the row
universe is point-in-time filtered to `time <= as_of` before the carry-through — a
conversation after the decision cutoff is **excluded entirely** (no leakage), exactly
the same `event_ts <= as_of` guarantee the windowed path gives. With no `time_column`
there is no time dimension to filter (the engineered columns are taken as-is). With
windowed `event_features` (the churn shape) a `time_column` is required.

```cpp
spec.time_column = "event_ts";  // optional; when set, drop rows with event_ts > as_of
spec.as_of       = 1000.0;
```

In static mode `point_in_time_featurize` ignores the `events` frame (pass the same
frame for both `events` and `accounts`); the output `FeatureTable` carries the
engineered columns + label, one row per entity, ascending.

## Distributed — the `featurize` ComputeJob

`run_featurize_job` runs the *same* featurization across the cluster through the
[job coordinator](../compute/job-coordinator.md) as a recorded
[`ComputeJob`](../compute/compute-jobs.md) of kind **`featurize`**, so it is durable, audited,
and appears in `system.jobs`. It is the literal `churn_features.sql` shape
(`accounts ⋈ events … GROUP BY account`) realized with the distributed ops:

1. **[distributed_join](ddf-join.md)** the events shard with the accounts shard on
   the entity key — repartitioning by key so every one of an entity's events, enriched
   with that entity's static account columns, is co-located on one node;
2. for each windowed feature, **[filter](ddf-ops.md)** the enriched rows to the
   feature's window (`time <= as_of`, optional trailing bound + event type) and combine
   per entity with **[distributed_group_by](ddf-groupby.md)**; the static account
   columns (constant per entity) ride out of a parallel `distributed_group_by`;
3. assemble the per-entity feature table — **identical on every node**, because
   `distributed_group_by` broadcasts the merged result back to all participants.

```cpp
auto job = node.create_job(zetta::compute::JobKind::Featurize);
auto out = zetta::ddf::run_featurize_job(coordinator, *job, events_shard,
                                         accounts_shard, spec);
// out.table is the GLOBAL feature table, identical on every participating node;
// out.status == JobStatus::Succeeded; the job is recorded in system.jobs.
```

Call it **concurrently on every participating node** (the collectives need all
participants), after `coordinator.start()`. Because raw rows are repartitioned by the
join and only small per-group partials cross the wire (the existing,
schema-validated `CollectiveMessage`), this is **compute-near-data** — no egress of
source rows by default. Single-node and multi-node runs produce identical tables (the
acceptance test asserts both equal the single-node reference).

### Scope note (inner join)

[`distributed_join`](ddf-join.md) is **inner-only** (#382), so an account with no
events in any shard is not emitted by the distributed path — the fixture has events
for every account, so this matches the single-node (left-join) reference there. A
true left join, emitting zero-event accounts with `COALESCE`-0 features, lands when
`distributed_join` gains outer-join support, behind this same entry. The single-node
`point_in_time_featurize` is already left-join (it drives off the accounts universe).

## Compute-near-data over a partitioned table (#532)

`run_near_data_featurize_job` (`include/zetta/ddf/local_featurize_job.hpp`) is the
**compute-near-data** variant — the featurize sibling of
[`run_near_data_group_by_job`](../compute/compute-near-data.md). Where `run_featurize_job` takes
the shards the caller built however it liked, this runner **loads this node's shard
from its OWNED partitions** of a single [partitioned table](../data/partitioned-write-path.md)
(via [`load_local_partitions`](../compute/compute-near-data.md)) and then drives the same
distributed featurization + ComputeJob lifecycle over it. It is how the
saas-sales-conversations engineered columns are featurized over the partitioned
dataset (the static spec above):

```cpp
auto job = node.create_job(zetta::compute::JobKind::Featurize);
auto out = zetta::ddf::run_near_data_featurize_job(
    coordinator, node, *job, table_id, engineered_schema, spec, device);
// out.table is the GLOBAL feature table, identical on every node and equal to a
// single-node featurize over the whole dataset (the acceptance);
// out.partitions_read / rows_read / local_bytes_read = data read NEAR the node;
// out.wire_bytes_moved = the per-entity partials that crossed the wire.
```

Each node reads only the partition bodies it physically holds (no `LOREQ`, no
full-dataset pull — the dropped embeddings never even reach disk here), runs the
featurization over that local shard, and exchanges only the per-entity partials over
the collectives. The global feature table is broadcast, so it is identical on every
node and matches a single-node oracle. Call it **concurrently on every participating
node** after `coordinator.start()`. The single-table runner targets the **static**
engineered-columns shape; a windowed/churn featurize over two partitioned tables runs
through `run_featurize_job` directly with two `load_local_partitions` calls. See
[compute-near-data.md](../compute/compute-near-data.md).

## Errors (non-throwing)

Like the rest of the compute seam, neither entry point throws across the zetta-core
boundary — they return a failing
[`ComputeStatus`](../compute/compute-device.md#error-model-computestatus) (single node) or drive
the job to **Failed** with the reason in its `detail` (distributed):

| Situation | Result |
| --- | --- |
| a named column is absent | `InvalidArgument` |
| the entity column is not an integer dtype | `InvalidArgument` |
| a feature's window / value is malformed | `InvalidArgument` |
| a device read-back fails | that device's status |
| a collective times out / a peer is unreachable (distributed) | the round's failure |

## The spec JSON boundary

`FeaturizeSpec` carries a validated JSON boundary — the same hand-written,
non-throwing `to_json` / `from_json` pattern as
[`AsOfAggregate`](ddf-as-of.md#the-spec-json-boundary) and
[`GroupBySpec`](ddf-groupby.md), mirroring `schemas/featurize_spec.schema.json`
(draft-07). The JSON form is:

```json
{ "entity_column": "entity", "time_column": "ts", "as_of": 60,
  "event_features": [
    { "name": "failed_payments", "agg": "count", "type_column": "type", "type_value": 3 },
    { "name": "support_severity", "agg": "sum", "value_column": "value",
      "type_column": "type", "type_value": 4 },
    { "name": "recent_logins", "agg": "count", "window_start": 30,
      "type_column": "type", "type_value": 1 } ],
  "account_features": ["mrr", "seats", "tenure_days"],
  "label_column": "churned" }
```

A **static** spec omits `time_column` / `as_of` / `event_features` (the canonical
`to_json` form drops them when empty):

```json
{ "entity_column": "conversation_id",
  "account_features": ["customer_engagement", "sales_effectiveness", "conversation_length"],
  "label_column": "outcome" }
```

`from_json` returns `std::nullopt` (never throws) unless the document is an object
with a non-empty string `entity_column`; an **optional** non-empty `time_column`
(**required** iff `event_features` are present) carrying a numeric `as_of` when set; an
**optional** `event_features` array of well-formed features (non-empty `name`, known
`agg`, a `value_column` whenever the agg needs one, an optional `window_start` no
greater than `as_of`, `type_column` and `type_value` together or not at all); an
optional `account_features` array of non-empty strings; an optional non-empty
`label_column`; **at least one feature column** (an event feature or an account
feature — a label alone is not a feature table); all output column names unique; and no
unknown keys.

## This slice — and what's next

This is the **reference featurization**: the single-node op reads the columns back
from the frame's device and composes the leaf ops in host code (correctness over
throughput, the way [`CpuDevice`](../compute/cpu-device.md) is the numerical reference for
[`CudaDevice`](../compute/compute-device.md)); the distributed op runs it as a recorded
`featurize` ComputeJob; and `run_near_data_featurize_job` runs it compute-near-data
over a partitioned table. The slices built on the feature table have shipped:
**training** a model from it over the partitioned dataset — the standalone
[train over partitions](../use-cases/uc1-train-over-partitions.md) (#533) and the
[featurize → train composition](../use-cases/uc1-featurize-train-over-partitions.md)
(#530, the GBDT fed this feature table) — and the
[UC-1-at-scale E2E](../use-cases/uc1-at-scale-e2e.md) (#531). Still a later slice:
the left-join distributed path once `distributed_join` gains outer-join support.

See also [ddf.md](ddf.md) (the DataFrame substrate), [ddf-as-of.md](ddf-as-of.md)
(the windowing primitive), [ddf-groupby.md](ddf-groupby.md) (the aggregate op),
[ddf-join.md](ddf-join.md) (the join op), [ddf-ops.md](ddf-ops.md) (filter),
[job-coordinator.md](../compute/job-coordinator.md) (the SPMD work surface),
[churn-dataset.md](../use-cases/churn-dataset.md) (the fixture with the leakage trap), and
[architecture.md](../architecture/architecture.md).
