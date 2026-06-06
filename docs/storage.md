# Storage

## On-disk layout

Everything a node owns lives under its `data_dir` (default `./zetta-data`,
overridable with `--data-dir`):

```
data_dir/
  <db-uuid>/                 one folder per database (UUIDv4 = its id)
    database.json            { "id": "...", "name": "..." }
    <table-uuid>/            one folder per table (UUIDv4 = its id)
      table.json             { "id": "...", "name": "..." }
      000000.jsonl           records 0..9999      (one JSON document per line)
      000001.jsonl           records 10000..19999
      ...                    JsonlStore::kRecordsPerChunk = 10,000 per file
```

The folder name **is** the id; the human name lives only in the metadata file.
So renaming a database rewrites `database.json` and never moves the folder.
`Node::start()` scans `data_dir`, reads each `database.json` / `table.json`, and
rebuilds the in-memory tree (sorted by name for stable display).

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
record — simple and crash-resilient; can hold the stream open as an
optimization later). Corrupt lines are skipped on load.

To add an engine, implement `StorageEngine` (`name`, `add`, `size`, `at`) and
construct it where a `Table` builds its store.

## Limits / future

- All records are loaded into memory on open; streaming/lazy reads can come
  later behind the same interface.
- No compaction, indexes, or schema enforcement yet.
