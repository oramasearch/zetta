# zetta-dev — reference (codebase map, recipes, gotchas)

Loaded on demand from [SKILL.md](SKILL.md). This is a navigation aid: where each
subsystem lives, how to extend it, and the load-bearing details that are easy to
break. `CLAUDE.md` and `docs/*.md` remain authoritative for behavior.

## Subsystem map

| Subsystem | Key files | Doc | Seam / invariant |
|---|---|---|---|
| Run modes / wiring | `src/main.cpp`, `include/zetta/headless.hpp`, `src/core/headless.cpp` | architecture.md | main builds `ReplicationManager` → `Node` → `ReplicatedBlobStore` → `Cluster`, then dispatches headed (`gui::App`) or headless (`run_headless`). One UUID = node id = actor id. |
| Integrity | `Node::check_integrity()` → `IntegrityReport` (node.hpp), `sha256.hpp` | storage.md | Preflight on `start()` (default; `--no-preflight`): re-hash local LO bodies vs recorded sha256, flag dangling catalog refs. `report.ok()`. |
| Data model | `node`, `database`, `table` (`.hpp`/`src/core/*.cpp`) | architecture.md, storage.md | `Node`→`Database`→`Table`→`StorageEngine`. shared_ptr handles, empty on miss. UUIDv4 folder = id; name in `*.json`. |
| Storage engine | `include/zetta/storage/{engine,jsonl_store,json_store}.hpp`, `src/core/jsonl_store.cpp` | storage.md | `StorageEngine` interface: `add(id,doc)`/`contains`/`size`/`at`/`id_at`. Rows carry a UUID (the CRDT key). JSONL line = `{"_zid":id,"doc":row}`. |
| Large objects | `large_object.hpp`, `src/core/large_object.cpp`, `blob_store.hpp`, `replicated_blob_store.hpp`, `src/core/replicated_blob_store.cpp`, `sha256.hpp` | large-objects.md | Metadata (`lo:` key, incl. `size`+`sha256`) replicates in the CRDT; **body does not** — it transfers P2P and is verified against the hash. `BlobStore` is the transfer seam. |
| Query layer | `include/zetta/query/{ast,dialect,executor,postgres,service}.hpp`, `src/core/query/*.cpp` | query.md | text → `Dialect.parse()` → generic `Statement` variant → `Executor`(Node). Executor is dialect-neutral. Virtual read-only **`system`** database holds computed system tables (`system.zetta_nodes`); `system` is reserved. |
| Import | `import.hpp`, `src/core/import.cpp` | large-objects.md | `.json`/`.jsonl` → rows via `Table::add_row` (so it replicates). |
| Cluster | `cluster.hpp`, `src/core/cluster.cpp` (the one Corosio user) | cluster.md | Equal peers over TCP; line protocol `HELLO/SYNC/STATUS/LOREQ/LORES`. Self-healing (keepalive + reconnect + replace-by-address). `nodes()` = status view. |
| Replication (CRDT) | `replication.hpp`, `src/core/replication.cpp` (the one automerge-cpp user), `src/core/catalog.hpp` (internal key scheme) | replication.md | `ReplicationManager`: `set`/`remove`/`snapshot`/`save`/`merge`/`entry_count`/`version`. Document = source of truth. Thread-safe (mutex). |
| Async/net seams | `runtime.hpp`, `runtime/net.hpp` | runtime.md, research/coroutines.md | `zetta::runtime` (Capy) + `zetta::runtime::net` (Corosio). Backend names confined to adapters. |
| GUI | `src/gui/app.hpp` (forward-declares SDL/ImGui), `src/gui/app.cpp` | architecture.md | Thin renderer over a `Node`+`Cluster`. Only `app.cpp` pulls heavy headers. |
| Utilities | `uuid.hpp`, `sha256.hpp`, `bytes.hpp`, `duration.hpp`, `signals.hpp`, `version.hpp` | — | `new_uuid`, `sha256_hex`, `format_bytes`, `format_duration`. |

## The replicated catalog (CRDT key scheme)

Defined in `src/core/catalog.hpp` (internal). Flat top-level keys, structured
values — each row/object owns its own key by UUID so concurrent edits never
collide; the UUID is also the on-disk folder/row id (identical on every node):

```
node:<node_id>  -> { address }                     cluster membership
db:<db_uuid>    -> { name }                         a database
tbl:<tbl_uuid>  -> { db, name }                     a table
row:<row_uuid>  -> { tbl, data:{…} }                a row
lo:<lo_uuid>    -> { db, name, size, sha256 }       large-object metadata (no body)
```

`Node::reconcile_from_replication()` decodes these into desired state and
materializes the on-disk projection; `migrate_to_replication()` captures
local-only data into the document on startup; persisted to
`data_dir/replication.automerge`.

## How-to recipes

