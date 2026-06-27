# DDF DataFrame (`zetta::ddf::DataFrame`)

The **DDF (distributed dataframe) core** is a typed, columnar `DataFrame` over the
existing storage engine / tables — the substrate **every DDF op operates on**
(**ARC-12 / GLO-11**, entity **ENT-33**; part of the native distributed
accelerated-compute pass for UC-1 churn,
[#264](https://github.com/oramasearch/zetta/issues/264) /
[#380](https://github.com/oramasearch/zetta/issues/380)).

A table today is untyped JSON rows ([`JsonlStore`](storage.md)'s `{_zid, doc}`
lines). The DataFrame reads a table's rows into **typed numeric columns**,
**chunked** by row range, with each chunk backed by a
[`zetta::compute::DeviceBuffer`](compute-device.md) — so the same CPU/CUDA
operators that back the rest of the compute pass (map / reduce / histogram / gemm /
sort / …) run directly over a frame's columns on whichever device it was loaded
onto.

```
include/zetta/ddf/data_frame.hpp   the DataFrame / Column / DataFrameSchema types + loader
src/core/ddf/data_frame.cpp        the loader, schema inference, and the schema JSON boundary
schemas/data_frame_schema.schema.json   the schema-config JSON boundary (draft-07)
tests/test_data_frame.cpp          load/round-trip, chunking, inference, and boundary tests
```

## What it is — and isn't

- **Numeric.** The element-type set is [`zetta::compute::DataType`](compute-device.md#the-memory-model-devicebuffer)
  — float / int / uint only. A DataFrame is a *numeric projection* of a table:
  the loader pulls the chosen numeric fields out of each row's `doc`. Non-numeric
  fields (strings, objects) can't become columns and are simply not selected.
- **Columnar + chunked.** Each `Column` is a sequence of row-aligned chunks
  (`DeviceBuffer`s of the column's dtype). Chunk `i` holds rows
  `[i·chunk_rows, min((i+1)·chunk_rows, row_count))`, and **every column in a frame
  shares the same chunk layout** — so a chunk index selects the same rows across
  columns. That alignment is the property DDF ops rely on to work chunk-by-chunk
  and to distribute work. The default chunk size (`kDefaultChunkRows = 10 000`)
  matches `JsonlStore::kRecordsPerChunk`, so a frame's chunking lines up with the
  table's on-disk chunking.
- **Device-backed.** A frame is loaded onto a `ComputeDevice` (the always-built
  [`CpuDevice`](cpu-device.md), or a GPU backend). Columns own their device memory
  and free it on destruction (`DeviceBuffer` is move-only RAII), so `DataFrame` and
  `Column` are **move-only**.
- **Storage + read-back only (this slice).** Like the [`Provisioner`](provisioner.md)
  seam, scope is the type + loader + JSON boundary. There is **no Node CRUD, no SQL
  surface, and no replication** — DDF *ops* (featurize / join / groupby / …) and any
  wiring are later issues that build on this.

## Loading a table into a frame

```cpp
#include "zetta/ddf/data_frame.hpp"
#include "zetta/compute/cpu_device.hpp"

zetta::compute::CpuDevice device;

// Explicit schema: which numeric fields, at which dtype.
zetta::ddf::DataFrameSchema schema;
schema.columns = {
    {.name = "score", .dtype = zetta::compute::DataType::Float64},
    {.name = "age",   .dtype = zetta::compute::DataType::Int32},
    // read a differently-named row field into a column:
    {.name = "value", .field = "raw_value", .dtype = zetta::compute::DataType::UInt32},
};

auto result = zetta::ddf::load_data_frame(table.store(), schema, device);
if (!result.ok()) { /* result.status.message explains why */ }

const zetta::ddf::DataFrame& frame = result.frame;
frame.row_count();              // number of rows
frame.column("score");          // const Column* (nullptr if absent)

// Read a column back to the host (the round-trip), concatenating its chunks.
std::vector<double> score;
frame.to_host<double>("score", score);   // ComputeStatus; mirrors ComputeDevice::download
```

`load_data_frame` takes any [`StorageEngine`](storage.md) (`Table::store()`, a
`JsonlStore`, or the in-memory `JsonStore`), so it loads from a real table or from
test fixtures uniformly.

### Schema inference

For "just load the table", `infer_schema(store)` picks the columns for you: a field
becomes a column **iff it is present and numeric in every row** (so the column is
dense and the inferred schema round-trips), typed `Int64` when every value is an
integer, else `Float64`. Columns come out in the first row's key order. An empty
store — or one whose rows share no numeric field — yields an empty schema.

```cpp
auto schema = zetta::ddf::infer_schema(table.store());
auto frame  = zetta::ddf::load_data_frame(table.store(), schema, device);
```

## The value boundary

The conversion from a JSON value to a typed column element is a **trust boundary**,
validated per value. The loader is **non-throwing** (zetta-core compiles with
`-fexperimental-library` and must not unwind across the seam): on the first
offending value it returns a failing [`ComputeStatus`](compute-device.md#error-model-computestatus)
with a row-and-field-qualified reason, and an empty frame. It rejects:

| Situation | Result |
| --- | --- |
| field missing from a row | `InvalidArgument` |
| field is not a JSON number | `InvalidArgument` |
| real number into an integer column (would silently truncate) | `InvalidArgument` |
| negative value into an unsigned column | `InvalidArgument` |
| integer out of range for the column's dtype (e.g. `300` → `uint8`) | `InvalidArgument` |
| `chunk_rows == 0` | `InvalidArgument` |

A **float column accepts integer JSON values** (widened to the float) — the one
implicit conversion, since it can't lose information. Read-back enforces the dtype
too: `to_host<T>` returns `InvalidArgument` if `T` doesn't match the column's dtype
(or the column name is unknown).

## The schema JSON boundary

`DataFrameSchema` carries the same kind of validated JSON boundary as the
[`DeviceInfo`](device-registry.md) capability report: a hand-written, non-throwing
`to_json` / `from_json` mirroring `schemas/data_frame_schema.schema.json` (draft-07).
The JSON form is:

```json
{ "columns": [ { "name": "score", "dtype": "float64" },
               { "name": "value", "field": "raw_value", "dtype": "uint32" } ] }
```

`from_json` returns `std::nullopt` (never throws) unless the document is an object
with a **non-empty** `columns` array of objects, each having a non-empty string
`name`, a known `dtype` (one of the seven `DataType` names), and an optional
non-empty string `field`, with **unique** column names — so a malformed schema
config fails fast with no partial frame.

## Tests

`tests/test_data_frame.cpp` covers the acceptance and the boundaries: the
load → read-back **round-trip** across float / int / uint columns (over both an
in-memory `JsonStore` and a real on-disk `Table`), **chunking** (a small
`chunk_rows` splitting rows into multiple buffers that reassemble in order),
**schema inference** (numeric-field selection, int-vs-float typing, dropping
non-dense fields), the **value boundary** (missing / non-number / real-into-int /
negative-into-unsigned / out-of-range / zero chunk size), the **read-back boundary**
(dtype mismatch, unknown column), and the **schema JSON boundary** (round-trip plus
rejection of every malformed shape).

## DDF operators

The first ops over this substrate are built. The local, single-node ops live in
**[ddf-ops.md](ddf-ops.md)**: **scan / filter / project** (`zetta::ddf::scan` /
`filter` / `project` — read a table into a frame, `gather`-based row filtering, and
`map`/`elementwise` computed columns; #381) and **sort / concat / slice**
(`zetta::ddf::sort` / `concat` / `slice` — order a frame by a key column via the device
`sort_by_key`, row-wise union, and a contiguous row range; #385). Two cross-node ops
compose them: the **[distributed sort](ddf-distributed-sort.md)**
(`zetta::ddf::DistributedSorter` — sample-partition + local sort + merge across the
cluster; #385) and the **[distributed equi-join](ddf-join.md)**
(`zetta::ddf::distributed_join` — repartition two frames by key via collectives, then a
local hash / sort-merge join per shard; #382).

## What's next

Still ahead: groupby-aggregate and as-of windowing for the churn featurization
pipeline, built the same way on the [`ComputeDevice`](compute-device.md) operator set,
plus the [`ComputeJob`](compute-jobs.md) `ddf` kind that schedules them.

See also [compute-device.md](compute-device.md) (the device seam),
[cpu-device.md](cpu-device.md) (the reference backend), [storage.md](storage.md)
(the storage engine), and [architecture.md](architecture.md).
