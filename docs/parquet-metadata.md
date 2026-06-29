# Parquet footer / metadata parser

`zetta::parquet::parse_parquet_metadata` is the native, read-only **Parquet
footer parser** — it reads a `.parquet` file's Thrift `FileMetaData` (the
schema, row groups, and per-column-chunk layout) **without Apache Arrow or
parquet-cpp**. It is the layout foundation the data decoders consume; it decodes
**no data** itself.

- **Code:** `include/zetta/parquet/metadata.hpp`, `src/core/parquet/metadata.cpp`
  (namespace `zetta::parquet`, in `zetta-core`).
- **Test:** `tests/test_parquet_metadata.cpp`.
- **Fixture + golden:** `tests/fixtures/parquet/churn_sample.parquet` +
  `tests/fixtures/parquet/churn_sample.metadata.json`.
- **Issue / ADR:** #505, realizing the footer slice of **ADR-32** (#481) — a
  hand-rolled Parquet reader, no foreign runtime (refines **ADR-17**).

## Why hand-rolled

ADR-32 decides zetta reads Parquet with its own from-scratch native code (the
same ethos as the hand-written Automerge CRDT and Snappy decoder), rather than
vendoring Arrow / parquet-cpp. ADR-17 forbids foreign runtimes in the engine,
the build, and CI. So the reader covers exactly what the target datasets use,
nothing more, and the only "reference implementation" (pyarrow) is used **once at
dev time** to author the test golden — never at build or CI time.

This is the **first of four** Parquet slices, all consuming this layout:

1. **#505 (this)** — footer / metadata parser (schema + row-group / column-chunk
   layout).
2. **#507** — Snappy page decompression (`zetta::parquet::snappy_decompress`).
3. **#506** — column-chunk decoders (PLAIN + dictionary / RLE).
4. **#508** — reader → DDF columns with projection; removes the
   `ChurnDataFormat::Parquet` "not yet supported" error.

## The file layout it reads

A Parquet file ends with a self-describing footer:

```
PAR1                       4-byte leading magic
... data pages, dictionary pages, page headers ...
<Thrift FileMetaData>      the layout description (Thrift compact protocol)
<footer length>            4-byte little-endian u32 = byte length of the
                           FileMetaData blob immediately before it
PAR1                       4-byte trailing magic
```

The parser checks the magic, reads the trailing length, slices out the
`FileMetaData` blob, and decodes it. The metadata is serialized with the **Thrift
compact protocol** (varint field-id deltas, zigzag varints for ints,
length-prefixed binary, a compact list/map framing), so the reader carries a
small, self-contained compact-protocol decoder. It dispatches on the
parquet.thrift field ids it models and **skips** everything else (statistics,
logical types, column orders, encryption metadata, …), so a file that carries
fields this slice does not need still parses.

## Usage

`parse_parquet_metadata` takes the **whole file's bytes** (it needs the trailing
magic + length) and returns a `ParquetMetadataResult` — non-throwing, like every
zetta-core boundary:

```cpp
#include "zetta/parquet/metadata.hpp"

std::string bytes = read_file("accounts.parquet");      // whole file
auto result = zetta::parquet::parse_parquet_metadata(bytes);
if (!result.ok()) { /* result.error explains why */ }

const zetta::parquet::FileMetaData& meta = result.metadata;
meta.num_rows;                       // total rows across all row groups
meta.created_by;                     // writer string
for (const auto& column : meta.leaf_columns()) {
    column.path;                     // dotted column path, e.g. "account_id"
    column.type;                     // PhysicalType (Int64, Double, ByteArray, …)
}
for (const auto& group : meta.row_groups) {
    group.num_rows;
    for (const auto& chunk : group.columns) {
        chunk.path();                // dotted path
        chunk.codec;                 // CompressionCodec (Snappy, Zstd, …)
        chunk.encodings;             // vector<Encoding> (Plain, RleDictionary, …)
        chunk.data_page_offset;      // byte offset of the first data page
        chunk.has_dictionary_page;   // and chunk.dictionary_page_offset when true
        chunk.total_compressed_size; // bytes on disk for the chunk
        chunk.total_uncompressed_size;
    }
}
```

`FileMetaData::schema` holds the full flattened `SchemaElement` tree (root group
first, depth-first preorder) faithfully; `leaf_columns()` is the data-bearing
projection (with dotted paths) most callers want.

### Scope (this slice)

- **Reads the footer only** — no page bodies, no decompression, no values.
- **Unencrypted files** — an encrypted footer (`PARE` magic) is rejected with a
  clear reason, not misparsed.
- Forward-compatible: unknown fields/types are skipped, and unknown enum codes
  map to `…::Unknown` rather than failing.

## Boundary validation (schema as code)

The reader's binary input is bounds-checked at every read (the binary-format
analog of the schema-at-the-boundary rule — Thrift is binary, so there is no
JSON schema for the file itself). The **JSON projection** `to_json` emits *does*
have a schema:

- `parquet_metadata_schema()` is the draft-07 schema, the **single source of
  truth**; `schemas/parquet_metadata.schema.json` is generated from it and
  drift-checked by `tests/test_schemas.cpp` (regenerate with
  `ZETTA_WRITE_SCHEMAS=1`). See [schemas.md](schemas.md).
- `validate_parquet_metadata_json(doc)` validates a document against it (the
  embedded pboettch draft-07 validator), returning `""` or the first
  path-qualified failure. The committed golden re-enters the program through this
  check before it is trusted.

## Test + golden (no foreign runtime)

`tests/test_parquet_metadata.cpp` parses the committed fixture and asserts its
`to_json` form equals the committed **pyarrow golden** field for field — an
*independent* reference for the schema + row-group / column-chunk layout. The
fixture is small but representative: 5 columns spanning `int64` / `double` /
`boolean` / `byte_array`, 4 codecs (`snappy` / `zstd` / `uncompressed` / `gzip`),
dictionary-encoded and non-dictionary columns, across 2 row groups.

Both artifacts are regenerated by **`scripts/gen-parquet-fixture.py`**, which is
**not** a build or CI dependency (ADR-17): it needs `pyarrow` and is run by hand:

```bash
python3.12 -m venv /tmp/pqvenv && /tmp/pqvenv/bin/pip install pyarrow
/tmp/pqvenv/bin/python scripts/gen-parquet-fixture.py
ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R Schemas   # if the schema changed
```

The negative tests cover the trust boundary: empty / too-small input, wrong
magic, a corrupted footer length that would overrun the file, and an encrypted
(`PARE`) footer — each fails with a reason rather than crashing.
