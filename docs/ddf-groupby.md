# DDF distributed groupby-aggregate (`zetta::ddf::group_by`)

The first DDF **operator** over the [DataFrame core](ddf.md) — groupby +
aggregates (`count` / `sum` / `min` / `max` / `mean`, an extensible set),
**distributed**: every node computes a partial aggregate over its own shard, then
the partials are combined across the cluster so every node ends with **identical**
results (**ARC-12**; part of the native distributed accelerated-compute pass for
UC-1 churn, [#264](https://github.com/oramasearch/zetta/issues/264) /
[#383](https://github.com/oramasearch/zetta/issues/383)). It builds on the DDF
core ([#380](https://github.com/oramasearch/zetta/issues/380)) and the
[collectives](collectives.md) ([#365](https://github.com/oramasearch/zetta/issues/365)),
and [runs as a recorded `ddf` `ComputeJob`](#running-it-as-a-recorded-computejob)
through the [job coordinator](job-coordinator.md)
([#387](https://github.com/oramasearch/zetta/issues/387)).

```
include/zetta/ddf/group_by.hpp   the AggregateOp / GroupBySpec / GroupByResult / GroupByPartial types + entry points
src/core/ddf/group_by.cpp        the local op, the partial merge / wire form, the JSON boundary
schemas/group_by_spec.schema.json   the spec JSON boundary (draft-07)
tests/test_group_by.cpp          single-node, the partial/merge/wire, the JSON boundary, the multi-node acceptance
include/zetta/ddf/group_by_job.hpp   run_group_by_job — the op as a recorded ComputeJob (#387)
src/core/ddf/group_by_job.cpp        drives distributed_group_by through the coordinator's job lifecycle
tests/test_ddf_group_by.cpp          the ComputeJob acceptance (the job records + shows in system.zetta_jobs)
```

## What it is

- **Numeric, like the frame.** Group keys and aggregated values are read back from
  the frame's columns through the [`ComputeDevice`](compute-device.md) download
  seam as `double`s, so the op is **backend-agnostic** — it runs the same over a
  [`CpuDevice`](cpu-device.md) frame or a CUDA frame. Group keys must be finite
  numbers (a NaN key is rejected at the boundary).
- **Deterministic.** The result is one row per distinct key, **keys ascending**, so
  it does not depend on row order or node order. count / sum / min / max combine
  associatively; mean carries a running sum + count and divides only at finalize —
  so the distributed result is bit-identical to the single-node result over the
  whole frame (for integer-valued data, exactly; floating-point sums can differ by
  a ULP across partitions, the usual non-associativity caveat).
- **Non-throwing.** Every entry point returns a
  [`ComputeStatus`](compute-device.md#error-model-computestatus) alongside its
  result (zetta-core compiles with `-fexperimental-library` and must not unwind
  across the seam), with a clear reason on a missing column, a NaN key, or a
  collective failure.

## Specifying a groupby-aggregate

A `GroupBySpec` is the key column plus the aggregates:

```cpp
#include "zetta/ddf/group_by.hpp"

zetta::ddf::GroupBySpec spec;
spec.key = "region";
spec.aggregates = {
    {.op = zetta::ddf::AggregateOp::Count},
    {.op = zetta::ddf::AggregateOp::Sum,  .column = "amount"},
    {.op = zetta::ddf::AggregateOp::Min,  .column = "amount"},
    {.op = zetta::ddf::AggregateOp::Max,  .column = "amount"},
    {.op = zetta::ddf::AggregateOp::Mean, .column = "amount", .output = "avg_amount"},
};
```

Each `Aggregate` is an op, the `column` it reads (required for `sum`/`min`/`max`/
`mean`; ignored for `count`), and an optional `output` name. The output name
defaults to `"count"` for count or `"<op>_<column>"` otherwise (e.g. `sum_amount`,
`mean_amount`) — set `output` to disambiguate.

**Extending the op set.** Add a value to `AggregateOp`, give it a name in
`to_string` / `aggregate_op_from_string`, and handle it in the three switches in
`group_by.cpp` — the per-row accumulate, the `merge`, and the `finalize`. Anything
that can be expressed as an associative accumulator (plus an optional finalize step,
the way mean is sum/count) drops straight into the distributed path.

## Single-node

```cpp
#include "zetta/ddf/data_frame.hpp"
#include "zetta/compute/cpu_device.hpp"

zetta::compute::CpuDevice device;
auto loaded = zetta::ddf::load_data_frame(table.store(),
                                          zetta::ddf::infer_schema(table.store()),
                                          device);

auto outcome = zetta::ddf::group_by(loaded.frame, spec);
if (!outcome.ok()) { /* outcome.status.message explains why */ }

const zetta::ddf::GroupByResult& result = outcome.result;
result.keys;                       // distinct group keys, ascending
result.value("sum_amount");        // const std::vector<double>* (nullptr if absent)
```

`GroupByResult` is columnar: `keys[g]` is group `g`'s key, and `values[a][g]` is
aggregate `a`'s value for group `g` (`value(name)` looks one up by output name).

## Distributed

`distributed_group_by` combines every participant's local partial into one result
identical on every node. It is a template over any **collective surface** that
exposes `gather` + `broadcast` — both [`zetta::Collectives`](collectives.md) and
the job coordinator's [`JobRound`](job-coordinator.md) qualify — so a SPMD job body
calls it directly with its `JobRound`:

```cpp
coordinator.run(job, [&](zetta::compute::JobRound& round) -> JobCoordinator::Outcome {
    // Each node loads only its own shard into `frame`.
    auto outcome = zetta::ddf::distributed_group_by(frame, spec, round, "groupby");
    if (!outcome.ok()) return {.ok = false, .detail = outcome.status.message};
    // outcome.result is identical on every node.
    return {.ok = true, .detail = "groups=" + std::to_string(outcome.result.group_count())};
});
```

The round (SPMD — every node calls with the same `spec` + `tag` over its own shard):

1. **Local partial.** Each node computes a `GroupByPartial` — per group, the row
   count plus one running accumulator per aggregate.
2. **Gather.** Each node serializes its partial to a flat `vector<double>` and the
   [collective `gather`](collectives.md) lands the whole ordered set at the
   deterministic root (lowest node id).
3. **Merge at root.** The root merges every node's partial (count/sum/mean add;
   min/max combine) — order-independent, so the root's single merge is canonical.
4. **Broadcast.** The merged partial is broadcast back to every node.
5. **Finalize.** Every node finalizes the same merged partial (mean = sum/count) →
   the same `GroupByResult`.

This `gather → merge-at-root → broadcast` shape (rather than a key-aligned
allreduce) is the natural fit because each node's set of group keys differs;
gathering whole partials lets the root take the union without a prior key-agreement
round. No new wire format is introduced — the partial rides the existing
collective's `vector<double>` payload, which is schema-validated at the
[signal/collective boundary](signal-plane.md).

## Running it as a recorded ComputeJob

The `distributed_group_by` round above is driven inside an *ad-hoc* `coordinator.run`
body. **`run_group_by_job`** (`include/zetta/ddf/group_by_job.hpp`, **FEAT-8**,
[#387](https://github.com/oramasearch/zetta/issues/387)) packages that exact pattern
as a reusable function that runs the groupby as a **recorded
[`ComputeJob`](compute-jobs.md) of kind `ddf`** — so the work is durable, audited
(`job_finished`), and **appears in `system.zetta_jobs`**, like any other unit of
accelerated compute.

```cpp
#include "zetta/ddf/group_by_job.hpp"

auto job = node.create_job(zetta::JobKind::Ddf);          // the record
auto out = zetta::ddf::run_group_by_job(
    coordinator, *job, my_shard_frame, spec,
    /*round_tag=*/"region-agg-round");                    // shared across nodes

// out.result is the global aggregation (identical on every node, via the
// broadcast); out.status is the job's terminal status.
```

It adds nothing to the distribution math — it calls `distributed_group_by` with the
job's `JobRound` and lets the [coordinator](job-coordinator.md) drive the lifecycle
(`Running → Succeeded | Failed`) and record the job. Call it **concurrently on every
participating node** (the collectives need all participants), each with its own
`ComputeJob`. The `round_tag` must be **identical on every node** (a node's own job
id can't serve — each node holds a distinct `ComputeJob`); empty defaults to a
deterministic, spec-derived tag. Because `distributed_group_by` broadcasts the merged
result, `out.result` is populated on **every** node, not the root alone.

## The partial and its wire boundary

`GroupByPartial` is the mergeable per-node state and the unit the collectives carry.
Its `to_wire()` flattens it to

```
[ aggregate_count, group_count, (key, count, acc_0 .. acc_{k-1}) * group_count ]
```

with groups in ascending key order (so the bytes are deterministic).
`from_wire(ops, wire)` is the **boundary** for a collective-carried partial:
non-throwing, returning `std::nullopt` on a header/length mismatch, a duplicate
key, or a NaN key — so a malformed partial fails the round with a clear reason
instead of corrupting the merge.

## The spec JSON boundary

`GroupBySpec` carries the same hand-written, non-throwing `to_json` / `from_json`
boundary as [`DataFrameSchema`](ddf.md#the-schema-json-boundary), mirroring
`schemas/group_by_spec.schema.json` (draft-07). The JSON form is:

```json
{ "key": "region",
  "aggregates": [ { "op": "count" },
                  { "op": "sum", "column": "amount" },
                  { "op": "mean", "column": "amount", "output": "avg_amount" } ] }
```

`from_json` returns `std::nullopt` (never throws) unless the document is an object
with a non-empty string `key` and a non-empty `aggregates` array of objects, each
with a known `op`, an optional non-empty string `output`, and a non-empty string
`column` whenever the op aggregates a column (`sum`/`min`/`max`/`mean`) — the last
a conditional rule enforced in code on top of the draft-07 shape.

## Tests

`tests/test_group_by.cpp` covers the op names, the **JSON boundary** (round-trip +
every malformed shape), the **single-node** op across the full aggregate set, the
**partial** (wire round-trip, `from_wire` rejections, a hand-merge of the three
shards), and — the acceptance — `GroupBy.DistributedMatchesSingleNode`: a 3-node
loopback cluster where each node loads only its own shard and runs
`distributed_group_by` inside a coordinated job, asserting every node's result
equals the single-node group-by over the whole frame.

## What's next

The groupby now runs as a recorded `ddf` [`ComputeJob`](compute-jobs.md) through the
[job coordinator](job-coordinator.md) (above). The remaining DDF featurization ops
(join — already its own [op](ddf-join.md) — and as-of windowing) over a frame's
columns, schedulable the same way, are later issues against this substrate.

The single-node `group_by` is also reachable from **SQL**: a
`SELECT … GROUP BY` runs through this op (the table is loaded into a `DataFrame`,
`WHERE` applied via `ddf::filter`, then `group_by`), materialized back into result
rows — see [query.md](query.md) → *Aggregation (GROUP BY)* (ARC-12 / API-2, #386).

See also [ddf.md](ddf.md) (the DataFrame core), [collectives.md](collectives.md)
(the combine), [job-coordinator.md](job-coordinator.md) (the SPMD work surface),
[query.md](query.md) (the SQL surface), and
[compute-device.md](compute-device.md) (the device seam).
