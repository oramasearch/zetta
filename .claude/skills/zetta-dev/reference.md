# zetta-dev — reference (codebase map, recipes, gotchas)

Loaded on demand from [SKILL.md](SKILL.md). This is a navigation aid: where each
subsystem lives, how to extend it, and the load-bearing details that are easy to
break. `CLAUDE.md` and `docs/*.md` remain authoritative for behavior.

## Subsystem map

| Subsystem | Key files | Doc | Boundary / invariant |
|---|---|---|---|
| Run modes / wiring | `src/main.cpp`, `include/zetta/headless.hpp`, `src/core/headless.cpp` | architecture/architecture.md | main builds `ReplicationManager` → `Node` → `ReplicatedBlobStore` → `Cluster`, then dispatches headed (`gui::App`) or headless (`run_headless`). One UUID = node id = actor id. |
| Integrity | `Node::check_integrity()` → `IntegrityReport` (node.hpp), `sha256.hpp` | data/storage.md | Preflight on `start()` (default; `--no-preflight`): re-hash local LO bodies vs recorded sha256, flag dangling catalog refs. `report.ok()`. |
| Data model | `node`, `database`, `table` (`.hpp`/`src/core/*.cpp`) | architecture/architecture.md, data/storage.md | `Node`→`Database`→`Table`→`StorageEngine`. shared_ptr handles, empty on miss. UUIDv4 folder = id; name in `*.json`. |
| Projects | `project.hpp`, `src/core/pipeline/project.cpp`, `timestamp.hpp` | pipeline/projects.md | `Node` CRUD (`create/find/drop/rename_project`, `project_names`) mirrors databases. Top-level entity (no parent): id+name+created/updated+owner. Owns inputs. Replicated (`project:` key) + `<uuid>/project.json` projection. |
| Project inputs | `project_input.hpp`, `src/core/pipeline/project_input.cpp`, `sha256.hpp`, `timestamp.hpp` | pipeline/project-inputs.md | A `Project` owns inputs like a `Database` owns tables. `create_input/find_input/drop_input` (immutable — no rename). Metadata (`input:` key, incl. `content_hash`+`size`) replicates; **payload body does not** — on disk at `<project>/inputs/<id>/payload`, P2P transfer is a later slice. `content_hash` computed at boundary. |
| Deployments | `deployment_target.hpp`, `deployment.hpp`, `src/core/deployment{,_target}.cpp`, `ir/execution_graph.hpp` | pipeline/deployments.md | `Node::deploy_graph(graph, target, name)` places a validated graph onto a `DeploymentTarget` (ENT-6; R1 `Local` only) + `find_deployment`/`deployments`. Top-level entity: graph_name+target+node+counts+created (graph recorded as a summary). Replicated (`deployment:` key) + `<uuid>/deployment.json`. Queryable via `system.deployments`. `Node::node_id()` = actor id (or `data_dir/node_id` standalone). |
| Runs | `run.hpp`, `src/core/pipeline/run.cpp`, `timestamp.hpp` | pipeline/runs.md | One graph execution: status/logs/outputs/lineage (+ optional project/graph refs). Top-level entity, **keyed by id** (no name): `Node` `create_run/find_run/drop_run/runs`; "update" = handle mutators (`set_status/add_log/set_outputs/set_lineage`). Replicated (`run:` key) + `<uuid>/run.json`. **Validated against a JSON Schema** (`parse_run_value`) at both boundaries (disk + CRDT) via pboettch/json-schema-validator (PRIVATE to zetta-core, `-fexperimental-library` to match ABI). |
| Models | `model.hpp`, `src/core/ml/model.cpp`, `replicated_blob_store.{hpp,cpp}`, `sha256.hpp` | ml/models.md | A trained model (ENT-14/FEAT-6): metadata (name/kind/feature_schema/metrics/created + optional project/run refs). Top-level, **keyed by id**: `Node` `create_model`(requires non-empty `kind`)/`find_model/drop_model/models`; handle mutators `set_weights/set_metrics`. Replicated metadata (`model:` key) + `<uuid>/model.json`; **serialized weights body NOT in CRDT** — on disk `<uuid>/weights`, transfers P2P over the **same LO body path** (`ReplicatedBlobStore` resolves `model:` weights too), verified vs `weights:{size,sha256}`. **JSON-Schema validated** (`parse_model_value`). Registry = `Node::models()` + `system.models`. |
| Access control | `principal.hpp`, `access_key.hpp`, `scope.hpp`, `src/core/{principal,access_key,scope}.cpp` | governance/access-control.md | `Principal` (kind/name) owns immutable `AccessKey`s, each carrying a deny-by-default `Scope` of `Resource`s. `Node::authorize(key_id, resource)` is the authorization boundary (deny on unknown key); `zetta::governance::authorize(key, resource)` is the pure decision. Replicated (`principal:`/`key:` keys) + `<uuid>/principal.json` + `keys/<id>/key.json`. RULE-3 / ENT-20/21. |
| Audit trail | `audit_event.hpp`, `src/core/governance/audit_event.cpp`, `timestamp.hpp` | governance/audit-trail.md | Immutable `AuditEvent` records (action/actor/target/allowed/detail/time plus optional read summary). Registry writes, denials and failures are per-call; selected successful polling reads use actor/capability/scope windows. `Node` is the `AuditSink`, window owner and retention scheduler. Replicated as `audit:` keys; locally projected into checksummed compressed segments with a compact id index and bounded recent window. Legacy per-event JSON migrates on startup. Old detail expires by policy while summaries remain. **JSON-Schema validated**. ENT-23 / FEAT-9 / #1154. |
| Storage engine | `include/zetta/storage/{engine,jsonl_store,json_store}.hpp`, `src/core/data/jsonl_store.cpp` | data/storage.md | `StorageEngine` interface: `add(id,doc)`/`contains`/`size`/`at`/`id_at`. Rows carry a UUID (the CRDT key). JSONL line = `{"_zid":id,"doc":row}`. |
| Large objects | `large_object.hpp`, `src/core/data/large_object.cpp`, `blob_store.hpp`, `replicated_blob_store.hpp`, `src/core/data/replicated_blob_store.cpp`, `sha256.hpp` | data/large-objects.md | Metadata (`lo:` key, incl. `size`+`sha256`) replicates in the CRDT; **body does not** — it transfers P2P and is verified against the hash. `BlobStore` is the transfer interface (`ReplicatedBlobStore` also serves model `weights` bodies — see Models). |
| Query layer | `include/zetta/query/{ast,dialect,executor,postgres,service}.hpp`, `src/core/query/*.cpp` | data/query.md | text → `Dialect.parse()` → generic `Statement` variant → `Executor`(Node). Executor is dialect-neutral. `WHERE` is a single comparison predicate (`=`/`!=`/`<>`/`<`/`<=`/`>`/`>=`); a `SELECT … GROUP BY` routes through the **DDF engine** (`ddf::scan`/`filter`/`group_by`) and materializes back to rows (#386, numeric only). Virtual read-only **`system`** database holds computed system tables (`system.nodes`, `system.deployments`, `system.models`, `system.jobs`); `system` is reserved. |
| Import | `import.hpp`, `src/core/data/import.cpp` | data/large-objects.md | `.json`/`.jsonl` → rows via `Table::add_row` (so it replicates). |
| n8n import | `n8n_import.hpp`, `src/core/pipeline/n8n_import.cpp`, `project.hpp` (`create_input`) | pipeline/n8n-import.md | `import_n8n_workflow`/`import_n8n_workflow_file` → artifact `ProjectInput`. **Schema-validated at boundary** (`validate_n8n_workflow`/`n8n_workflow_schema`, draft-07: requires `nodes` array of `{name,type}` objects + `connections` object; extras allowed) via pboettch/json-schema-validator (model on run.cpp). Stores raw bytes byte-for-byte (round-trips); SHA-256 via `create_input`. Storage+validation only — compile is a separate step; not GUI-wired. |
| n8n compile | `n8n_compile.hpp`, `src/core/pipeline/n8n_compile.cpp` | pipeline/n8n-compile.md | `compile_n8n_workflow(content / ProjectInput)` → `ir::ExecutionGraph` (#308). Re-uses `validate_n8n_workflow` boundary, then maps nodes→typed steps (`n8n_step_kind` table, keyed by node **name**) + connections→`DataFlow` edges. Non-throwing (`optional` + path-qualified reason) like `ir::from_json`; unmapped node type fails. In-memory IR only (no persist/GUI yet). |
| Cluster | `cluster.hpp`, `src/core/cluster/cluster.cpp` (the one Corosio user) | cluster/cluster.md | Equal peers over TCP; line protocol `HELLO/SYNC/STATUS/LOREQ/LORES`. Self-healing (keepalive + reconnect + replace-by-address). `nodes()` = status view. Mutual TLS 1.3 per link when `ClusterConfig::security` is enabled (#466). |
| Cluster security | `cluster/security.hpp`, `src/core/cluster/security.cpp` (the one OpenSSL-libcrypto user) | cluster/security.md | CTL-2/TB-3 (#466): join token → deterministic Ed25519 cluster CA → per-node CA-signed identity; `load_or_create_cluster_security` persists under `<data_dir>/security/` (schema-validated secret file). Token-gated join enforced at the TLS handshake; **always on** (#799 removed the `--cluster-plaintext` opt-out — no plaintext peer mode). |
| Replication (CRDT) | `replication.hpp`, `src/core/cluster/replication.cpp` (the one automerge-cpp user), `src/core/catalog.hpp` (internal key scheme) | cluster/replication.md | `ReplicationManager`: `set`/`remove`/`snapshot`/`save`/`merge`/`entry_count`/`version`. Document = source of truth. Thread-safe (mutex). |
| Async/net adapters | `runtime.hpp`, `runtime/net.hpp` | architecture/runtime.md, research/coroutines.md | `zetta::runtime` (Capy) + `zetta::runtime::net` (Corosio). Backend names confined to adapters. |
| GUI | `src/gui/app.hpp` (forward-declares SDL/ImGui), `src/gui/app.cpp` | architecture/architecture.md | Thin renderer over a `Node`+`Cluster`. Only `app.cpp` pulls heavy headers. |
| Utilities | `uuid.hpp`, `sha256.hpp`, `bytes.hpp`, `duration.hpp`, `signals.hpp`, `version.hpp` | — | `new_uuid`, `sha256_hex`, `format_bytes`, `format_duration`. |

## The replicated catalog (CRDT key scheme)

Defined in `src/core/catalog.hpp` (internal). Flat top-level keys, structured
values — each row/object owns its own key by UUID so concurrent edits never
collide; the UUID is also the on-disk folder/row id (identical on every node):

```
node:<node_id>        -> { address }                  cluster membership
db:<db_uuid>          -> { name }                      a database
tbl:<tbl_uuid>        -> { db, name }                  a table
row:<row_uuid>        -> { tbl, data:{…} }             a row
lo:<lo_uuid>          -> { db, name, size, sha256 }    large-object metadata (no body)
project:<proj_uuid>   -> { name, created, updated, owner }   an AI project (no parent)
input:<input_uuid>    -> { project, kind, source, filename, content_hash, size,
                           created_by, created_at }    a project input (no body)
deployment:<dep_uuid> -> { graph_name, target, node, node_count, edge_count, created }
                                                       a graph placed onto a target
run:<run_uuid>        -> { project, graph, status, created, updated, logs, outputs, lineage }
                                                       a run (top-level; JSON-Schema validated)
experiment:<uuid>     -> { name, seed, allocations, created }
                                                       a holdout experiment (#1105)
experiment_assignment:<experiment>:<entity_digest>
                      -> { entity, allocation, tier, propensity, bucket, exempt, assigned_at }
                                                       an independent uplift row (#1105)
principal:<uuid>      -> { kind, name, created }       an authenticated actor (no parent)
key:<uuid>            -> { principal, scope, created }  an access key (owned by a principal)
audit:<uuid>          -> { action, actor, target, allowed, detail, time, summary? }
                                                       an immutable audit event;
                                                       JSON-Schema validated
```

`Node::reconcile_from_replication()` decodes these into desired state and
materializes the on-disk projection; `migrate_to_replication()` captures
local-only data into the document on startup; persisted to
`data_dir/replication.automerge`.

## How-to recipes

**Add a storage engine** — implement `StorageEngine` (`name`, `add(id,doc)`,
`contains`, `size`, `at`, `id_at`) and construct it where `Table` builds its
store (`src/core/data/table.cpp`). Keep the row-id contract so reconcile stays
idempotent. Tests in `tests/data/test_jsonl_store.cpp`.

**Add a query dialect** — implement `zetta::query::Dialect` (`name()`+`parse()`)
producing the existing `Statement`s; construct it in `QueryService`. **Don't
touch the `Executor`.** Model on `src/core/query/postgres.cpp`. (A non-SQL input
*language* → the **new-input** skill instead.)

**Replicate a new kind of state** — add a key prefix + value shape to
`src/core/catalog.hpp`; **capture** it at the owning model object (give that
object the `ReplicationManager*`, call `set`/`remove`); **reconcile** it in
`Node::reconcile_from_replication()` (+ decode, + a `materialize_*` path that
does NOT re-capture); **migrate** it in `Node::migrate_to_replication()`. Add a
two-node convergence test (see `tests/cluster/test_data_replication.cpp`). If the value
is a rich/structured payload, **validate it against a JSON Schema at the
boundary** (both disk + CRDT entry points) — model on `src/core/pipeline/run.cpp`'s
`run_value_schema()` / `parse_run_value()` (pboettch/json-schema-validator,
PRIVATE to zetta-core, error-handler not exceptions). Expose the schema via a
public `*_schema()` accessor and register it in
`zetta::schemas::boundary_schemas()` (`src/core/schemas/registry.cpp`, #646) —
that one entry makes it drift-checked (`ZETTA_WRITE_SCHEMAS=1 ctest -R
Schemas` regenerates `schemas/<name>.json`) and discoverable on the spec index
(`/v1/specs`, `list_specs`/`get_spec`).

**Add a cluster protocol message** — add a `ZETTA/1 <VERB> …` line builder +
handle it in `apply_gossip` (read pump) and/or `gossip_writer` (write pump) in
`src/core/cluster/cluster.cpp`. The read pump must NOT write to the socket directly
(only one writer) — queue replies on `GossipLink::outbound` for the writer to
drain. Big payloads use base64 in one line (bounded by `kMaxLineBytes`).

**Transfer bytes peer-to-peer** — model on large objects: metadata in the CRDT,
bytes through a `BlobStore`-style interface that works off the CRDT + disk paths only
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

- **architecture/architecture.md** — node, run modes, data model, module map.
- **pipeline/projects.md** — the AI project (`zetta::pipeline::Project`) data model + lifecycle CRUD on a node.
- **pipeline/project-inputs.md** — project inputs (`zetta::pipeline::ProjectInput`): raw intent/artifact ingestion + provenance (replicated).
- **pipeline/n8n-import.md** — importing an n8n workflow JSON as an artifact input (raw bytes + sha256, schema-validated at the boundary).
- **pipeline/n8n-compile.md** — compiling an imported n8n workflow JSON into the ExecutionGraph IR (`compile_n8n_workflow`: nodes→typed steps, connections→edges; #308).
- **pipeline/deployments.md** — placing a validated graph onto a `DeploymentTarget` (R1 local), recorded + queryable.
- **pipeline/runs.md** — the Run (`zetta::pipeline::Run`) record: status/logs/outputs/lineage + JSON-Schema boundary validation.
- **ml/models.md** — the Model (`zetta::ml::Model`) entity: `model:` metadata + weights as a P2P large-object body (sha256) + `system.models` (ENT-14/FEAT-6).
- **data/storage.md** — on-disk layout, JSONL `_zid`/`doc` line format, engine interface.
- **data/query.md** — dialect-agnostic layer, Postgres subset, the `system` database (`system.nodes`, `system.deployments`, `system.models`).
- **data/large-objects.md** — LO API, metadata-in-CRDT + body-P2P-with-hash, import.
- **architecture/runtime.md** — `zetta::runtime` async interface + migration plan.
- **cluster/cluster.md** — equal peers, reachable-address banner, self-healing links, status view.
- **cluster/replication.md** — CRDT, "everything is replicated", consistency model + leadership (design), data replication.
- **build.md** — build/run/test, compilers, submodules.
- **research/coroutines.md** — coroutine-runtime design deep-dive (background).

## Sibling skills

- **new-input** — add a new input language/format (parser + executor that
  re-implements its behavior in zetta's runtime; e.g. Python `load_dataset`).
- **skill-writer** — author/update skills (progressive-disclosure structure).
- **perplexity** — generate a long deep-dive research query from a topic.
