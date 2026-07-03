# DDF DataFrame ops (`zetta::ddf`)

The local **operators over the [DDF DataFrame](ddf.md) substrate** (**ARC-12**; part
of the native distributed accelerated-compute pass for UC-1 churn,
[#264](https://github.com/oramasearch/zetta/issues/264)). Two families landed
together as the leaf of the DDF op algebra:

- **scan / filter / project**
  ([#381](https://github.com/oramasearch/zetta/issues/381)) — read a table into a
  frame, keep matching rows, and select / compute columns.
- **sort / concat / slice**
  ([#385](https://github.com/oramasearch/zetta/issues/385)) — order a frame by a key
  column, row-wise union, and a contiguous row range.

They are the single-node building blocks the cross-node ops compose: the
[**distributed sort**](ddf-distributed-sort.md) (`zetta::ddf::DistributedSorter`) and
the [**distributed equi-join**](ddf-join.md) (`zetta::ddf::distributed_join`).

```
include/zetta/ddf/ops.hpp              scan / filter / project + FilterPredicate / Projection
src/core/ddf/ops.cpp                   the ops + the predicate/projection JSON boundaries
schemas/ddf_filter_predicate.schema.json   the filter-predicate JSON boundary (draft-07)
schemas/ddf_projection.schema.json         the projection-spec JSON boundary (draft-07)
tests/test_ddf_ops.cpp                 the scan/filter/project acceptance + per-op + JSON-boundary tests
include/zetta/ddf/data_frame_ops.hpp   sort / concat / slice declarations
src/core/ddf/data_frame_ops.cpp        the ops + the host-staging helpers
src/core/ddf/data_frame_internal.hpp   the internal HostFrame staging (shared with distributed_sort)
tests/test_data_frame_ops.cpp          single-node sort/concat/slice op + boundary tests
```

## What they are — and where the compute runs

These are **DDF-level ops**: free functions in `zetta::ddf` that operate over a
[`DataFrame`](ddf.md), *built on* the `ComputeDevice` operator seam — **not** new
methods on it. The `ComputeDevice` interface is the fixed, low-level buffer-op set
(map / elementwise / reduce / gather / sort_by_key / …); DDF ops compose those
primitives over a frame's chunked columns.

Each op reads its input frame(s) — never mutating them — and produces a **fresh,
move-only** `DataFrame` (a `DataFrameResult`, like the loader: the new `frame` plus a
`ComputeStatus` that explains any failure). All of them are **non-throwing**, matching
the loader and zetta-core's no-exceptions convention (the frame is populated only when
`status.ok()`; nothing unwinds across the zetta-core seam).

The heavy data movement runs **through the device's existing operators**, so it is
accelerated and CUDA-parityable: `filter` gathers, `project`'s computed columns
map/elementwise, and `sort`'s compare runs on the device via `sort_by_key`. The only
host-side steps are evaluating a filter predicate into row indices and the
concat/slice memcpy plumbing (see below).

## scan — table → frame

`scan` is the leaf op: read a table/store into a typed, chunked frame. The
schema-less overload **infers** the schema ("just scan the table"); both overloads
are thin wrappers over [`load_data_frame`](ddf.md#loading-a-table-into-a-frame), so
`scan`/`filter`/`project` read as one vocabulary.

```cpp
#include "zetta/ddf/ops.hpp"
#include "zetta/compute/cpu_device.hpp"

zetta::compute::CpuDevice device;

// Inferred schema (numeric fields → columns):
auto scanned = zetta::ddf::scan(table.store(), device);

// Or an explicit schema:
auto scanned = zetta::ddf::scan(table.store(), schema, device);
```

## filter — keep matching rows

`filter` keeps the rows of a frame that satisfy a **predicate**, returning a new
frame with the **same columns** (names + dtypes) and the **same `chunk_rows`**,
holding only the surviving rows. The selected rows move through the device **`gather`**
op (per source chunk, then re-packed into regular output chunks).

A `FilterPredicate` is a single-column comparison — `column <comparison> value`.
Each row's value is **widened to double** before the comparison, so one predicate
type covers every numeric column dtype (exact for integers up to 2⁵³; float
equality carries the usual caveats).

```cpp
using zetta::ddf::Comparison;

auto kept = zetta::ddf::filter(
    scanned.frame,
    {.column = "score", .comparison = Comparison::GreaterEqual, .value = 0.5});
if (!kept.ok()) { /* kept.status.message explains why */ }
```

`Comparison` is `Equal` / `NotEqual` / `Less` / `LessEqual` / `Greater` /
`GreaterEqual` (`eq` / `ne` / `lt` / `le` / `gt` / `ge`). The comparison itself runs
on the **host** — the `ComputeDevice` op set has no comparison / stream-compaction
primitive (a deliberate scope note; a select/compare op on the seam is a separate
issue) — exactly as the [loader](ddf.md#the-value-boundary)'s JSON→value conversion
does; the row selection and every computed column stay on the device. `filter` fails
with `InvalidArgument` if the frame has no device (it is empty) or the predicate
column is absent. Selecting no rows yields a well-formed empty frame (0 rows, each
column 0 chunks).

## project — select / compute columns

`project` builds a new frame with the projection's **output columns** over the
frame's rows — the **same `row_count` + `chunk_rows`**, the columns selected or
computed. Each `ProjectColumn` is exactly one of three modes (build with the
factories so the mode is well-formed):

| Mode | Factory | Backed by |
| --- | --- | --- |
| **Passthrough** — copy/reorder an existing column | `ProjectColumn::passthrough(name, source)` | a chunk copy |
| **Unary** — a `UnaryOp` over one column | `ProjectColumn::unary(name, op, source)` | the device `map` op |
| **Binary** — a `BinaryOp` over two columns | `ProjectColumn::binary(name, op, left, right)` | the device `elementwise` op |

```cpp
using zetta::compute::BinaryOp;
using zetta::compute::UnaryOp;

zetta::ddf::Projection projection;
projection.columns = {
    zetta::ddf::ProjectColumn::passthrough("a", "a"),       // pick column a
    zetta::ddf::ProjectColumn::binary("sum", BinaryOp::Add, "a", "b"),   // a + b
    zetta::ddf::ProjectColumn::unary("nega", UnaryOp::Negate, "a"),      // -a
};
auto out = zetta::ddf::project(kept.frame, projection);
```

A computed column **inherits its input dtype** (binary requires both inputs share a
dtype — `elementwise` enforces it). A float-only unary op (`sqrt` / `exp` / `log`)
over an integer column fails with `InvalidArgument` from the device op, propagated
out. `project` also fails with `InvalidArgument` if the frame has no device, the
projection is empty, or a referenced column is absent.

The computed-column vocabulary is intentionally flat (passthrough / unary / binary)
— scalar broadcasts (`col * 2`) and nested expression trees are later slices.

## sort / concat / slice — frame shaping

| Op | Signature | Result |
| --- | --- | --- |
| **sort** | `sort(frame, key_column, order = Ascending)` | a new frame, rows globally ordered by `key_column`, every other column carried along (the payload follows its row). **Stable** — equal keys keep input order. |
| **concat** | `concat({&a, &b, …})` | row-wise union (bag union, no dedup): the inputs appended in order. Every frame must share the same schema (column names + dtypes, same order). |
| **slice** | `slice(frame, row_begin, row_end)` | the contiguous row range `[row_begin, row_end)` (clamped to the row count), same schema. The natural way to cut a sorted frame into range partitions. |

```cpp
#include "zetta/ddf/data_frame_ops.hpp"

auto sorted = zetta::ddf::sort(frame, "risk_score",
                               zetta::compute::SortOrder::Descending);
auto lower  = zetta::ddf::slice(sorted.frame, 0, 100);     // top-100 range
auto merged = zetta::ddf::concat({&lower.frame, &tail.frame});
```

### How the sort runs

The **compare sort runs on the frame's `ComputeDevice`**: the op uploads the key
column plus an index buffer `[0, rows)` and calls
[`sort_by_key`](../compute/compute-device.md) — the device does the actual O(n log n) work and
hands back the **sort permutation** (so a CUDA-backed frame sorts on the GPU). The
rows are then gathered through that permutation, and the result re-chunked at the
input frame's chunk size. The surrounding data movement (gather / concat / slice)
is plain host memcpy — *correctness over throughput*, the same stance
[`CpuDevice`](../compute/cpu-device.md)'s own sort takes — staged through an internal
type-erased `HostFrame` so the orchestration stays simple and dtype-generic.

A frame's rows are device buffers chunked per column; because sorting reorders
*every* row, concat appends, and slice cuts, each op necessarily builds a new
frame rather than editing in place (`DataFrame` is move-only and owns its device
memory). A 0/1-row frame round-trips through `sort` unchanged; an empty `slice` range
yields a valid 0-row frame.

## The JSON boundaries

`FilterPredicate` and `Projection` each carry a **validated JSON boundary** — the
same hand-written, non-throwing `to_json` / `from_json` pattern as
[`DataFrameSchema`](ddf.md#the-schema-json-boundary) / `ScalingPolicy`, mirroring
`schemas/ddf_filter_predicate.schema.json` and `schemas/ddf_projection.schema.json`
(draft-07) — so a future query layer or [`ComputeJob`](../compute/compute-jobs.md) can
serialize an op spec and have it fail fast on malformed input. `from_json` returns
`std::nullopt` (never throws) unless the document is well-formed.

```json
{ "column": "score", "comparison": "ge", "value": 0.5 }
```

```json
{ "columns": [ { "name": "a", "source": "a" },
               { "name": "root", "source": "x", "unary": "sqrt" },
               { "name": "prod", "binary": "multiply", "left": "a", "right": "b" } ] }
```

For a projection column, the mode is determined by which fields are present:
`source` only → passthrough; `source` + `unary` → unary; `left` + `right` +
`binary` → binary. Output column names must be unique; unknown op / comparison
names, empty names, and mixed-mode columns are rejected.

`sort` / `concat` / `slice` introduce **no new JSON format** — they consume and
produce already-validated `DataFrame`s, rejecting at the C++ boundary instead, with a
clear [`ComputeStatus`](../compute/compute-device.md#error-model-computestatus):

| Situation | Result |
| --- | --- |
| `sort` key column absent | `InvalidArgument` |
| `sort`/`slice` on an empty frame (no device) with rows | `InvalidArgument` |
| `concat` of an empty list | `InvalidArgument` |
| `concat` frames disagree on schema | `InvalidArgument` |
| a device allocation / copy failure | propagated from the device |

## Tests

`tests/test_ddf_ops.cpp` covers the scan/filter/project **acceptance** (scan → filter
→ project on a fixture frame matches the expected rows + cols, with `chunk_rows` small
enough that the kept rows span multiple chunks — exercising the per-chunk gather and
re-pack), each op in isolation (every comparison, multi-chunk gather, select-none,
column select/reorder, unary/binary compute, the float-only-op rejection, and the
absent-column / empty-frame / empty-projection guards), and the **`FilterPredicate` /
`Projection` JSON boundaries** (round-trip plus rejection of every malformed shape).
The ops are generic over any `ComputeDevice`, so a guarded block asserts **CUDA parity
where built** (`ZETTA_WITH_CUDA`): the same filter+project run on a `CudaDevice` must
match the `CpuDevice` reference.

`tests/test_data_frame_ops.cpp` covers sort's ordering + payload-carry, descending
order, sort **stability** for equal keys, sorting **across chunks** (a small
`chunk_rows`), the unknown-key and single-row paths; concat's row-order append,
single-frame copy, empty-list and schema-mismatch rejections; slice's range cut and
out-of-range clamping; and the **slice → sort → concat** round-trip that reassembles a
global order — the shape the [distributed sort](ddf-distributed-sort.md) relies on.

## What's next

The [distributed sort](ddf-distributed-sort.md) and [distributed equi-join](ddf-join.md)
compose these across the cluster. Further DDF ops — groupby-aggregate / as-of windowing
for the churn featurizer — and the [`ComputeJob`](../compute/compute-jobs.md) `ddf` kind that
schedules them (through the [job coordinator](../compute/job-coordinator.md)) are later issues
against this substrate.

See also [ddf.md](ddf.md) (the DataFrame core), [compute-device.md](../compute/compute-device.md)
(the device seam), and [cpu-device.md](../compute/cpu-device.md) (the reference backend).
