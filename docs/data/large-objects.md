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

`zetta::data::import_records(Table&, path)`:

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
| `import_bytes(name, bytes)` | `lo_from_bytea` | stores in-memory bytes as a new object (no source file) — same fingerprint + CRDT capture as `import_file`; used by the [partitioned write path](partitioned-write-path.md) to store a partition's serialized rows |
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
  [replication.md](../cluster/replication.md)). It converges like any other catalog entry,
  so every node learns the object exists and what its body must be.
- **Body transferred peer-to-peer.** The bytes are *not* in the document. A node
  that has the metadata but not the body requests it over the cluster
  (`LOREQ`/`LORES`, see [cluster.md](../cluster/cluster.md)); the sender streams the bytes
  and the receiver **verifies them against the recorded size + SHA-256** before
  writing — a mismatch is dropped, never stored. This keeps large binaries out
  of the CRDT (which would bloat every node's document) while still converging.

So a freshly-replicated object shows up immediately (metadata) and its body
fills in shortly after; until then `LargeObject::body_present` is false and the
UI marks it "syncing body". `read`/`export` return nothing until the body lands.
The `BlobStore` interface (`include/zetta/data/blob_store.hpp`) is what the cluster uses to
fetch/serve/verify bodies; `ReplicatedBlobStore` implements it purely from the
CRDT metadata + on-disk paths (so it's safe to call from the network thread).

- **Partition bodies place on their owners, not everyone.** The above is
  replicate-to-all — every node fetches every body. A body that is the backing
  blob of a [dataset partition](dataset-partitions.md) on a
  [`Partitioned` / `PartitionedWithReplicas`](replication-strategy.md) table is the
  exception: `ReplicatedBlobStore` fetches and holds it **only on the partition's
  owner node(s)** (its `owner_nodes`, with `rf` owners → exactly `rf` copies), so a
  non-owner never `LOREQ`s it. Plain large objects and model weights are unaffected.
  See [sharded-blob-placement.md](sharded-blob-placement.md).

## Exposure — SQL read view + web routes

- **`system.large_objects`** — a RULE-7 virtual read view (`executor.cpp`,
  `large_object_rows`) computed live from each database's `LargeObjectStore.list()`:
  one row per object across every database with `object_id`, `name`, `database`,
  `size_bytes`, `sha256`, `body_present`. Filterable, e.g.
  `SELECT * FROM system.large_objects WHERE database = 'shop'`.
- **Web dashboard routes** (`src/core/api/http_server.cpp`, cookie-authed like the rest of
  the web UI; the Data explorer's **Files** panel drives them). The session user must hold
  the right for each, and every decision is audited (#1389; see
  [access-control.md](../governance/access-control.md)):
  - `POST /blob?db=<database>&name=<filename>` — the raw request body IS the file bytes
    (no multipart); stores it via `LargeObjectStore::import_bytes`. Returns `{ok,id,name,size}`.
    Needs `sql:write` on the database.
  - `GET /blob/download?db=<database>&id=<object_id>` — streams the body back as an
    `attachment` (`Content-Disposition`), reading via `LargeObjectStore::read` (404 until the
    body has synced to this node). Needs `blob:read` covering the id (403 otherwise).
  - `POST /blob/delete?db=<database>&id=<object_id>` — `LargeObjectStore::unlink`. Needs
    `sql:write` on the database.

## Limits / future

- Whole-file in memory on import/read/export and per-transfer (the web upload/download
  routes buffer the whole body too); streaming / chunked transfer can come later. A single
  body is currently capped by the cluster line size (~48 MiB after base64); larger objects
  need chunking.
- No SQL `lo_*` mutation functions yet — `system.large_objects` is read-only; create/delete go
  through the C++ API or the web `/blob` routes above.
- Large-object content is not (yet) referenceable from table rows.
