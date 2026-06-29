# Parquet column-chunk decoders

`zetta::parquet::decode_column` / `decode_column_chunk` are the native, read-only
**Parquet data-page decoders** — they read a column's pages into typed values, on
top of the [footer / metadata parser](parquet-metadata.md) (#505) and the
[Snappy page codec](parquet-snappy.md) (#507). **No Apache Arrow, no
parquet-cpp.** This is the third of the four native Parquet slices (ADR-32 /
#481); turning these typed columns into [DDF](ddf.md) columns is the next one
(#508).

- **Code:** `include/zetta/parquet/column_decoder.hpp`,
  `src/core/parquet/column_decoder.cpp` (namespace `zetta::parquet`, in
  `zetta-core`). The shared Thrift compact-protocol reader lives in
  `src/core/parquet/thrift_compact.hpp` (used by both the footer parser and the
  page-header parser).
- **Test:** `tests/test_parquet_column_decoder.cpp`.
- **Fixture + golden:** `tests/fixtures/parquet/churn_decode.parquet` +
  `tests/fixtures/parquet/churn_decode.values.json`.
- **Issue / ADR:** #506, the column-chunk-decoder slice of **ADR-32** (#481) — a
  hand-rolled Parquet reader, no foreign runtime (refines **ADR-17**).

## What it decodes

The footer parser hands back, per column chunk, **where** its pages live (a byte
offset) and **how** they are stored (physical type, codec, encodings). A column
chunk is a sequence of pages: an optional **dictionary page** followed by one or
more **data pages**. This module walks those pages and decodes their values for
the encodings the target (churn-shaped) datasets use:

| Encoding | How | Physical types |
|---|---|---|
| **PLAIN** | fixed-width little-endian; bit-packed for `boolean`; 4-byte-length-prefixed for `byte_array` | int32 / int64 / float / double / boolean / byte_array |
| **dictionary** (`PLAIN_DICTIONARY` / `RLE_DICTIONARY`) | a PLAIN dictionary page + data pages of RLE/bit-packed integer indices into it | all of the above |

It also reads **definition levels** — the RLE/bit-packed-hybrid null markers an
**OPTIONAL** column carries — into a per-row validity bitmap. A **REQUIRED**
column carries no def levels (every row is present), so its `validity` is empty.
The max definition / repetition levels come from the schema, computed during the
footer parser's leaf walk and exposed on `LeafColumn`
(`max_definition_level` / `max_repetition_level`).

### Decoded output

```cpp
#include "zetta/parquet/column_decoder.hpp"

auto meta = zetta::parquet::parse_parquet_metadata(file_bytes);   // #505
auto result = zetta::parquet::decode_column(file_bytes, meta.metadata, /*column_index=*/0);
if (!result.ok()) { /* result.error explains why */ }

const zetta::parquet::DecodedColumn& column = result.column;
column.path;                       // dotted schema path
column.type;                       // PhysicalType
column.num_rows;                   // logical rows, including nulls
column.validity;                   // size num_rows (1=present, 0=null), or empty => all present
column.values.int64_values;        // dense present-only values, by physical type
column.row_is_valid(row);          // validity.empty() || validity[row]
column.valid_count();              // number of present values
```

`DecodedColumn` stores values the **Arrow-native** way: dense, present-only
typed vectors plus a validity bitmap — exactly the split the DDF projection
(#508) consumes. Nulls are **not** stored in the value vectors; they are marked
in `validity` and re-expanded only by `to_json`. `decode_column_chunk` decodes
one row group's slice; `decode_column` decodes a whole leaf column across all row
groups, concatenated in row-group order.

### Scope (this slice)

- **Flat columns, page version 1** — the shape pyarrow writes by default and the
  target datasets use. A nested / repeated column (`max_repetition_level > 0`)
  fails with a reason.
- **Codecs:** uncompressed + **snappy** (what zetta can decompress today). A page
  compressed with a codec without a decoder yet (**zstd / gzip**) fails with a
  clear reason — the metadata fixture deliberately mixes those in, and decoding
  such a column reports `unsupported page codec` rather than producing garbage.
  zstd is the documented [Snappy](parquet-snappy.md) follow-up.
- **Encodings:** PLAIN + dictionary. A **v2 data page**, or an unsupported value
  encoding (delta / byte-stream-split), fails with a reason.
- **No DDF / SQL / run wiring** — projecting these columns into typed
  [DDF](ddf.md) columns is #508.

Everything is **non-throwing** (zetta-core is `-fexperimental-library`): a
malformed / truncated / unsupported page, or an out-of-range dictionary index,
fails the decode with a reason instead of reading out of bounds or producing
wrong values — the binary analog of schema-at-the-boundary.

## How a page is read

1. **Page header** — each page begins with a Thrift compact `PageHeader`
   (`type` / `uncompressed_page_size` / `compressed_page_size` + the
   `DataPageHeader` or `DictionaryPageHeader`), read with the shared
   `detail::CompactReader`. The (compressed) body of `compressed_page_size` bytes
   follows immediately.
2. **Decompress** — uncompressed pass-through, or
   [`snappy_decompress`](parquet-snappy.md); other codecs fail.
3. **Definition levels** — for an OPTIONAL column, the v1 data page body begins
   with a 4-byte little-endian length, then the RLE/bit-packed-hybrid level
   stream (bit width `ceil(log2(max_def_level + 1))`); a level equal to
   `max_def_level` marks a present row.
4. **Values** — PLAIN (per the table above) or dictionary indices (a leading
   bit-width byte, then RLE/bit-packed indices mapped through the chunk's PLAIN
   dictionary page).

The **RLE/bit-packed hybrid** decoder (shared by definition levels and dictionary
indices) reads runs that are either a bit-packed run (`(num_groups << 1) | 1`,
groups of 8 values, each `bit_width` bits LSB-first) or an RLE run
(`(run_length << 1) | 0`, one `ceil(bit_width/8)`-byte value repeated).

## Boundary validation (schema as code)

The binary input is bounds-checked at every read. The **JSON projection**
`to_json` emits — `{ "path", "type", "values": [ <value-or-null> ... ] }`, one
`values` entry per logical row (a null row is JSON null; a `byte_array` is its
bytes as a JSON string) — has a schema:

- `parquet_columns_schema()` is the draft-07 schema for a decoded-columns
  document (`{ "num_rows", "columns": [ … ] }`), the **single source of truth**;
  `schemas/parquet_columns.schema.json` is generated from it and drift-checked by
  `tests/test_schemas.cpp` (regenerate with `ZETTA_WRITE_SCHEMAS=1`). See
  [schemas.md](schemas.md).
- `validate_parquet_columns_json(doc)` validates a document against it (the
  embedded pboettch draft-07 validator). The committed golden re-enters the
  program through this check before it is trusted.

## Test + golden (no foreign runtime)

`tests/test_parquet_column_decoder.cpp` decodes every column of
`churn_decode.parquet` and asserts the assembled `{ num_rows, columns }` document
equals the committed golden field for field.

The decode fixture is **separate** from the metadata fixture
(`churn_sample.parquet`) on purpose: `churn_sample` mixes in **zstd + gzip** to
exercise the metadata parser's codec identification, but those codecs have no
native decompressor yet. `churn_decode.parquet` uses only **uncompressed +
snappy** while still spanning every encoding (PLAIN + dictionary), every needed
physical type (int32 / int64 / float / double / boolean / byte_array), a
**REQUIRED** column (no def levels) and an **OPTIONAL** one (def levels, with
nulls), across **2 row groups** (4 + 2 rows, so chunks concatenate). The float32
values are exact in float32 (and the doubles round-trip), so decoded numbers
equal the golden's exactly.

Both artifacts are regenerated by **`scripts/gen-parquet-fixture.py`**, which is
**not** a build or CI dependency (ADR-17): it needs `pyarrow` and is run by hand:

```bash
python3.12 -m venv /tmp/pqvenv && /tmp/pqvenv/bin/pip install pyarrow
/tmp/pqvenv/bin/python scripts/gen-parquet-fixture.py
ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R Schemas   # if the schema changed
```

The negative tests cover the trust boundary: a codec without a decoder yet
(zstd) fails with `codec` in the reason, a truncated file fails rather than
reading out of bounds, and out-of-range row-group / column indices are rejected.
A bonus test decodes `account_id` (snappy + dictionary int64) from the
*independently-built* metadata fixture, cross-checking the decoder against a
different codec/encoding mix.

## How it fits

```
#505 footer/metadata ──┐
                       ├─→ #506 column-chunk decoders (this) ──→ #508 reader → DDF columns
#507 Snappy codec ─────┘
```

The footer parser locates each column chunk's pages; these decoders read +
decompress + decode them into typed values + a validity bitmap; the
[reader → DDF wiring (#508)](https://github.com/oramasearch/zetta/issues/508)
turns those into typed [DDF](ddf.md) columns and removes the
`ChurnDataFormat::Parquet` "not yet supported" error. zstd page decompression is
the sibling codec to add behind the [Snappy](parquet-snappy.md) per-page seam
when a target file needs it.
