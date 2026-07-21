# Parquet writer

zetta could **read** Parquet and not write it, which is why partition bodies were
still JSON-Lines. `zetta::parquet::write_parquet_rows` closes that: a native,
no-Arrow, no-pyarrow writer that emits a complete Parquet file — Thrift-compact
page headers and footer, PLAIN column values, RLE definition levels for nullable
columns, SNAPPY-compressed page bodies, `PAR1` magic at both ends
([ADR-32](https://github.com/oramasearch/zetta/issues/481) / ADR-17: no foreign
runtime; issue #833).

It is the inverse of [`read_parquet_rows`](parquet-reader.md), and it is written
against it. **The round trip is the contract:**

```
write_parquet_rows(rows) → read_parquet_rows(bytes) == rows
```

value for value and **type for type** — an integer stays an integer, a double
stays a double, a bool stays a bool, a null stays a null, and the column order is
the rows' own key order.

## The API

`include/zetta/parquet/writer.hpp` (namespace `zetta::parquet`):

| Function | Returns | Behavior |
|---|---|---|
| `write_parquet_rows(rows, options)` | `ParquetWriteResult` | Write JSON rows as a Parquet file, inferring the schema from the rows. `result.data` is the whole file's bytes. |
| `write_parquet_rows(rows, columns, options)` | `ParquetWriteResult` | The same, against an **explicit** schema — for a caller that must pin `int32`, `float`, or a REQUIRED column. |
| `infer_parquet_schema(rows)` | `ParquetSchemaResult` | The schema inference on its own, so a caller can inspect or adjust it before writing. |

```cpp
#include "zetta/parquet/writer.hpp"

const auto written = zetta::parquet::write_parquet_rows(rows);
if (!written.ok()) {
    // written.error names the column and row that could not be written
}
// written.data is the file
```

Nothing throws — zetta-core is compiled `-fexperimental-library`, so every entry
point returns a result carrying a **path-qualified reason**
(`parquet writer: column "score" row 41: expected number, got string`), exactly
like the reader's. A failure produces **no** bytes, never a half-written file.

`WriterOptions` controls the layout: `codec` (SNAPPY by default —
`Uncompressed` is accepted for debugging, anything else is refused because zetta
has no encoder for it), `max_rows_per_row_group`, `max_rows_per_page`, and the
footer's `created_by`.

## What it writes

```
PAR1
[page header][page body] …          one chunk per (row group, column);
                                    chunks are laid out column-major
                                    inside a row group
[Thrift FileMetaData]               schema + row groups + chunk offsets
[footer length, 4-byte LE]
PAR1
```

A **page body** is the definition-level stream (only for an OPTIONAL column:
4-byte LE length, then an RLE-hybrid run of 1-bit levels) followed by the
PLAIN-encoded present values; the whole body is then compressed with the page
codec. Definition levels are emitted as **RLE runs**, so an all-present or
all-null page costs a handful of bytes regardless of its length.

Scope is deliberately the exact shape the reader already handles, because the
reader is the oracle: **flat** columns (no nesting or repetition), **v1** data
pages, **PLAIN** values, and the physical types `read_parquet_rows` materializes
— `boolean`, `int32`, `int64`, `float`, `double`, `byte_array`. Dictionary
encoding, data page v2, and nested schemas are later slices. A column this writer
cannot express **fails with a reason** rather than being silently coerced.

`byte_array` columns are annotated `converted_type = UTF8`: JSON strings are
UTF-8 by definition of the data model, zetta's own reader ignores the annotation,
and every other tool renders an unannotated `byte_array` as opaque binary.

## Schema inference

`infer_parquet_schema` reads the rows and decides:

| Observed in a column | Physical type |
|---|---|
| only booleans | `boolean` |
| only integers | `int64` |
| any real among the numbers | `double` (an integer widens) |
| only strings | `byte_array` |
| nothing but nulls | `byte_array` (every row of it is null anyway, so any type round-trips; this one says the least) |

A column is **OPTIONAL** if any row omits it or holds JSON null for it, and
**REQUIRED** otherwise — so a dense batch pays no definition-level bytes at all.
Column order is the sorted key order the rows present, which is also the order
`read_parquet_rows` hands rows back in, so a round trip is order-stable.

A column that **mixes families** (a string and a number, a boolean and a number)
or holds an array/object fails with a reason instead of picking a type. There is
no physical type that would round-trip such a column, and a writer that guessed
would break the very property callers depend on; a caller who knows better can
pass an explicit schema.

Two consequences worth stating plainly, both of them the *inference's* limits
rather than the writer's:

- Inference sees only **this batch**. A column that happens to be dense here is
  written REQUIRED, so appending a null to it later needs an explicit schema.
- An integer above `INT64_MAX` (JSON's unsigned range) is refused rather than
  wrapped.

## Correctness — the round trip is the test

`tests/data/test_parquet_writer.cpp`:

- **Every scalar type**, with the JSON type asserted on the way back (not just
  the value) — `is_number_integer`, `is_number_float`, `is_boolean`, `is_string`.
- **Degenerate tables** — no rows, a row with no columns, one row, and a schema
  with columns but no rows (which still declares its columns).
- **Nulls in every position** — first, middle, last of every column; an all-null
  column; a key missing from a row entirely; and nulls landing exactly on a page
  boundary.
- **Awkward strings** — empty, unicode, an embedded NUL, and strings of 65 536
  and 70 000 bytes that cross the length-field boundaries.
- **Numeric edge values** — `INT64_MIN`/`INT64_MAX`, ±inf, NaN, `-0.0`,
  denormals. NaN and `-0.0` are asserted on the **bits** (`std::isnan`,
  `std::signbit`), because JSON equality alone would pass a writer that lost
  either.
- **Many pages and row groups** — 5 000 rows at 61 rows per page and 512 per row
  group, plus the assertion that the file is smaller than the same rows as
  JSON-Lines (which is the reason the codec is SNAPPY).
- **Every failure path** — invalid options, an unsupported codec, a duplicate or
  empty column name, an unsupported physical type, a null in a REQUIRED column, a
  value of the wrong JSON type, an out-of-range `int32` — each returning a reason
  and no bytes.

### External validity

The committed goldens under `tests/fixtures/parquet/` were produced by pyarrow
and prove the *reader* is right. The writer borrows that: **read a golden, write
it back out with the writer, read that again, and require the same rows**
(`RewritesPyarrowGoldensToTheSameRows`). A writer bug that produced a file only
zetta's reader accepts would have to be matched by a reader bug that mis-reads
pyarrow's output — the two are checked against each other.
(`churn_sample.parquet` is deliberately excluded: pyarrow wrote it with the ZSTD
page codec, which zetta cannot decode yet.)

For conformance beyond zetta's own reader, set `ZETTA_WRITE_PARQUET_SAMPLES` to a
directory and that test drops its output files there to be opened with pyarrow /
DuckDB **by hand** — the same discipline as `ZETTA_WRITE_GOLDEN`: offline,
dev-time, never a build or CI dependency (ADR-17).

## The Thrift compact writer

`src/core/parquet/thrift_compact.hpp` carries both halves of the protocol:
`CompactReader` (the cursor the footer and page-header parsers read through) and
`CompactWriter`, its inverse. They live in one header because they have to agree
byte for byte.

`CompactWriter` cannot fail — a growing buffer has no bounds to run past — so its
methods return `void`. The discipline it carries instead is **structural**: it
owns the running last-field-id (compact encodes a field id as a delta from the
previous field in the *same* struct) and the stack of those ids across nested
structs, because those are exactly what a hand-rolled emitter gets wrong.
`begin_struct_field` / `end_struct` are the only correct way to nest.

## How it fits

This is the write-side foundation of
[#833](https://github.com/oramasearch/zetta/issues/833): once partition bodies
are Parquet instead of JSON-Lines, there is one body format, one read path, one
write path, and column projection everywhere. The paging `ParquetStore`, Parquet
partition bodies, and the scatter-gather read path build on this writer being
correct — which is why the round trip, not a byte-for-byte golden, is what this
slice is held to.
