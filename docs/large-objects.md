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
Binary content round-trips exactly (embedded NULs and all). On import the body's
**SHA-256** is computed and recorded.

### On-disk layout

```
data_dir/<db-uuid>/large_objects/
  <lo-uuid>/
    meta.json   { "id", "name", "size", "sha256" }
    blob        raw file bytes
```

`Database::load` ignores `large_objects/` when scanning for tables (it has no
`table.json`); the store loads its objects on construction.

## Replication

Large objects replicate across the cluster, but **split**: the *metadata*
travels in the CRDT, the *body* does not.

- **Metadata in the CRDT.** On import, a `lo:<lo-uuid>` key is captured into the
  replicated document — `{ db, name, size, sha256 }` (see
  [replication.md](replication.md)). It converges like any other catalog entry,
  so every node learns the object exists and what its body must be.
- **Body transferred peer-to-peer.** The bytes are *not* in the document. A node
  that has the metadata but not the body requests it over the cluster
  (`LOREQ`/`LORES`, see [cluster.md](cluster.md)); the sender streams the bytes
  and the receiver **verifies them against the recorded size + SHA-256** before
  writing — a mismatch is dropped, never stored. This keeps large binaries out
  of the CRDT (which would bloat every node's document) while still converging.

So a freshly-replicated object shows up immediately (metadata) and its body
fills in shortly after; until then `LargeObject::body_present` is false and the
UI marks it "syncing body". `read`/`export` return nothing until the body lands.
The `BlobStore` seam (`include/zetta/blob_store.hpp`) is what the cluster uses to
fetch/serve/verify bodies; `ReplicatedBlobStore` implements it purely from the
CRDT metadata + on-disk paths (so it's safe to call from the network thread).

## Limits / future

- Whole-file in memory on import/read/export and per-transfer; streaming /
  chunked transfer can come later. A single body is currently capped by the
  cluster line size (~48 MiB after base64); larger objects need chunking.
- No SQL `lo_*` functions yet (UI + C++ API only).
- Large-object content is not (yet) referenceable from table rows.
