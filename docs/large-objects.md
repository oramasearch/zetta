# Large objects & file import

zetta ingests files two ways, both reachable from a **native file dialog** and
by **dragging a file onto the window**:

| Kind | What it does | Where it goes |
|---|---|---|
| **Record import** | A `.json` / `.jsonl` file becomes table rows | the selected table |
| **Large object** | Any file is stored as a binary blob | the selected database |

Drag-and-drop routes by extension: `.json` / `.jsonl` → record import into the
selected table; anything else → stored as a large object in the selected
database. The dialog buttons are explicit ("Import JSON/JSONL…" in the Rows
panel, "Store file…" in the Large Objects panel).

## Record import

`zetta::import_records(Table&, path)`:

- `.jsonl` — one JSON document per line (blank lines skipped).
- `.json` (or any other extension) — a JSON **array** (each element a row) or a
  single JSON value (one row).
- Duplicates are allowed; nothing is de-duplicated.
- All-or-nothing: the file is fully parsed before any row is added, so a parse
  error imports nothing (and reports the offending line for `.jsonl`).

## Large objects

Modeled on PostgreSQL's large object API. A `LargeObjectStore` belongs to a
`Database` (`db.large_objects()`):

| zetta | Postgres analog | Notes |
|---|---|---|
| `import_file(path)` | `lo_import` | reads the file, assigns a UUID id, returns metadata |
| `export_file(id, path)` | `lo_export` | writes the blob to a path |
| `read(id)` | `lo_get` / `loread` | returns the raw bytes |
| `unlink(id)` | `lo_unlink` | deletes the object |
| `list()` | — | metadata for all objects, sorted by name |

Ids are UUIDv4 (consistent with databases/tables) rather than Postgres OIDs.
Binary content round-trips exactly (embedded NULs and all).

### On-disk layout

```
data_dir/<db-uuid>/large_objects/
  <lo-uuid>/
    meta.json   { "id", "name", "size" }
    blob        raw file bytes
```

`Database::load` ignores `large_objects/` when scanning for tables (it has no
`table.json`); the store loads its objects on construction.

## Limits / future

- Whole-file in memory on import/read/export; streaming can come later.
- No SQL `lo_*` functions yet (UI + C++ API only).
- Large-object content is not (yet) referenceable from table rows.
