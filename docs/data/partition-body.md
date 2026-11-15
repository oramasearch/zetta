# The partition body format — Parquet, for every dataset

A partition body was JSON Lines: one `{"_zid": <id>, "doc": <row>}` object per
line. It is now **Parquet** — the row's fields as columns, the row's stable id in
a `_zid` column beside them — for **every** dataset, not only connector ones
(#833).

One body format buys three things at once:

- **one read path and one write path**, instead of a JSON one and a Parquet one
  that drift;
- **paging** — a body can be read a row group at a time by
  [`ParquetStore`](parquet-store.md) instead of parsed line by line into memory;
- **column projection everywhere** — a near-data featurize that drops 3072
  `embedding_*` columns should not pay to decode them, which is only possible if
  the body is columnar.

`zetta::data` (`include/zetta/data/partition_body.hpp`) owns it:

| Function | What it does |
|---|---|
| `serialize_partition_body(rows)` | rows → bytes + the `format` they ended up in |
| `read_partition_body(bytes, partition_id)` | bytes → rows, sniffing the format |
| `sniff_partition_body(bytes)` | `Parquet` when the bytes carry `PAR1` at both ends, else `Jsonl` |
| `body_extension(format)` | `".parquet"` / `".jsonl"` — the large object's name suffix |

## Reading old bodies

`.jsonl` bodies already exist on running nodes, so the reader **sniffs the bytes**
(`PAR1` at both ends) rather than trusting a name: an old body is read as JSON
Lines, a new one as Parquet, and neither is ever mis-parsed as the other. A body
named `.jsonl` whose bytes are Parquet still reads correctly — the name is a
convenience, the magic is the truth.

The JSON Lines path stays exactly as tolerant as it was (`{_zid, doc}`, or a bare
object read as the row itself with a synthetic `<partition>#<line>` id) and
exactly as strict: a line that is not valid JSON, or not an object, **fails the
read**. A partition silently short of rows diverges from the single-node oracle,
which is the failure mode worth being loud about.

## When Parquet cannot express the rows

The [writer](parquet-writer.md) is flat and scalar-typed. A row holding an array
or an object, or a column mixing strings and numbers, has **no** Parquet schema
that would round-trip it — and a row that already carries its own `_zid` field
would collide with the id column. Such a partition is written as **JSON Lines**,
and says so in `PartitionBodyWriteResult::format`, which the catalog records as
`stats.format`. Coercing would lose data; falling back does not, and the reader
handles both regardless.

This is the one place the "one body format" goal is not absolute, and it is
deliberate: nested-valued datasets keep working, and they become Parquet the day
the writer grows nested schemas.

## What changes about a round trip

Rows written through `write_partitioned` read back **identical** — same ids, same
values, same JSON types (`PartitionWriter.RowsRoundTripThroughTheBodyReadPath`,
`PartitionBody.RoundTripsRowsThroughParquet`).

One representational difference is worth stating plainly. A columnar body has a
column for every field **any** row carries, so a row that **omitted** a field
reads back with that field explicitly `null`. JSON Lines could distinguish
"absent" from "null"; a columnar format cannot. Every consumer here treats them
the same (DDF schema inference drops a non-numeric field either way; the fulltext
indexer indexes neither), and the behavior is pinned by
`PartitionBody.AnOmittedFieldComesBackAsAnExplicitNull` rather than left as a
claim.

## Who reads bodies

Both body consumers go through `read_partition_body`, so neither can drift from
the writer:

- [`load_local_partitions`](../ddf/ddf.md) — a node's owned partitions → a
  `JsonStore` → a DataFrame (compute-near-data).
- [`DistributedFulltextIndex::build_local`](../indexing/distributed-fulltext.md)
  — a node's owned partitions → `IndexDocument`s → its full-text sub-index.

Both currently materialize a whole body's rows, because both then hold every row
anyway. The paging path is `ParquetStore`, which is what a reader that does *not*
want the whole body uses.

## Tests

`tests/data/test_partition_body.cpp` — the round trip (values **and** JSON
types), nulls, an empty body, the omitted-field null, the nested / mixed-type /
own-`_zid` fallbacks (each still lossless), legacy `{_zid, doc}` and bare-object
lines, and every failure carrying the partition's name: a malformed line, a
non-object line, and a `PAR1`-wrapped body of garbage.

`tests/data/test_partition_writer.cpp` — the end-to-end round trip through
`write_partitioned` (every row, its id, its value, in exactly one partition), the
recorded `stats.format`, and the `.parquet` blob name.
