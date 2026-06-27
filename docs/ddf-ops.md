# DDF DataFrame ops — sort / concat / slice (`zetta::ddf`)

The first **operators** over the [DDF DataFrame](ddf.md) substrate (**ARC-12**,
[#385](https://github.com/oramasearch/zetta/issues/385)): **sort** (order a frame by
a key column), **concat** (union / row-wise concatenation), and **slice** (a
contiguous row range). They are the local, single-node building blocks the
[**distributed sort**](ddf-distributed-sort.md) composes across the cluster.

```
include/zetta/ddf/data_frame_ops.hpp   sort / concat / slice declarations
src/core/ddf/data_frame_ops.cpp        the ops + the host-staging helpers
src/core/ddf/data_frame_internal.hpp   the internal HostFrame staging (shared with distributed_sort)
tests/test_data_frame_ops.cpp          single-node op + boundary tests
```

## What they are

Each op reads its input frame(s) — never mutating them — and produces a **fresh,
move-only** `DataFrame` (a `DataFrameResult`, like the loader: the new `frame` plus
a `ComputeStatus` that explains any failure). All three are **non-throwing**,
matching the loader and zetta-core's no-exceptions convention.

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

## How the sort runs

The **compare sort runs on the frame's `ComputeDevice`**: the op uploads the key
column plus an index buffer `[0, rows)` and calls
[`sort_by_key`](compute-device.md) — the device does the actual O(n log n) work and
hands back the **sort permutation** (so a CUDA-backed frame sorts on the GPU). The
rows are then gathered through that permutation, and the result re-chunked at the
input frame's chunk size. The surrounding data movement (gather / concat / slice)
is plain host memcpy — *correctness over throughput*, the same stance
[`CpuDevice`](cpu-device.md)'s own sort takes — staged through an internal
type-erased `HostFrame` so the orchestration stays simple and dtype-generic.

A frame's rows are device buffers chunked per column; because sorting reorders
*every* row, concat appends, and slice cuts, each op necessarily builds a new
frame rather than editing in place (`DataFrame` is move-only and owns its device
memory).

## The boundary

There is **no new JSON format** here — the ops consume and produce already-validated
`DataFrame`s. They reject at the C++ boundary instead, with a clear
[`ComputeStatus`](compute-device.md#error-model-computestatus):

| Situation | Result |
| --- | --- |
| `sort` key column absent | `InvalidArgument` |
| `sort`/`slice` on an empty frame (no device) with rows | `InvalidArgument` |
| `concat` of an empty list | `InvalidArgument` |
| `concat` frames disagree on schema | `InvalidArgument` |
| a device allocation / copy failure | propagated from the device |

A 0/1-row frame round-trips through `sort` unchanged; an empty `slice` range yields
a valid 0-row frame.

## Tests

`tests/test_data_frame_ops.cpp` covers ordering + payload-carry, descending order,
sort **stability** for equal keys, sorting **across chunks** (a small `chunk_rows`),
the unknown-key and single-row paths; concat's row-order append, single-frame copy,
empty-list and schema-mismatch rejections; slice's range cut and out-of-range
clamping; and the **slice → sort → concat** round-trip that reassembles a global
order — the shape the [distributed sort](ddf-distributed-sort.md) relies on.

## What's next

The [distributed sort](ddf-distributed-sort.md) composes these across the cluster
(sample-partition + local sort + merge). Further DDF ops — join / groupby-aggregate
/ as-of windowing for the churn featurizer — and the [`ComputeJob`](compute-jobs.md)
`ddf` kind that schedules them are later issues against this substrate.

See also [ddf.md](ddf.md) (the DataFrame core), [compute-device.md](compute-device.md)
(the device seam), and [cpu-device.md](cpu-device.md) (the reference backend).