**Add a storage engine** — implement `StorageEngine` (`name`, `add(id,doc)`,
`contains`, `size`, `at`, `id_at`) and construct it where `Table` builds its
store (`src/core/table.cpp`). Keep the row-id contract so reconcile stays
idempotent. Tests in `tests/test_jsonl_store.cpp`.

**Add a query dialect** — implement `zetta::query::Dialect` (`name()`+`parse()`)
producing the existing `Statement`s; construct it in `QueryService`. **Don't
touch the `Executor`.** Model on `src/core/query/postgres.cpp`. (A non-SQL input
*language* → the **new-input** skill instead.)

**Replicate a new kind of state** — add a key prefix + value shape to
`src/core/catalog.hpp`; **capture** it at the owning model object (give that
object the `ReplicationManager*`, call `set`/`remove`); **reconcile** it in
`Node::reconcile_from_replication()` (+ decode, + a `materialize_*` path that
does NOT re-capture); **migrate** it in `Node::migrate_to_replication()`. Add a
two-node convergence test (see `tests/test_data_replication.cpp`).

**Add a cluster protocol message** — add a `ZETTA/1 <VERB> …` line builder +
handle it in `apply_gossip` (read pump) and/or `gossip_writer` (write pump) in
`src/core/cluster.cpp`. The read pump must NOT write to the socket directly
(only one writer) — queue replies on `GossipLink::outbound` for the writer to
drain. Big payloads use base64 in one line (bounded by `kMaxLineBytes`).

**Transfer bytes peer-to-peer** — model on large objects: metadata in the CRDT,
bytes through a `BlobStore`-style seam that works off the CRDT + disk paths only
(so it's network-thread-safe), verified by hash before write.

**Add a test** — new file in `tests/`, then add it to the explicit list in
`tests/CMakeLists.txt` (it is NOT globbed). Use `tests/temp_dir.hpp`. Multi-node
tests: short `ClusterConfig` timeouts + patient polling.

## Gotchas / load-bearing details

- **GUI scaling (`src/gui/app.cpp`)** — uses `SDL_GetDisplayContentScale` (NOT
  `SDL_GetWindowDisplayScale`, which double-scales on Retina), and calls
  `SDL_SetRenderScale` before clearing (the SDLRenderer3 backend doesn't, so the
  UI would paint only the top-left quadrant). Re-rasterizes the font on scale
  change. Don't "simplify" these.
- **Cluster threading** — reader + writer coroutines per connection run on the
  **single** io_context thread (cooperative, never parallel), so per-connection
  state (`GossipLink`) needs no lock. Cross-thread state (`State::peers`) is
  mutex-guarded. The network thread must not touch the `Node` tree.
- **Line framing** — `read_line` carries a **persistent** per-connection buffer;
  one `read_some` routinely returns several `\n`-delimited lines coalesced
  (Nagle/MTU on a real link), so leftover bytes after the first newline MUST be
  kept for the next call. Dropping them silently loses every line after the first
  — a bug that only shows up cross-machine (loopback delivers each line in its
  own read). Don't reintroduce a fresh-buffer-per-call `read_line`.
- **Keepalive needs traffic** — the idle-timeout watchdog only runs when the
  writer runs (i.e. `replication`/`blobs` wired). Gossip every ~0.5s keeps a
  healthy link's `last_activity` fresh; silence past the timeout drops it.
- **Replication persistence cadence** — the whole document is re-saved on each
  version change, and reconcile rescans on each version bump (fine for modest
  data; incremental save / per-table versioning is a known future optimization).
- **Large-object body size** — one base64 line, capped by `kMaxLineBytes`
  (~48 MiB body); bigger needs chunked transfer (future).
- **Headed Ctrl-C** — SDL3 on macOS doesn't turn terminal SIGINT into a quit;
  close the window / Cmd+Q. Only headless installs signal handlers.
- **Font** — DroidSans.ttf is baked into a header by `cmake/embed_font.cmake`;
  no runtime asset dependency.

## Docs index (`docs/`)

- **architecture.md** — node, run modes, data model, module map.
- **storage.md** — on-disk layout, JSONL `_zid`/`doc` line format, engine seam.
- **query.md** — dialect-agnostic layer, Postgres subset, the `system` database (`system.zetta_nodes`).
- **large-objects.md** — LO API, metadata-in-CRDT + body-P2P-with-hash, import.
- **runtime.md** — `zetta::runtime` async seam + migration plan.
- **cluster.md** — equal peers, reachable-address banner, self-healing links, status view.
- **replication.md** — CRDT, "everything is replicated", consistency model + leadership (design), data replication.
- **build.md** — build/run/test, compilers, submodules.
- **research/coroutines.md** — coroutine-runtime design deep-dive (background).

## Sibling skills

- **new-input** — add a new input language/format (parser + executor that
  re-implements its behavior in zetta's runtime; e.g. Python `load_dataset`).
- **skill-writer** — author/update skills (progressive-disclosure structure).
- **perplexity** — generate a long deep-dive research query from a topic.
