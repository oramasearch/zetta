# Storage

## On-disk layout

Everything a node owns lives under its `data_dir` (default `./zetta-data`,
overridable with `--data-dir`):

```
data_dir/
  replication.automerge      the replicated CRDT document (source of truth)
  <db-uuid>/                 one folder per database (UUIDv4 = its id)
    database.json            { "id": "...", "name": "..." }
    <table-uuid>/            one folder per table (UUIDv4 = its id)
      table.json             { "id": "...", "name": "..." }
      000000.jsonl           rows 0..9999       (one row per line)
      000001.jsonl           rows 10000..19999
      ...                    JsonlStore::kRecordsPerChunk = 10,000 per file
```

The folder name **is** the id; the human name lives only in the metadata file.
So renaming a database rewrites `database.json` and never moves the folder.
`Node::start()` scans `data_dir`, reads each `database.json` / `table.json`, and
rebuilds the in-memory tree (sorted by name for stable display).

Each JSONL line is `{ "_zid": "<row-uuid>", "doc": <row data> }` — the `_zid` is
the row's key in the replicated CRDT (so the same logical row has the same id on
every node; see [replication.md](replication.md)). `doc` is the row itself, which
is what `SELECT` returns. Legacy lines that are just the bare row (no `_zid`) are
still read; they're assigned a fresh id on load.

When a node is replicated, the **CRDT document (`replication.automerge`) is the
source of truth** and this folder tree is a *projection* of it: local writes are
captured into the document and the tree is reconciled from it (so changes from
peers land on disk here too). A standalone (non-replicated) node just reads and
writes the tree directly. See [replication.md](replication.md).

Each database also has a `large_objects/` folder holding stored files as blobs;
see [large-objects.md](large-objects.md). The table scan ignores it (no
`table.json`).

## Storage engines

Rows are held through the `StorageEngine` interface, so a table's backing format
is pluggable:

| Engine | Where | Notes |
|---|---|---|
| `JsonlStore` | `storage/jsonl_store.hpp` | **Default.** JSON Lines on disk, chunked at 10k records/file; caches all records in memory for reads; loads existing chunks on construction. |
| `JsonStore` | `storage/json_store.hpp` | In-memory only (no persistence). Kept for a future `:memory:` mode. |

`add_row` appends a line to the active chunk immediately (append-open per
row — simple and crash-resilient; can hold the stream open as an optimization
later). Corrupt lines are skipped on load. Every row carries a stable id (its
CRDT key): `add(id, doc)` stores it, `contains(id)` lets reconciliation apply a
replicated row exactly once, and `id_at(i)` enumerates rows to replicate them.

To add an engine, implement `StorageEngine` (`name`, `add`, `contains`, `size`,
`at`, `id_at`) and construct it where a `Table` builds its store.

## Preflight integrity check

On `start()` a node runs a **preflight integrity check** by default (turn off with
`NodeConfig::preflight = false` / `--no-preflight`) and logs a `preflight:`
summary. `Node::check_integrity()` is read-only and reports an `IntegrityReport`:

- **Corrupt large-object bodies** — each local body is re-hashed and compared to
  the SHA-256 recorded in its metadata; a mismatch means on-disk corruption.
- **Large-object bodies awaiting transfer** — metadata is present but the body
  isn't here yet (informational — it may still be syncing from a peer).
- **Dangling references** in the replicated catalog — a row whose table is gone,
  a table/large object whose database is gone (orphaned keys the reconciler
  ignores but that shouldn't pile up).

`report.ok()` is true when there's no corruption and no dangling references
(missing bodies don't count). The last report is kept on the node
(`Node::integrity()`) for future surfacing (e.g. a `system.integrity` table).

## Limits / future

- All records are loaded into memory on open; streaming/lazy reads can come
  later behind the same interface.
- No compaction, indexes, or schema enforcement yet.
