# ParquetStore — a storage engine that pages off disk

`JsonlStore` reads every row into memory when it opens. That is fine for a table
a node owns and impossible for a partition body of a connector result set, which
is exactly the data #833 says must **not** be memory-resident.

`zetta::storage::ParquetStore` (`include/zetta/storage/parquet_store.hpp`) is the
`StorageEngine` over a Parquet body: it opens the file by its **footer only**,
decodes **one row group at a time** on demand, bounds the decoded pages by a byte
budget, and hands rows back as [`ParquetLazyPtr`](lazy-rows.md) handles that
survive the cache reclaiming the page they came from.

```cpp
const auto opened = zetta::storage::ParquetStore::open(path);
if (!opened.ok()) { /* opened.error says why */ }
for (std::size_t i = 0; i < opened.store->size(); ++i) {
    const auto row = opened.store->at(i);
    if (!row) { /* row.error(): a page read failed — data loss, not absence */ }
    use(*row, row.id());
}
```

## What it holds, and what it doesn't

| Resident for the store's life | Paged |
|---|---|
| the footer (schema + row-group layout) | **row data** — one row group at a time |
| the row **ids** + the id→index map | |

Ids are resident deliberately: `contains(id)` and `id_at(i)` are asked *without*
wanting the row, and answering either from a page would mean decoding row data to
look up a key. That is a fixed cost proportional to the row **count** (a string
per row), not to the row **size** — the 3072-column embedding body pages exactly
like a two-column one.

A body without an id column (any foreign `.parquet` file) gets ids synthesized as
`<file name>#<row index>` — stable, because a body is immutable.

## Opening by the footer alone

`parse_parquet_metadata` wants the whole file's bytes because it walks back from
the trailing magic. Reading a multi-gigabyte body to learn its layout would
defeat the point, so the store reads the last 8 bytes (footer length + `PAR1`),
then the footer itself, and hands the parser `"PAR1" + <tail>`: a buffer whose
*end* is the file's end and whose start carries the leading magic. Every offset
in the parsed footer is absolute in the real file, which is all the pager needs.

## Paging: one row group is one page

A row group's column chunks are contiguous, so a page is one `pread` of
`[min(chunk offset), max(chunk end))`. The decoders address a chunk by absolute
file offset, so the pager decodes the span it just read against a **metadata copy
whose offsets are rebased** to the start of that buffer — the same
`decode_column_chunk` the whole-file reader uses, pointed at a window of the file
instead of all of it. Column projection falls out of this: the id column is
skipped when it is the key rather than data, and a future projected read skips
whatever the caller did not ask for.

## The cache, and what "reaping" means

One slot per row group:

- **`strong`** — the cache's own `shared_ptr`. Dropping it *is* reaping.
- **`weak`** — survives the drop, so a page a caller still holds through a handle
  is **re-adopted** on the next read instead of decoded a second time.
- **`last_used`** — the eviction order.

An insert that pushes the resident total past `page_cache_bytes` evicts
least-recently-used pages (never the one just inserted) until it fits. A budget
smaller than a single page still works: each page is decoded, used, and reaped, so
a scan costs one page of memory rather than one dataset.

`decode_count()` counts only real decodes — a re-adopted page does not count,
which is what makes "no duplicate decode" assertable rather than assumed.

> **Deviation from the design, recorded deliberately.** #833 calls for
> `std::atomic<std::shared_ptr<Page>>` cache slots so a resident page can be taken
> without a lock. **libc++ does not implement that specialization** (P0718 — the
> primary template static-asserts on trivially copyable), and the C++20
> `atomic_load(&shared_ptr)` free functions are *removed* in C++26. The slots are
> guarded by the pager's mutex instead. The guarantee is identical — a reader sees
> the old page or the new one, never a torn pointer — and what is lost is the
> lock-free fast path, a one-line change the day libc++ ships the specialization.
> Page decoding currently happens under that mutex, so concurrent readers of
> *different* pages serialize; that is a throughput limit, not a correctness one.

## Mutation: refused, not ignored

A Parquet body is an immutable artifact — a partition body written once by
`write_partitioned`, or a file loaded as a dataset. `add`, `update` and `remove`
are **refused**: each records a path-qualified reason in `last_mutation_error()`,
counts in `rejected_mutations()`, and writes the reason to stderr. Changing a
dataset stored this way means writing a new body.

They are refused rather than reported through a status because `StorageEngine`'s
mutators return `void`, and widening them to a status is a change to every write
path in the engine — out of scope for this slice and not needed by anything that
holds a `ParquetStore` today. What matters is that a caller cannot mistake a
refusal for a write that happened: nothing is silent.

## Tests

`tests/data/test_parquet_store.cpp` writes bodies with the
[Parquet writer](parquet-writer.md) (small row groups, to force many pages) and
asserts:

- rows, ids, `contains`, and a past-the-end read returning a **failed handle**;
- the id column hidden by default, kept on request, and synthesized when absent;
- **a handle read after `reap()`** — the property the handle design exists for;
- **one decode for two rows of the same page**, including across a reap
  (re-adoption through the `weak_ptr`);
- a released handle re-reading the page (the decode counter proves the re-read);
- a **4000-row / 80-page scan under a 4 KiB budget**: one resident page at a
  time, every page decoded exactly once, the sum still correct;
- every mutation refused, counted, and leaving the body untouched;
- a missing / malformed / too-small body failing to open with a reason, and a
  body truncated under a live store returning a failed handle — while `id_at`
  still answers, so **data loss stays distinguishable from absence**.

## Where it fits

This is the read side of "partition bodies become Parquet"
([partitioned-write-path.md](partitioned-write-path.md)): one body format, one
read path, one write path. The scatter-gather `SELECT` over partition-only tables
and fetch-affinity placement (#833 sections 4–7) build on a store that can read a
partition body without loading it.
