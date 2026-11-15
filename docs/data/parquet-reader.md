# Parquet reader → DDF columns (column projection)

`zetta::ddf::load_parquet_data_frame` is the native, read-only **Parquet reader**:
it assembles the [footer / metadata parser](parquet-metadata.md) (#505), the
[column-chunk decoders](parquet-column-decoders.md) (#506), and the
[Snappy page codec](parquet-snappy.md) (#507) into a reader that materializes a
**requested subset** of a file's columns straight into a typed, chunked
[DDF `DataFrame`](../ddf/ddf.md) (DeviceBuffer-backed columns). **No Apache Arrow, no
parquet-cpp.** This is the capstone (fourth) of the native Parquet slices
(ADR-32 / #481); it removes the `ChurnDataFormat::Parquet` "not yet supported"
error and wires Parquet into the dataset loader path.

- **Code:** `include/zetta/ddf/parquet_reader.hpp`,
  `src/core/ddf/parquet_reader.cpp` (namespace `zetta::ddf`, in `zetta-core`) for
  the typed-columns reader; `include/zetta/parquet/reader.hpp`,
  `src/core/parquet/reader.cpp` (namespace `zetta::parquet`) for the untyped
  whole-file rows reader.
- **Test:** `tests/data/test_parquet_reader.cpp` (the DDF reader, TC-9 parity) +
  `tests/usecases/churn/test_churn_dataset.cpp::ParquetFormatLoadsIdentically` (the rows reader);
  the same parity is also a GUI-runnable in-app **test-suite** case
  (`ddf.parquet_reader_parity`, [test-suite.md](../testing/test-suite.md)).
- **Fixture + golden:** `tests/fixtures/parquet/saas_sample.parquet` +
  `tests/fixtures/parquet/saas_sample.dataframe.json` (TC-9) and the churn
  Parquet siblings `tests/fixtures/churn/accounts.parquet` +
  `tests/fixtures/churn/events.parquet`.
- **Issue / ADR:** #508, the reader → DDF slice of **ADR-32** (#481); verifies
  **TC-9** (#499 — parity vs a Python/pyarrow golden) and mitigates **RSK-10**.

## Column projection — the point

A Parquet file stores each column's bytes in a separate **column chunk** per row
group, so reading column *c* never touches column *c′*'s bytes. The reader
exploits this: the `DataFrameSchema` you pass names **which** columns to read (and
their target dtype), and **only those leaf columns' chunks are decoded** — every
other column's page bytes are never read.

That is what lets a caller load the engineered numeric columns + `outcome` of the
**saas-sales-conversations** dataset while **dropping `embedding_0..3071`** (3072
columns) without paying to decode them:

```cpp
#include "zetta/ddf/parquet_reader.hpp"

zetta::compute::CpuDevice device;

// Read ONLY these columns; embedding_0..3071 are never decoded.
zetta::ddf::DataFrameSchema projection;
projection.columns = {
    {"messages",             "", zetta::compute::DataType::Int64},
    {"avg_response_seconds", "", zetta::compute::DataType::Float64},
    {"sentiment",            "", zetta::compute::DataType::Float64},
    {"questions_asked",      "", zetta::compute::DataType::Int64},
    {"outcome",              "", zetta::compute::DataType::Int64},
};

auto result = zetta::ddf::load_parquet_data_frame(file_bytes, projection, device);
if (!result.ok()) { /* result.status.message explains why */ }
zetta::ddf::DataFrame& frame = result.frame;   // 5 columns, embeddings dropped
```

Each `ColumnSchema`'s `source_field()` is the **parquet leaf path** to read and
`name` is the frame column (so you can rename on load); the reader resolves each
path to its leaf-column index and decodes just those. The chunk layout matches
[`ddf::load_data_frame`](../ddf/ddf.md) (rows-per-chunk = `chunk_rows`,
default `kDefaultChunkRows`), so the frame drops into the rest of the DDF op
algebra unchanged.

`infer_parquet_schema(file_bytes)` is the "just load every number" convenience: it
reads the **metadata only** (no column decode) and returns a `DataFrameSchema`
with one column per numeric leaf, typed to its natural `DataType` (int32 → Int32,
int64 → Int64, float → Float32, double → Float64, boolean → UInt8), skipping
`byte_array` leaves. The projection overload above is what drops the embeddings.

## Dense + numeric boundary

A DataFrame is **numeric and dense**, so the reader is too. Each requested column
is converted to its target `DataType`, **validated at the boundary** (mirroring
[`load_data_frame`](../ddf/ddf.md)'s `extract`):

- an integer physical value **widens** into a float column, and is **range- /
  sign-checked** into an integer column (out-of-range / negative-into-unsigned
  fails);
- a real physical value loads into a float column, but **a real value into an
  integer column fails** (real-into-integer);
- a `byte_array` (string) column **cannot** load into a numeric column;
- a column with **nulls** in the rows read fails (a DataFrame column is dense —
  there is no validity bitmap on a `Column`).

Everything is **non-throwing** (zetta-core is `-fexperimental-library`): a
malformed file, an unsupported page/codec/encoding (see the
[decoder scope](parquet-column-decoders.md#scope-this-slice) — flat columns, page
v1, uncompressed + snappy), a missing / non-numeric / null / out-of-range column,
or a device error returns a failing `ComputeStatus` (and an empty frame) with a
path-qualified reason rather than throwing across the seam.

## Untyped rows reader (the churn loader path)

`zetta::parquet::read_parquet_rows(file_bytes)` is the typed-columns reader's
sibling: it decodes **every** leaf column and transposes the values back into one
`nlohmann::json` object per logical row (`{ column_path: value-or-null, ... }`) —
a null row becomes JSON null, a `byte_array` becomes a JSON string, numeric and
boolean columns become their natural JSON scalar. This is the inverse of the
`JsonlStore`'s row shape, so a `.parquet` file drops into any code path that
already consumes JSON rows. Before transposing a column it checks the decoded
shape with `validate_decoded_column` (one dense value per present row, in the
vector the column's physical type selects), so a crafted file whose row groups
disagree on a column's physical type fails with a reason instead of reading past
the values (LSR-167, see [the decoders](parquet-column-decoders.md)).

That is how the **churn dataset loader** ([churn-dataset.md](../use-cases/churn-dataset.md))
now reads Parquet: `load_churn_dataset(dir, ChurnDataFormat::Parquet)` reads
`accounts.parquet` / `events.parquet` through `read_parquet_rows`, then runs them
through the **same** schema validators and `from_json` mappers as the JSONL/CSV
paths — so all three formats round-trip to the identical `ChurnDataset` (the
`ParquetFormatLoadsIdentically` parity test, mirroring `JsonlAndCsvLoadIdentically`).
The old `parquet format not yet supported` error is gone.

## Boundary validation (schema as code)

The reader's **input projection** rides the existing, validated `DataFrameSchema`
boundary (`schemas/data_frame_schema.schema.json`). The **TC-9 golden** — the
materialized DataFrame a parity test compares against —
(`{ "num_rows", "columns": [ { "name", "dtype", "values": [<number>...] } ] }`)
has its own schema:

- `parquet_data_frame_schema()` (`zetta::ddf`) is the draft-07 schema for that
  document, the **single source of truth**;
  `schemas/parquet_data_frame.schema.json` is generated from it and drift-checked
  by `tests/test_schemas.cpp` (regenerate with `ZETTA_WRITE_SCHEMAS=1`). See
  [schemas.md](../architecture/schemas.md).
- `validate_parquet_data_frame_json(doc)` validates a document against it (the
  embedded pboettch draft-07 validator). The committed golden re-enters the
  program through this check before it is trusted.

## Test + golden (no foreign runtime)

`tests/data/test_parquet_reader.cpp` loads the **projected subset** (engineered numeric
columns + `outcome`) of `saas_sample.parquet` into a DataFrame and asserts it
matches the committed pyarrow golden **exactly on all four axes the TC-9 / #499
acceptance names** — schema, row count, dtypes, and values:

- **schema** — the produced `column_names()` equal the golden's, in order;
- **row count** — `row_count()` equals the golden's `num_rows`;
- **dtypes** — both *produced* and *inferred*. The headline test asserts each
  produced column's `dtype()` equals the golden's declared dtype (not just that
  the values match the dtype the load was *asked* for), and the sibling
  `InfersDtypesMatchingGolden` test runs `infer_parquet_schema` (footer only, no
  decode) and asserts the reader's **own** metadata-driven type inference agrees
  with pyarrow for every projected column — so dtype parity is independent of the
  golden-imposed load, not circular;
- **values** — every column equals the golden field for field.

The fixture has 13 columns (5 engineered/outcome + 8 `embedding_*`); the headline
test asserts the loaded frame has exactly the 5 projected columns (no
`embedding_*` survives) across 2 row groups, and the inference test confirms the
embeddings are *seen* by inference (as `float32`) — proving column projection, not
inference, is what drops them. Further tests cover projection over the
independently-built #506 decode fixture, a custom chunk size, and the
dense/numeric boundary (nulls, byte_array, real-into-integer, missing column,
malformed bytes).

The identical parity is a GUI-runnable [in-app test-suite](../testing/test-suite.md) case
(`ddf.parquet_reader_parity`, category *DDF dataframe ops*): the testkit runs
headless and in the shipped GUI with no on-disk fixture, so the `saas_sample.parquet`
bytes (base64) + the golden JSON are **embedded** in
`src/core/testkit/parquet_fixture_data.hpp` (generated by `gen-parquet-fixture.py`
from the same on-disk fixture — one source of truth). The case decodes the embedded
bytes, re-validates the golden through `validate_parquet_data_frame_json`, and runs
the same four-axis parity (with a `Chunk rows` knob to split each column across
chunks while values stay identical).

All fixtures + goldens are regenerated by **`scripts/gen-parquet-fixture.py`**,
which is **not** a build or CI dependency (ADR-17): it needs `pyarrow` and is run
by hand:

```bash
python3.12 -m venv /tmp/pqvenv && /tmp/pqvenv/bin/pip install pyarrow
/tmp/pqvenv/bin/python scripts/gen-parquet-fixture.py
ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R Schemas   # if the schema changed
```

The committed saas fixture is a **scaled-down** stand-in (8 embedding columns, not
3072) — enough to prove projection drops them — matching the repo's tiny-fixture
convention.

## How it fits

```
#505 footer/metadata ──┐
#506 column decoders ──┼─→ #508 reader → DDF columns (this)
#507 Snappy codec ─────┘        ├─ ddf::load_parquet_data_frame  (typed columns, projection)
                                └─ parquet::read_parquet_rows     (untyped rows → churn loader)
```

The footer parser locates each column chunk's pages; the decoders read +
decompress + decode them into typed values + a validity bitmap; this reader turns
a **chosen subset** into typed [DDF](../ddf/ddf.md) columns (dropping the rest) and wires
Parquet into the dataset loader path. zstd page decompression is the sibling codec
to add behind the [Snappy](parquet-snappy.md) per-page seam when a target file
needs it; driving the reader from a deployed graph / `ddf` `ComputeJob` is a later
slice.

## Bounds on what a file's counts can allocate (LSR-252, #1425)

A file chooses its own footer and page counts, so none of them is trusted to size an allocation:

- `read_parquet_rows` holds every row as a JSON object, so a footer claiming more than
  `kMaxParquetRowsInMemory` (10,000,000) rows is refused before anything is allocated. Larger files
  go through the column decoder.
- Plain decoding checks that the page holds `count` values without overflowing the multiplication
  (`n > bytes / width` is refused). A byte-array page must hold at least four bytes per value.
- RLE/bit-packed decoding reserves no more than the input bytes could plainly encode. A run that
  really repeats a value grows the vector as it decodes.

