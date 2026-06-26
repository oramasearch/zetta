# zetta — Claude rules

> **`zetta` is the codename/binary for the product *Orama Zetta*** — not "just a database."
> Read **[`docs/orama-zetta.md`](docs/orama-zetta.md)** first for what we're building and the
> working decisions: naming (Orama Zetta; "ZettaDB" retired), value-first messaging,
> use-case-driven development, **AI projects as the central item** (use cases like
> churn are generic AI-project types; manage projects to success — most AI projects
> fail), the first use case (**churn / UC-1**) built via a **wtm worktree plan**
> (execution graph over its issues → parallel worktrees), **growing the use-case
> catalog** (the `add-use-case` skill + `adding-a-use-case` runbook → a Perplexity
> deep-research prompt → blueprint → worktree plan), and backlog grooming.
> Source of truth is the wtm `ORAMA-ZETTA` blueprint (epic
> [`#279`](https://github.com/oramasearch/zetta/issues/279)). This file below is the engineering rules for the codebase.

**Technically, a zetta node is a database.** A *zetta node* is one running instance of it. A node
can run in two modes:

- **headed** (default) — the node plus an SDL3 + Dear ImGui UI rendering it.
- **headless** — the node alone, no UI, for servers with no display.

C++26, macOS-first. The GUI stack (SDL3, Dear ImGui, nlohmann/json, GoogleTest)
comes via CMake `FetchContent`; the coroutine/networking stack is vendored as
git submodules (see below). macOS builds need Homebrew Clang (`brew install
llvm`).

## Repository shape: bridge + modules

zetta is both a buildable product **and** a bridge repo: third-party
dependencies we want pinned and locally readable (rather than fetched opaquely)
live as **git submodules under `modules/`**. (This differs from a pure bridge
like amaro-bridge, which has no code of its own — zetta has both.)

- `modules/capy` — the C++ Alliance **Capy** coroutine foundation (`task<T>`,
  executors, thread pools); the backend for the `zetta::runtime` async seam. Built.
- `modules/corosio` — **Corosio** (coroutine-native networking, the Asio
  successor); built, and backs the cluster (`src/core/cluster.cpp`).
- `modules/automerge-cpp` — from-scratch **C++23 CRDT** (no Rust); backs
  replication (`src/core/replication.cpp`). Has its **own** nested submodules
  (`upstream/json`, which also provides zetta's nlohmann/json) — so the
  `--recursive` clone matters.

Both track `develop` and need **Clang 17+** (hence the Homebrew-Clang toolchain).
**Clone with submodules** — `git clone --recurse-submodules`, or
`git submodule update --init --recursive` in an existing checkout — or the build
fails at `add_subdirectory(modules/capy)`. Update a vendored module with
`git submodule update --remote modules/<name>` and commit the new gitlink.
`modules/README.md` is the index.

## Architecture

The core rule: **`zetta::Node` knows nothing about any UI.** It owns the
database lifecycle and is the only thing headless mode needs. The GUI is a thin
renderer driven by a Node; headless mode drives the same Node with no renderer.
This is enforced at the build level — the `zetta-core` static library has **no
SDL/ImGui dependency** — which is what guarantees a node can run on a headless
server.

```
main()                       picks the mode, owns the Node + Cluster lifecycle
  │  builds zetta::Node + zetta::Cluster, starts both
  ├── --headless ─→ zetta::run_headless(node, cluster)   # core only, no SDL
  └── --headed   ─→ zetta::gui::App(node, cluster).run() # SDL3 + ImGui over them
  ...then cluster.stop(), node.stop()
```

A node owns **databases**, each database owns **tables**, and each table holds
its rows through a **storage engine** (`zetta::StorageEngine`). `Node` has full
CRUD over databases (`create`/`find`/`drop`/`rename_database`, `database_names`)
and `Database` over tables (`create`/`find`/`drop_table`, `table_names`);
ownership is via `shared_ptr` so lookups hand back a safe handle, never a raw
pointer.

A node also owns **projects** (`zetta::Project`) — the central, user-managed **AI
project** artifact (id + name + created/updated + owner key), a top-level entity
with no parent (its runs attach in later slices). `Node` has the
same CRUD shape as databases (`create`/`find`/`drop`/`rename_project`,
`project_names`). A project is replicated state (flat key `project:<uuid>`) with
a `<project-uuid>/project.json` on-disk projection, persisted across restart in
both standalone and replicated nodes. A project **owns execution graphs**
(`zetta::ProjectGraph` — a named, persisted `ExecutionGraph` IR; replicated key
`graph:<uuid>`, projection `<project-uuid>/graphs/<uuid>.json`), the way a
database owns tables, with `create_graph`/`update_graph`/`drop_graph` CRUD
(`update_graph` rewrites the projection **and** re-captures the CRDT key, so an
edit replicates like a create). The headed **Graph Workspace** panel
(`App::draw_graph_workspace`) renders a project's graph as an editable node/edge
canvas — nodes in columns by `StepKind`, edges between them — and persists edits
back to the IR via `update_graph` (SCR-3/CMP-1, the GUI slice of FEAT-2). See
[docs/projects.md](docs/projects.md).

A project owns its **inputs** (`zetta::ProjectInput`) — raw intent/artifact
payloads captured with provenance (`kind` intent|artifact, `source`
upload|pasted, optional `filename`, `content_hash`, `created_by`, `created_at`),
the same way a `Database` owns its tables. `Project` has `create_input`/
`find_input`/`drop_input` (inputs are immutable provenance records — no rename),
plus `capture_intent` — the named "capture NL intent" front door that stores a
natural-language string as an `intent`/`pasted` input and **rejects empty
intent** (empty/whitespace-only → empty handle, nothing stored).
Each input is replicated state (flat key `input:<uuid>` carrying its parent
`project` ref); the **metadata rides in the CRDT** but the **raw payload body
stays on disk** (`<project>/inputs/<input-uuid>/payload`) like a large-object
body — fingerprinted by `content_hash`, with peer-to-peer body transfer left to
a later slice (the record replicates today; the bytes don't move yet). This is
storage + provenance only — no parsing/compilation. See
[docs/project-inputs.md](docs/project-inputs.md). The headed **New Project**
screen (`App::draw_new_project_modal`) is the GUI front door for all three at
once — name a project, paste an NL intent, and import / drag-drop an n8n workflow
JSON artifact — wiring `create_project` + `capture_intent` +
`import_n8n_workflow_file` into one atomic create (the n8n artifact is staged +
schema-validated on drop, imported on Create; SCR-2, the GUI slice of FEAT-1).
See [docs/new-project-screen.md](docs/new-project-screen.md).

A node also owns **runs** (`zetta::Run`) — the record of **one execution of a
deployed graph**: `status` (`Pending`→`Running`→`Succeeded`|`Failed`), `logs`,
produced `outputs` (datasets/models/indexes), and `lineage` (sources, steps,
freshness, confidence), plus optional `project`/`graph`/`deployment` id references
(the `deployment` ref ties a run to the placement the reconcile loop keeps alive —
see the deployment controller below). Another
top-level, replicated entity (flat key `run:<uuid>`, on-disk `<run-uuid>/run.json`),
modeled like `Project` but keyed by id (runs have no human name): `create_run`/
`find_run`/`drop_run`/`runs`, with the "update" half being mutators on the handle
(`set_status`/`add_log`/`set_outputs`/`set_lineage`) that capture into the CRDT
then persist. **Run is the first record validated against a formal JSON Schema**
(draft-07) at the storage boundary — every value entering from disk or the CRDT
passes `parse_run_value`, so malformed data is rejected with a clear reason
instead of corrupting the tree (via `pboettch/json-schema-validator`, linked
PRIVATE to zetta-core). A run is now also **produced by executing a graph** (see
the Graph execution section below). The headed app surfaces runs in a
**read-only Runs / Outputs / Lineage panel** (`App::draw_runs` — list of runs +
a detail pane with the selected run's status, color-coded logs, an outputs
table, and a lineage section with a confidence bar; the GUI slice of FEAT-5 /
SCR-6 / CMP-4). See [docs/runs.md](docs/runs.md).

A node also owns **models** (`zetta::Model`) — a **trained model** registered on
the node (ENT-14 / FEAT-6): its metadata (`name`, `kind`, free-form
`feature_schema` + `metrics`, `created`) plus a fingerprint of its serialized
**weights**. Another top-level entity keyed by id (like `Run`): `create_model`
(requires a non-empty `kind`) / `find_model` / `drop_model` / `models`, with
`set_weights`/`set_metrics` mutators on the handle that capture into the CRDT then
persist. The model is **split like a large object**: the **metadata rides in the
CRDT** (flat key `model:<uuid>` carrying `weights:{size,sha256}`) while the
**serialized weights body stays off the CRDT** — it lives on disk
(`<model-uuid>/weights`) and **transfers peer-to-peer over the existing
large-object body seam** (`BlobStore`/`ReplicatedBlobStore`, the `LOREQ`/`LORES`
exchange), verified against `size` + `sha256` before being written. `Model` is
validated against a formal **JSON Schema** at the storage boundary like `Run`
(`parse_model_value`; `kind` required, `metrics` numeric, `weights` carries both
size + hash), and the preflight integrity check counts model weights (present /
awaiting body / corrupt) the same way it does large-object bodies. The registry
is `Node::models()` + the SQL **`system.zetta_models`** table. This is storage +
provenance + replication only — training/inference (and producing the weights) is
a later slice. See [docs/models.md](docs/models.md).

A node also owns **principals** (`zetta::Principal`) — authenticated actors
(user/service/node), a top-level entity with the same CRUD shape as projects
(`create`/`find`/`drop`/`rename_principal`, `principal_names`). A principal owns
its **access keys** (`zetta::AccessKey`) the way a `Project` owns its inputs
(`create_key`/`find_key`/`drop_key`; keys are immutable credentials — no rename).
A key bounds access along **two orthogonal, deny-by-default axes**: its
**`Scope`** (`include/zetta/scope.hpp`) — *which resources*: a set of `Resource`s
(`kind` database|project + name), or `unrestricted` for an admin key — and its
**`CapabilitySet`** (`include/zetta/capability.hpp`) — *which rights*: a set of
`Capability` grants (ENT-22), each a `domain` (api|data|orchestration) + `action`
with an optional **TTL** (`expires_at`). This is the **V1 key-scoped access model
(RULE-3 / PERM-1 / ADR-6 / ENT-22)**: a request inherits both the scope and the
capabilities of the key it logged in with — in-scope resources are allowed and
everything else denied, and an `Operation` (a `domain`+`action`) is allowed only
with a matching, unexpired capability (there is no wildcard capability). The two
axes **compose** (an operation on a resource checks both).
`Node::authorize(key_id, resource)` and `Node::authorize(key_id, operation)` are
the enforcement seams (an unknown/empty key denies); `zetta::authorize(key,
resource)` and `zetta::authorize(key, operation, now)` are the pure decisions (the
injectable `now` makes TTL deterministic; the capability deny reason distinguishes
"never granted" from "TTL expired"). Principals + keys are replicated state (flat
keys `principal:<uuid>` and `key:<uuid>` carrying its parent `principal` ref +
its `scope` + `capabilities`), with `<principal-uuid>/principal.json` +
`<principal-uuid>/keys/<key-uuid>/key.json` on-disk projections. Finer access
tiers (team/partner/per-dataset, per-table) are deferred; granting a capability as
a first-class command/event and approval gates are separate FEAT-9 slices (the
audit trail itself is built — see below). See
[docs/access-control.md](docs/access-control.md).

A node also owns an **audit trail** — an immutable, append-only `zetta::AuditEvent`
for every **graph change / deploy / data access** (ENT-23 / FEAT-9; the audit
half of governance, on top of the key-scoped access above). An `AuditEvent`
(id + `action` graph_created|graph_dropped|graph_deployed|data_access + `actor` +
`target` + `allowed` + `detail` + `time`) is a top-level, replicated entity (flat
key `audit:<uuid>`) — but **immutable and append-only**: no mutator, no drop, so
the set of `audit:` keys only grows and the CRDT converges trivially. `Node` owns
the trail and **is** the `AuditSink` seam: `record_audit_event` is the append API
(it stamps id + time, captures into the CRDT, persists), `audit_events()` lists
them chronologically. Emission is wired at the three governed seams —
`Project::create_graph`/`drop_graph` (via the `AuditSink*` threaded into a project
like its `ReplicationManager*`), `Node::deploy_graph`, and `Node::authorize`
(which is therefore **non-const**: every allow/deny decision records a
`data_access` event). The `materialize_*` reconciliation paths never emit (a
peer's action arrives already-audited as its own replicated `audit:` event).
It is the **second record validated against a formal JSON Schema** at the storage
boundary (like Run — `parse_audit_event_value`, the same `pboettch` validator),
projecting to a single shared `<data_dir>/audit/<uuid>.json` per event rather than
a folder per event. See [docs/audit-trail.md](docs/audit-trail.md).

The headed **Admin & Settings** panel (`App::draw_admin`) is the **governance
GUI** (SCR-7 / CMP-5, the GUI slice of FEAT-9): a **read-only** view of principals
+ their access keys (each key's `Scope` resources + `CapabilitySet` rights, with
expired TTLs flagged) and the immutable audit trail (a time/action/actor/target/
outcome/detail table). Like the Runs panel it reads already-validated in-memory
handles, so it adds **no new JSON boundary**; mutation (issuing keys, granting
capabilities, approval gates) is a later slice. See
[docs/admin-panel.md](docs/admin-panel.md).

**Everything persists to disk** under `config_.data_dir`, and `Node::start()`
rebuilds the in-memory tree by scanning it, then runs a **preflight integrity
check** by default (`Node::check_integrity()` → `IntegrityReport`, logged as
`preflight:`; disable with `--no-preflight`): it re-hashes each local
large-object body against its recorded SHA-256 and flags dangling references
(row→missing table, table/LO→missing database). `report.ok()` = no corruption +
no dangling. See `docs/storage.md`. Two storage engines exist behind the
`StorageEngine` seam: `zetta::JsonlStore` (the default tables use — JSON Lines on
disk, chunked) and `zetta::JsonStore` (in-memory only; kept for a future
`:memory:` mode). **New engines implement `StorageEngine`; query/indexes land
behind it** — and don't let SDL/ImGui types leak into `zetta-core`.

### Async runtime (`zetta::runtime`)

zetta's async / coroutine / networking layer lives behind
`include/zetta/runtime.hpp` (namespace `zetta::runtime`). Code uses `zetta::runtime::task<T>`,
`zetta::runtime::block_on`, `zetta::runtime::thread_pool`, etc. and **never names
`boost::capy` / `boost::corosio` directly** — so the (beta, churning) C++
Alliance backend can be swapped, or replaced by Boost.Asio, by editing that one
header. Backed by Capy today (`modules/capy`); Corosio (`modules/corosio`) adds
networking. Background + migration plan:
[docs/runtime.md](docs/runtime.md) and
[docs/research/coroutines.md](docs/research/coroutines.md).

### Cluster (equal peers)

Nodes form a cluster of **equal peers** over TCP — no configured leader. Each
listens on all interfaces by default (`--listen HOST:PORT`, default
`0.0.0.0:7777`, so a plain node is network-reachable; set `127.0.0.1` to
restrict to loopback) and dials any `--peer HOST:PORT` (repeatable); the
handshake is symmetric so both record each other. Nodes default to the same port
and bind the first free one in a range (`--port-range`, default 16) so several
can share a host (e.g. NUMA). On startup (both modes) a node prints its id, the
address(es) it's reachable at, and the join command — a wildcard bind expands to
the host's real LAN IPs (`Cluster::listen_addresses()`), since `0.0.0.0` itself
isn't dial-able; the same reachable address is what it advertises in the
handshake. Links **self-heal**: gossip flows every ~0.5s, so a peer silent past
`peer_idle_timeout_seconds` (default 6) is dropped (clears half-open connections
with no clean EOF), nodes re-dial disconnected `--peer`s every
`reconnect_interval_seconds` (default 3), and peers are keyed by listen address
so a restarted node (new id, same address) **replaces** its stale entry instead
of leaving a ghost. `zetta::Cluster` runs networking on a background thread via
**Corosio**, behind the `zetta::runtime::net` seam (adapter:
`src/core/cluster.cpp`; public API exposes no backend types). The headed Cluster
panel shows the node id, every reachable listen address, peers, and the
**command to add another node** plus a **Nodes & replication** table; headless
logs that table. `Cluster::nodes()` returns this node + every connected peer with
its replication status (entries, version, in-sync, last-sync) — surfaced in the
headed panel, the headless log, and the SQL `zetta_nodes` system table. **Leader
election + failover are the planned next step** (`ClusterRole` exists for it,
currently always `Follower`); membership + replication gossip for now. See
[docs/cluster.md](docs/cluster.md).

### Signal plane (RPC / events / tasks)

The cluster transport also carries a typed, **ephemeral** message layer — the
**signal plane** (`zetta::Signal`, `include/zetta/signal.hpp`; ADR-28 / ARC-10) —
alongside the `SYNC`/`STATUS`/`LOREQ`/`LORES` gossip. A `Signal` is one of four
`SignalKind`s (`request`→`response`→`event`→`task`) addressed to a peer **by node
id**: request/response **RPC** (`Cluster::request` blocks until the peer replies,
times out, or is unreachable), fire-and-forget **events** (`send_event` unicast /
`broadcast_event` to all), and **task** envelopes (`send_task` — the substrate for
job dispatch). The application plugs in a `SignalHandler` (`set_signal_handler`;
`on_request`/`on_event`/`on_task`, each with a safe default) invoked on the
network thread. This is the foundation for the native distributed compute pass
(collectives, the work-stealing scheduler, the job coordinator).
**Ephemeral — deliberately NOT routed through the CRDT document** (the one
exception to "everything is replicated state": a signal is a *message*, not
*state*; durable agreement still belongs in the CRDT). The wire frame is one new
line verb, `ZETTA/1 SIGNAL <envelope>`, where the envelope is a single-line,
compact JSON object (opaque payload base64'd inside, binary-safe) **validated at
the boundary** against a draft-07 JSON Schema (`schemas/signal_envelope.schema.json`,
mirrored embedded in `src/core/signal.cpp`, via the same `pboettch` validator as
Run) plus kind-specific required-field checks in code — a malformed envelope is
rejected with a path-qualified reason and never reaches a handler
(`decode_signal` is non-throwing). Cross-thread sends go through a mutex-guarded
per-peer outbox + an RPC promise map (no coroutine is posted across threads — the
backend's frame allocator isn't thread-safe); the per-link writer flushes it on a
short tick, and membership/signal-only links now send a `PING` keepalive (since
they carry no periodic gossip). `encode_signal`/`decode_signal` are the
serialization seam; the transport only adds/strips the framing. See
[docs/signal-plane.md](docs/signal-plane.md).

### Replication (CRDT)

**Core rule: EVERYTHING is replicated state.** Replication is not a feature
bolted onto some data — it is the single source of truth for *all* distributed
state in zetta: **schemas (databases + tables / DDL), rows, cluster membership
(the node list), metadata — everything.** When you add any state that more than
one node should agree on, it goes through `ReplicationManager` (the Automerge
CRDT), period. Do **not** introduce a parallel, separately-maintained copy of
state that lives outside the replicated document and gets synced by hand — that's
exactly the anti-pattern this rule exists to prevent. The replicated document is
authoritative; in-memory structures (the `Node`'s database tree, the `Cluster`'s
peer table) should be **derived from / kept in lockstep with** it, not the other
way around. New replicated state = a new key/shape in the document, replicated by
the existing gossip; new "kinds of agreement" = a new `ReplicationPolicy`, never a
bespoke sync path.

> Current reality vs. the rule: DDL + rows now flow through replication (the CRDT
> is the source of truth; disk is its projection). The one remaining gap is
> **cluster membership**, still tracked in the `Cluster` peer table separately —
> **migration debt, not the design**: route the node list through replication
> too. Move things *toward* this rule, never away.

**Consistency model: multi-leader by default; leadership only where exclusivity
is required.** Because the base is a CRDT, every node is a writer and merges
converge conflict-free — there is **no global leader / no global election** (the
cluster is equal peers). A leader is needed only for **exclusive** structures
(locks, sequences, single-writer resources), which get a scoped, planned
mechanism: a replicated **`system.zetta_leadership`** table holding, per resource,
the holder + a **lease with a timeout** — leadership by *ascension* (claim if
unheld/expired; deterministic tiebreak on concurrent claims; renew before
expiry). **Raft is a future option scoped to just those exclusive structures**,
never the whole DB. Leadership is **per-(node, resource)**: a node holds **zero to
many** roles at once (leader for one resource, follower for another), so
`ClusterRole` (a single value today, always `Follower`) is a placeholder for a
future *set* of roles, not a node-wide title. **Leadership is documented design
only — not built yet**
(`docs/replication.md` → *Consistency model*). The active build is the
multi-leader data plane: **the CRDT document is the source of truth for data
(DDL + rows); the on-disk store + in-memory tree are a materialized projection,
and the document is persisted (`save()`/`merge()`) so it survives restart.** See
`docs/replication.md` → *Data replication*.

A node's replicated state (DDL/metadata/rows) is an **Automerge CRDT**
(`modules/automerge-cpp`, from-scratch C++23, no Rust). `zetta::ReplicationManager`
exposes only JSON + bytes (`set`/`snapshot`/`save`/`merge`, plus `entry_count`/
`version` for status); Automerge is hidden in the adapter
`src/core/replication.cpp`. Peers **gossip** `save()`/`merge()` blobs over the
cluster (the `Cluster` takes a `ReplicationManager*`; per-connection `SYNC`/
`STATUS` exchange on a timer in `src/core/cluster.cpp`) and converge
**conflict-free**. `version()` fingerprints the Automerge heads, so two nodes are
"in sync" exactly when their versions match. Each node's **UUIDv4 is its actor
id** (16 bytes; distinct ids make concurrent edits merge without dropping
changes) — the same UUID as its cluster node id, **persisted in
`data_dir/node_id`** so it's stable across restarts (a fresh id each run would
fragment the change history and accumulate `node:` keys). The document uses
**flat keys**
(`src/core/catalog.hpp`): `node:<id>` (membership), `db:<uuid>`, `tbl:<uuid>`,
`row:<uuid>`, `lo:<uuid>`, `project:<uuid>`, `input:<uuid>` (a project input —
metadata only; its payload body stays on disk), `graph:<uuid>`,
`deployment:<uuid>`, `run:<uuid>`, `model:<uuid>` (a trained model — metadata
only; its serialized weights body stays on disk like an `lo:` body),
`principal:<uuid>` (an authenticated actor), `key:<uuid>` (an access key carrying
its parent `principal` ref + its `scope` + its `capabilities`), and
`audit:<uuid>` (an immutable, append-only audit event) — each entity owns its key
by UUID so concurrent edits never collide.
`Node` is the integration point: it **captures** local DDL/row mutations into the
document (via `Node`/`Database`/`Table`, which hold the `ReplicationManager*`),
**reconciles** the on-disk projection from the document on each `tick()` (so peer
changes land on disk), and **persists** it to `data_dir/replication.automerge`.
Policies plug in via `ReplicationPolicy`; the first is `ReplicateToAllNodes`
(full copy on every node). Done + tested: CRDT core, cluster gossip + per-node
status, **and full DDL+row data replication to disk** (`tests/test_data_replication.cpp`).
See [docs/replication.md](docs/replication.md).

### On-disk layout

```
data_dir/
  node_id                    ← this node's stable UUID (cluster id + actor id)
  replication.automerge      ← the replicated CRDT document (source of truth)
  <project-uuid>/            ← one folder per project (UUIDv4 = its id)
    project.json             ← { id, name, created, updated, owner }
    inputs/
      <input-uuid>/          ← one folder per project input (UUIDv4 = its id)
        input.json           ← { id, project, kind, source, filename,
                                 content_hash, size, created_by, created_at }
        payload              ← raw bytes (NOT in the CRDT; on-disk like an LO body)
    graphs/                  ← this project's execution graphs
      <graph-uuid>.json      ← { id, project, name, graph: { nodes, edges } }
  <deployment-uuid>/         ← one folder per deployment (UUIDv4 = its id)
    deployment.json          ← { id, graph_name, target, node, node_count, … }
  <run-uuid>/                ← one folder per run (UUIDv4 = its id)
    run.json                 ← { id, project, graph, status, created, updated,
                                  logs, outputs, lineage }
  <model-uuid>/              ← one folder per model (UUIDv4 = its id)
    model.json               ← { id, name, kind, project, run, feature_schema,
                                  metrics, created, weights:{ size, sha256 } }
    weights                  ← raw serialized bytes (NOT in the CRDT; on-disk
                                 like an LO body — transfers peer-to-peer)
  <principal-uuid>/          ← one folder per principal (UUIDv4 = its id)
    principal.json           ← { id, kind, name, created }
    keys/
      <key-uuid>/            ← one folder per access key (UUIDv4 = its id)
        key.json             ← { id, principal, scope:{ unrestricted, grants },
                                 capabilities:[ { domain, action, expires_at? } ],
                                 created }
  audit/                     ← the immutable, append-only audit trail
    <audit-uuid>.json        ← { id, action, actor, target, allowed, detail,
                                 time }  (one file per event, never edited)
  <db-uuid>/                 ← one folder per database (UUIDv4 = its id)
    database.json            ← { id, name }
    <table-uuid>/            ← one folder per table (UUIDv4 = its id)
      table.json             ← { id, name }
      000000.jsonl           ← rows 0..9999    ({ "_zid": row-uuid, "doc": row })
      000001.jsonl           ← rows 10000..19999
      ...                    ← JsonlStore::kRecordsPerChunk = 10k per chunk
```

The folder name *is* the id; the human name lives only in `database.json` /
`table.json` (so rename rewrites that file, never moves the folder). Each JSONL
line is `{ "_zid": <row-uuid>, "doc": <row> }` — `_zid` is the row's CRDT key
(same id on every node); `doc` is what `SELECT` returns. (Legacy bare-row lines
are still read, with a fresh id assigned.) When replicated, the folder tree is a
projection of `replication.automerge`; standalone nodes read/write it directly.
`add_row` appends a line to the active chunk immediately (append-open per row —
simple and crash-resilient; can hold the stream open later). `data_dir` defaults to
`./zetta-data` and is git-ignored.

### Files: large objects + import

Two ways to ingest files, both via a native file dialog and drag-and-drop onto
the window (routed by extension):
- **Record import** (`zetta::import_records`) — a `.json`/`.jsonl` file becomes
  rows in the selected table (array → many rows, object/value → one; duplicates
  allowed; all-or-nothing on parse error).
- **Large objects** (`zetta::LargeObjectStore`, per `Database`) — any file
  stored as a binary blob under `<db>/large_objects/<lo-uuid>/`, modeled on
  Postgres' LO API (`import_file`/`export_file`/`read`/`unlink`/`list`). Ids are
  UUIDv4; a SHA-256 (`zetta::sha256_hex`) is recorded on import. **They
  replicate split:** the *metadata* (`lo:` key — `{db, name, size, sha256}`)
  rides in the CRDT; the *body* does **not** (it would bloat every node's
  document) — it transfers peer-to-peer (`LOREQ`/`LORES`) and is **verified
  against size + sha256** before being written. The cluster moves bodies through
  the `zetta::BlobStore` seam; `ReplicatedBlobStore` implements it from the CRDT
  metadata + on-disk paths (no Node access → safe on the network thread). Detail
  in [docs/large-objects.md](docs/large-objects.md).

A third, **typed-artifact** ingestion path captures a file into a *project*
(not a table or database) as a `ProjectInput`:
- **n8n workflow import** (`zetta::import_n8n_workflow` /
  `import_n8n_workflow_file`) — an [n8n](https://n8n.io) workflow `.json` export
  is **schema-validated at the boundary** (it must parse *and* match
  `n8n_workflow_schema()`: an object with a `nodes` array of node-shaped objects
  + a `connections` object — invalid JSON and non-workflow JSON are rejected with
  a clear reason, nothing stored), then stored **byte-for-byte** as an artifact
  `ProjectInput` (kind=Artifact, source=Upload) so the payload round-trips for
  later re-compilation. Computes nothing about the payload itself — it delegates
  to `Project::create_input`, which fingerprints the raw bytes with SHA-256 at
  the boundary. Storage + validation only (the import itself compiles nothing);
  surfaced in the GUI through the **New Project** screen's artifact import (file
  dialog + drag-drop), above. Detail in [docs/n8n-import.md](docs/n8n-import.md).
- **n8n workflow compile** (`zetta::compile_n8n_workflow`,
  `include/zetta/n8n_compile.hpp`) — the first concrete **compiler** of FEAT-2
  (#308): map an imported n8n workflow JSON into the **ExecutionGraph IR** — each
  n8n **node** → a typed `ir::Node` step (keyed by its unique node name, tagged
  with a `StepKind` via the `n8n_step_kind` table — R1 covers the churn node
  kinds), each n8n **connection** → a `DataFlow` `ir::Edge`. It re-uses the import
  boundary schema (`validate_n8n_workflow`) then enforces the IR's graph-semantic
  rules in code (unique node names, every connection references an existing node),
  **non-throwing** like `ir::from_json` (`std::optional` + a path-qualified reason;
  an unmapped node type fails rather than guessing a stage). An overload compiles
  straight from a stored artifact `ProjectInput`. Produces the in-memory IR only —
  persisting it as a `ProjectGraph` / GUI wiring is a later slice. Detail in
  [docs/n8n-compile.md](docs/n8n-compile.md).

### Query layer

Queries are **dialect-agnostic**: text → a `Dialect` parser → a generic,
dialect-neutral AST (`zetta::query::Statement`, a `std::variant`) → the
`Executor` (which runs the AST against a `Node`). `PostgresDialect` is the first
frontend; `QueryService` is the parse+execute facade the UI calls. Supporting
another database format = writing another `Dialect` that targets the same AST —
**never** add dialect specifics to the executor. Supported today: basic DDL
(CREATE/DROP DATABASE, CREATE/DROP TABLE) and query (INSERT, SELECT + single
`WHERE` equality). The executor also serves a virtual, read-only **`system`**
database (à la `pg_catalog`) whose tables are computed at query time — first
**`system.zetta_nodes`** (cluster + replication status, when a `Cluster*` is
wired into the `Executor`/`QueryService`; bare `zetta_nodes` still resolves) and
**`system.zetta_deployments`** (recorded graph→target placements, sourced from
`Node::deployments()`; qualified name only) and **`system.zetta_models`** (the
model registry — trained models + their kind / metrics / weights fingerprint,
sourced from `Node::models()`; qualified name only). `system` is reserved (can't
be created/dropped/written; no real db may be named it) and is where future
system tables (e.g. `system.zetta_leadership`) live. Full detail in
[docs/query.md](docs/query.md).

### Execution graph IR (`zetta::ir`)

The **ExecutionGraph IR** is the in-memory model of a data/AI system as a
directed graph of typed **steps** — `zetta::ir::Node` (an id + a `StepKind` + a
name + a free-form `nlohmann::json` attribute bag) joined by typed
`zetta::ir::Edge`s (`EdgeKind::DataFlow`/`Control`). The **step-kind taxonomy**
(`StepKind`, in `ir/step_kind.hpp`) is the fixed pipeline
**connect→clean→model→index→serve→act**, declared in that order (so the enum
values increase along the pipeline). `ExecutionGraph` (build with `add_node`/
`add_edge`; edit with `remove_node`/`remove_edge` — the inverses, where removing
a node drops every edge incident to it so none is left dangling; traverse with
`nodes_of_kind`/`edges_of_kind`/`successors`/`predecessors`/`find_node`) hands
back nodes as `shared_ptr` handles (empty on miss) like the rest of zetta, and
guards its **structure** (no duplicate ids, no dangling or duplicate edges) — but
not its **semantics**. This is the keystone the compilers, gate, deploy, and run
all build on (ENT-3/FEAT-2).

A graph has a canonical **JSON form** (`ir/serialization.hpp`: `to_json` →
`{ nodes:[{id,kind,name,attributes}], edges:[{from,to,kind}] }`, with `kind` the
lowercase name not the enum int). `from_json` is the **trust boundary**: it is
**non-throwing** (returns `std::optional` + an error string, matching zetta's
no-exceptions convention — never `throw` across the zetta-core/`-fexperimental-library`
seam, which can't unwind), validating the document against a JSON Schema +
the IR's structural rules and skipping anything malformed. This JSON form is what
persists + replicates a graph **under a Project** (`graph:<uuid>`). Still later
slices: the dual-form Lua/Orly + Parquet/CSV representation (ADR-10) and semantic
validation (acyclicity, legal stage transitions — the **gate**). Full detail in
[docs/execution-graph.md](docs/execution-graph.md).

### Intent compiler (NL intent → ExecutionGraph skeleton)

The **intent compiler** (`include/zetta/intent_compiler.hpp`,
`zetta::compile_intent`) turns a stored natural-language **intent** — the text
`Project::capture_intent` recorded (FEAT-1) — into a reviewable
`ir::ExecutionGraph` skeleton laid out over the
`connect→clean→model→index→serve→act` taxonomy (FEAT-2 / #311). It is a
**templated, heuristic** mapping, **not** language understanding: a transparent
signal-word scan recognizes the **churn shape** (UC-1; `churn`/`retention`/
`attrition`/`cancel`/`renew`/`subscriber`/`lapse`/…) and emits the full churn
pipeline (two `connect` sources → feature `clean` → churn `model` → `index` →
`serve` → retention `act`); any other intent falls back to a generic-but-complete
six-stage skeleton, so an intent **always** compiles to a well-formed, editable
graph. The output is the IR itself — named, typed, **editable** steps, **not a
black box** — and is deterministic (same intent → identical graph). An empty /
whitespace-only intent compiles to nothing (`std::nullopt`), mirroring
`capture_intent`. Richer NL/LLM synthesis plugs in later behind the same
`compile_intent` front door, the seam pattern the executor's `StepExecutor` and
the gate's `Check` use. `Project::compile_intent_to_graph(input_id, name)` is the
bridge from a *stored* intent input to an editable graph: it reads the input's
payload, runs the compiler, and persists the skeleton via `create_graph` (so it
replicates + is editable in the **Graph Workspace** exactly like any stored
graph). It adds **no new JSON format** — the input is plain text and the output
is the already-schema-validated IR JSON form, so the skeleton round-trips through
`ir::from_json` and passes `default_gate()` by construction. See
[docs/intent-compiler.md](docs/intent-compiler.md).

### Correctness gate (`zetta::CorrectnessGate`)

The **correctness-gate runner** (`include/zetta/gate.hpp`) is the piece that joins
the two slices above: register a set of **checks**, run them all over an
`ExecutionGraph`, aggregate their outcomes into a `ValidationReport`, and **block
deploy on any failure** — **RULE-1** (no deploy without a passing gate). A `Check`
is the pluggable seam (`name()` + `evaluate(graph) → CheckResult`); concrete
checks implement it and plug in **without touching the gate**. `make_check(name,
callable)` wraps a stateless callable as a `Check` (stamping the registered name
onto its result so the two never drift). `default_gate()` ships the trivial
`NonEmptyGraphCheck` (an empty graph has nothing to deploy → fail) **plus the
concrete `#309` checks** — `SchemaTypeCheck` + `DataLeakageCheck`
(`include/zetta/graph_checks.hpp`): they read documented **annotations** from each
step's free-form `attributes` bag (`schema`/`requires` field→type profiles;
`as_of`/`label`/`features` point-in-time metadata, validated against
`schemas/graph_step_annotations.schema.json` at the boundary) and fail the gate
with a clear reason when a step requires a field no upstream source provides /
provides under a different type (**schema/type**), or when an ML feature uses data
after the as-of cutoff or is derived from the label (**data-leakage /
point-in-time**) — **plus** the first entity-resolution check,
`EntityResolutionCheck` (`#312` / TC-4, below). They are **opt-in per step** — a
graph carrying none of the annotations passes.
`CorrectnessGate::run` sets `block_deploy = !report.passed()` (baseline
block-on-failure policy; verdict and block_deploy stay
[separate](docs/validation.md) so a stricter gate can choose differently). It adds
**no new JSON format** — the `ValidationReport` it produces is already
schema-validated. Full detail in [docs/gate.md](docs/gate.md) +
[docs/gate-checks.md](docs/gate-checks.md).

### Entity-resolution correctness eval (`zetta::EntityResolutionCheck`)

The first concrete gate check (`include/zetta/checks/entity_resolution_check.hpp`,
FEAT-3 / TC-4, `#312`) — the **make-or-break** eval. It reads each graph node that
carries an `entity_resolution` attribute (a `{ records: [{ mention, entity,
resolved }] }` payload) and checks that resolution was **division-correct**:
"FedEx Freight" must land on its own CRM account without **conflating** the 5–6
sibling FedEx subsidiaries (Ground, Express, Office, Logistics, …), which are
separate accounts. The rule compares the partition by `resolved` against the
partition by `entity`: **conflation** (one account holding two distinct
entities) **fails** the gate and blocks deploy (RULE-1, naming the account +
entities); **fragmentation** (one entity split across accounts) is the softer,
symmetric error and **warns** (does not block); a graph with no resolution step
passes vacuously. The payload is **schema-validated at the boundary** against
`schemas/entity_resolution.schema.json` (embedded draft-07 copy in the `.cpp`,
via `pboettch/json-schema-validator`, the same pattern as the Run record) so a
malformed payload fails the check with a clear reason. Registered in
`default_gate()` after `NonEmptyGraphCheck`. Full detail in
[docs/entity-resolution-check.md](docs/entity-resolution-check.md).

### Synthetic churn dataset (UC-1 fixture) + loader

The **synthetic churn dataset** (`zetta::ChurnDataset`,
`include/zetta/churn_dataset.hpp`) is the **root task of FEAT-6** ("learns from
your data") for **UC-1 churn** (#402; supports TC-4): a small, committed,
synthetic (non-customer) churn dataset — **accounts** + **comms/usage events** —
plus a tiny C++ loader, the substrate the later featurization/training/oracle/
end-to-end tests build on. **No foreign runtime (ADR-17):** the data is checked
in under `tests/fixtures/churn/` (`dataset.json` manifest + `accounts.{jsonl,csv}`
+ `events.{jsonl,csv}`); the generator that authored it is **not** a
repo/build/CI dependency. It carries an **`as_of_date`** (`t0`) + a
**`churn_horizon_days`** (so featurization windows to `event_ts <= t0` and a few
events deliberately sit **after** `t0` — the leakage trap), a per-account churn
label, and the make-or-break **FedEx entity-resolution case** (FedEx Freight + 5
sibling subsidiaries as **separate accounts**). `load_churn_dataset(dir, format)`
is **non-throwing** and **schema-validates every record at the boundary** (three
draft-07 schemas — `schemas/churn_{dataset,account,event}.schema.json`, embedded
in the `.cpp` like the Run record); `ChurnDataFormat` is `{ Jsonl (canonical),
Csv (a flat sibling), Parquet (planned — an honest seam that errors until an
Arrow/parquet-cpp dependency lands) }`. `entity_resolution_attribute(dataset)`
builds the division-correct `entity_resolution` payload from the comms mentions,
ready to feed `EntityResolutionCheck` (TC-4); the JSONL also imports straight into
a zetta table via `import_records`. See [docs/churn-dataset.md](docs/churn-dataset.md).

### Deployment (place a graph onto a target)

A **deployment** places a validated `ir::ExecutionGraph` onto a
**`DeploymentTarget`** and records it — the first slice of *one-button deploy*
(FEAT-4) + the `DeploymentTarget` entity (ENT-6). A `DeploymentTarget`
(`include/zetta/deployment_target.hpp`) is a placement on the **dial**
`local → kubernetes → vpc → cloud` (`DeploymentKind`, declared in that order),
holding a Cloud Interface **binding** + a **credentials-ref**; R1 ships **`Local`
only** (the rest are declared but not built, like `ClusterRole`), and a Local
target has an empty binding/credentials (implicitly this node). `Node` owns
deployments like projects: **`deploy_graph(graph, target, name)`** (empty handle
on an empty graph or a non-local target — basic guards only; the semantic
**correctness gate** above runs separately, not yet wired into `deploy_graph`),
`find_deployment(id)`, `deployments()`, `deployment_count()`. A `Deployment`
(`include/zetta/deployment.hpp`) is **replicated state** (flat key
`deployment:<uuid>` → `{ graph_name, target, node, node_count, edge_count,
created }`) with a `<deployment-uuid>/deployment.json` projection, persisted across
restart and replicated to peers (recording the node it was placed on). The graph is
recorded as a **summary** (name + node/edge counts), not the full graph — IR
serialization is a later slice. It's queryable via the C++ API **and** the SQL
**`system.zetta_deployments`** table. k8s/cloud are separate tasks; the
reconcile/self-heal control plane is the **deployment controller** below, and the
**Deploy panel** below now drives deploy from the GUI. `Node` learns its own id
(`Node::node_id()`) from the `ReplicationManager` actor id, or from
`data_dir/node_id` when standalone. See [docs/deployments.md](docs/deployments.md).

### AI Solution Library + Deploy panel (one-button deploy)

The **AI Solution Library** (`zetta::SolutionCatalog`,
`include/zetta/solution_catalog.hpp`) is a curated, schema-validated catalog of
ready-to-deploy `ir::ExecutionGraph` **blueprints** — the GUI's source of
deployable graphs until a graph editor lands. A `Solution` is identity +
presentation (`id`/`name`/`summary`/`category`) + a `graph`; `SolutionCatalog`
is an ordered, read-only library with `find(id)` (non-owning `const Solution*`).
`SolutionCatalog::builtin()` ships five representative blueprints (churn,
recommendations, semantic search, lead scoring, support triage) parsed once from
embedded data. The catalog is **boundary-validated like the Run record**:
`parse_solution_catalog` is non-throwing and layers a draft-07 **JSON Schema**
(`schemas/solution_catalog.schema.json`, mirrored embedded in
`src/core/solution_catalog.cpp`) for the document shape, then
`ir::from_json` for each blueprint's graph structure, then an id-uniqueness check
— rejecting a malformed catalog with a path-qualified reason instead of corrupting
state.

The headed **Deploy panel** (`App::draw_deploy`, `src/gui/app.cpp`) realizes
*one-button deploy* (FEAT-4 / SCR-5 / CMP-3): pick a solution, choose a placement
on the **dial** (R1 **Local** only — k8s/vpc/cloud shown but disabled, labelled
"(planned)"), then the one **Deploy** button **runs the correctness gate first**
(`default_gate().run`; RULE-1 — blocks with the failing check's reason, places
nothing) and on a pass calls `Node::deploy_graph`. A **Deployments** table shows
the recorded placements (`Node::deployments()`) so a deploy's status is visible.
The panel adds no persisted state of its own — only UI selection. See
[docs/solution-catalog.md](docs/solution-catalog.md).

### Deployment controller (reconcile + self-heal)

The **control plane** for local deployments (`zetta::DeploymentController`,
`include/zetta/deployment_controller.hpp`) drives a node from *desired* state (a
`Deployment` — "this graph should run here") toward *actual* state (a `Run` — its
execution), and **self-heals** when the placed run is killed or fails — the
reconcile-loop slice of FEAT-4, realizing TC-3. `Node` owns one
(`deployment_controller()`) and drives it from **`tick()`**: each heartbeat (1)
pulls peer changes into the on-disk projection, (2) runs `controller_.reconcile()`,
then (3) persists if the document moved. `reconcile()` scans **this node's** local
deployments (`target` local **and** `node_id() == self` — R1; cross-node failover
needs leadership and is a later slice) and for each ensures a healthy run: a run
**satisfies** its deployment unless it has **failed** (pending/running keep desired
met; *succeeded* is terminal and not restarted), so a deployment with no run, or
only failed runs, gets a fresh `Running` run started + logged + recorded against it.
Steady state captures nothing (a read-only scan → no replication churn). The run↔
deployment link is a new optional **`deployment` ref on `Run`** (set via
`create_run(project, graph, deployment)`), part of the Run JSON Schema and
boundary-validated like every other field. `reconcile()` returns a `ReconcileReport`
(local/satisfied/started/healed counts); cumulative counters
(`total_runs_started()`/`passes()`) aid observability. See
[docs/deployment-controller.md](docs/deployment-controller.md).

### Graph execution (run a graph → a Run)

The **graph executor** (`include/zetta/executor.hpp`) joins the ExecutionGraph IR
(*what* to run) and the Run record (*the result*) — an atomic slice of FEAT-5.
`zetta::GraphExecutor` walks a graph in **dependency (topological) order**
(`topological_order` — Kahn's over both `DataFlow` + `Control` edges; ready steps
in insertion order, so it's deterministic; a cycle/self-edge → a cleanly Failed
run) and drives a `Run` created on the node through `Pending → Running →
Succeeded | Failed`, accumulating a log line per step, the typed outputs each step
materializes, and the lineage (sources/steps/freshness/confidence). The entry
point is **`Node::run_graph(graph, project={}, graph_id={})`** (a thin convenience
over the standalone `GraphExecutor`, which mirrors `CorrectnessGate`). Per-step
work is the pluggable **`StepExecutor`** seam (mirrors the gate's `Check`): the
`DefaultStepExecutor` interprets the `connect→clean→model→index→serve→act`
taxonomy (connect→`dataset`+a lineage source, clean→`dataset`, model→`model`,
index→`index`, serve/act→no artifact) with **no real compute** — real
training/indexing plugs in behind the seam without touching the orchestrator
(same altitude as deployments recording a summary). Fail-fast: a step fails via a
truthy `"fail"` attribute (or a custom `StepExecutor`), later steps don't run, and
the failing step + reason land in the run's logs. It adds **no new JSON format** —
the only artifact is a `Run`, already schema-validated at the storage boundary,
and the executor only emits schema-legal output kinds (a restart-reload test
proves the produced run passes `parse_run_value`). See
[docs/execution.md](docs/execution.md).

### Provisioning (node capability model + the `Provisioner` seam)

The **`Provisioner`** seam (`include/zetta/provisioner.hpp`) and the **node
capability model** (`include/zetta/node_capability.hpp`) open the **native
distributed accelerated compute** pass for UC-1 churn (ARC-13 / ENT-32). A
**`zetta::NodeCapability`** describes one node's compute resources — `cores`
(CPU), `memory_bytes`, and a `Gpu` (`GpuKind` none/cuda/rocm/metal +
`throughput_tflops` + optional `model`) — used **both** as the capability a node
reports and as the per-node floor a provision spec asks for; `satisfies()` is the
meet-or-exceed comparison (cores ≥, memory ≥, and a matching-kind GPU at ≥ the
requested throughput when a GPU is required). It is **distinct from**
`zetta::Capability` (an access-control *right*, not *compute*) and feeds the
per-node device registry (#362). The **`Provisioner`** interface is the abstract
seam both the Local (given-hardware, #374) and cloud-burst (AWS #377 / GCP #378)
backends implement and the autoscaler/job coordinator consumes:
`provision(request)` brings up `count` nodes meeting a `spec` **best-effort**
(may return fewer), `teardown(node_id)` releases one **idempotently** (unknown id
→ no-op), and `provisioned()` lists what is up. `ProvisionedNode`
(`{ node_id, address, capability }`) and `NodeCapability` carry a
**validated JSON boundary** (non-throwing `from_json`, mirroring
`schemas/node_capability.schema.json`, the `DeploymentTarget` pattern). **No
cloud SDK lives behind the seam** — this slice is interface + value types + a
fake-provisioner round-trip test only; persistence/replication and the concrete
backends are later slices. See [docs/provisioner.md](docs/provisioner.md).

## Tech stack

- **C++26** (`-std=c++2c`) — needs **Clang 17+** (or GCC 14+). Auto-selected:
  Homebrew LLVM Clang on macOS (`brew install llvm`), newest `clang++-NN` on
  Linux. System defaults (Apple Clang 15, Ubuntu GCC 13) are too old. See C++ conventions below
- **CMake 3.28+ / Ninja**, GUI deps via `FetchContent`:
  - **SDL3** (release-3.2.0) — window + input + Metal-backed renderer
  - **Dear ImGui** (v1.91.5) — immediate-mode GUI, SDL3 + SDL_Renderer backend
  - **nlohmann/json** (v3.11.3) — JSON documents for the storage engine
    (linked into `zetta-core`, so headless gets it too)
  - **json-schema-validator** (pboettch, 2.4.0) — JSON-Schema boundary
    validation (the Run record; `src/core/run.cpp`). FetchContent'd **after**
    automerge-cpp so it reuses that nlohmann/json target; linked **PRIVATE** to
    `zetta-core`, and compiled with `-fexperimental-library` to match
    zetta-core's libc++ ABI (propagated from Boost::capy).
- **ccache** used as the compiler launcher when present (auto-detected).

## Build + run

```bash
# Submodules (modules/capy, corosio, automerge-cpp + their nested ones):
git submodule update --init --recursive   # or clone with --recurse-submodules
./scripts/install-deps.sh                  # system build tools (macOS / Ubuntu)

cmake -S . -B build -G Ninja
cmake --build build

./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta                                  # cluster node (binds :7777)
./build/bin/zetta --peer 127.0.0.1:7777            # 2nd node here (auto :7778), joins it
./build/bin/zetta --help
```

The first configure clones SDL3 + ImGui into `build/_deps/` (a few minutes);
subsequent builds are fast (ccache). `build/` is git-ignored.

## Tests

GoogleTest, fetched via FetchContent, wired into CTest:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

Tests live in `tests/` (one file per area) and link `zetta-core` directly —
they exercise the headless core, never the GUI. `tests/temp_dir.hpp` gives each
test an isolated, auto-cleaned `data_dir` for persistence tests. **Add/extend
tests when you touch core logic** (storage, persistence, CRUD, parsing).
Building tests is on by default; turn off with `-DZETTA_BUILD_TESTS=OFF`.

## Code layout

```
CMakeLists.txt              build graph: imgui lib, zetta-core lib, zetta exe
cmake/embed_font.cmake      bakes a .ttf into a C++ header at configure time
scripts/install-deps.sh     install system build tools (macOS / Ubuntu)
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the node; CRUD over databases + projects + deployments + runs + models + principals; run_graph; the audit sink
  database.hpp              zetta::Database — owns tables (disk-backed)
  project.hpp               zetta::Project — an AI project; owns inputs + execution graphs
  project_input.hpp         zetta::ProjectInput — raw intent/artifact input + provenance
  project_graph.hpp         zetta::ProjectGraph — a named ExecutionGraph under a project
  deployment_target.hpp     zetta::DeploymentTarget — a placement (ENT-6; R1 local)
  deployment.hpp            zetta::Deployment — a graph placed onto a target
  deployment_controller.hpp zetta::DeploymentController — reconcile local deployments desired→actual + self-heal (FEAT-4/TC-3)
  node_capability.hpp       zetta::NodeCapability/Gpu/GpuKind — node compute capability model (cores/GPU/memory) + satisfies (ENT-32)
  provisioner.hpp           zetta::Provisioner/ProvisionedNode — provision N nodes against a capability spec / teardown / list (ARC-13; #373)
  solution_catalog.hpp      zetta::SolutionCatalog — AI Solution Library (deployable blueprints) + schema-validated parse
  run.hpp                   zetta::Run — one graph execution (status/logs/outputs/lineage) + Run JSON Schema
  model.hpp                 zetta::Model — a trained model (metadata + weights fingerprint) + Model JSON Schema (ENT-14/FEAT-6)
  principal.hpp             zetta::Principal — an authenticated actor; owns access keys
  access_key.hpp            zetta::AccessKey — a scoped + capability-bearing login credential + authorize()
  scope.hpp                 zetta::Scope/Resource — deny-by-default key-scoped access, the resource axis (RULE-3)
  capability.hpp            zetta::Capability/CapabilitySet/Operation — deny-by-default rights, the verb axis, TTL'd (ENT-22)
  audit_event.hpp           zetta::AuditEvent/AuditSink — immutable audit trail + JSON Schema (ENT-23)
  intent_compiler.hpp       zetta::compile_intent — NL intent → templated ExecutionGraph skeleton (FEAT-2)
  executor.hpp              zetta::GraphExecutor / StepExecutor — orchestrate a graph into a Run
  table.hpp                 zetta::Table — owns a storage engine (its rows)
  large_object.hpp          zetta::LargeObjectStore — files as blobs (per database)
  import.hpp                zetta::import_records — JSON/JSONL files → table rows
  churn_dataset.hpp         zetta::ChurnDataset / load_churn_dataset — synthetic UC-1 churn fixture loader (FEAT-6 / #402)
  n8n_import.hpp            zetta::import_n8n_workflow — n8n workflow JSON → artifact ProjectInput (schema-validated)
  n8n_compile.hpp           zetta::compile_n8n_workflow — n8n workflow JSON → ExecutionGraph IR (nodes→steps, connections→edges; #308)
  uuid.hpp                  zetta::new_uuid() — v4 UUID for folder names
  sha256.hpp                zetta::sha256_hex — large-object body fingerprint
  timestamp.hpp             zetta::now_iso8601() — UTC ISO-8601 stamp (created/updated)
  validation.hpp            zetta::ValidationReport — correctness-gate result (ENT-5/FEAT-3)
  gate.hpp                  zetta::CorrectnessGate — gate runner: checks → report (RULE-1)
  graph_checks.hpp          zetta::SchemaTypeCheck + DataLeakageCheck — the #309 concrete checks
  checks/
    entity_resolution_check.hpp  zetta::EntityResolutionCheck — division-correct resolution (TC-4)
  bytes.hpp / duration.hpp  human-readable size / time formatters
  runtime.hpp               zetta::runtime — async/coroutine seam over Capy
  runtime/net.hpp           networking slice of the seam (Corosio; adapter-only)
  cluster.hpp               zetta::Cluster — equal-peer membership over TCP + the signal-plane API
  signal.hpp                zetta::Signal/SignalHandler — ephemeral RPC/event/task signals + envelope encode/decode (ADR-28)
  replication.hpp           zetta::ReplicationManager — Automerge CRDT state
  blob_store.hpp            zetta::BlobStore — large-object body transfer seam
  replicated_blob_store.hpp zetta::ReplicatedBlobStore — BlobStore from CRDT + disk
  headless.hpp              zetta::run_headless(Node&, Cluster&)
  signals.hpp               install_signal_handlers / stop_requested
  storage/
    engine.hpp              zetta::StorageEngine interface
    json_store.hpp          zetta::JsonStore — in-memory vector<json> (header-only)
    jsonl_store.hpp         zetta::JsonlStore — JSONL on disk, 10k/chunk
  ir/
    step_kind.hpp           zetta::ir::StepKind — connect→…→act taxonomy (header-only)
    execution_graph.hpp     zetta::ir::ExecutionGraph — typed step nodes + edges
    serialization.hpp       zetta::ir::to_json/from_json — the graph's JSON form
src/core/                   zetta-core impl
  catalog.hpp               flat-key scheme for the replicated catalog (internal)
  node.cpp                  load_databases()/load_projects()/load_deployments()/load_runs() + replication capture/reconcile/persist
  database.cpp
  project.cpp               zetta::Project — persist/rename project.json + input/graph CRUD
  project_input.cpp         zetta::ProjectInput — persist input.json + payload body
  project_graph.cpp         zetta::ProjectGraph — persist a graph's json projection
  churn_dataset.cpp         load_churn_dataset — JSONL/CSV churn fixture reader + 3 embedded draft-07 schemas (boundary validation)
  n8n_import.cpp            zetta::import_n8n_workflow — n8n workflow JSON Schema + validate + store as artifact input
  n8n_compile.cpp           zetta::compile_n8n_workflow — n8n node-type→StepKind table + connections→edges → ExecutionGraph IR
  deployment_target.cpp     zetta::DeploymentTarget — names + JSON (boundary-validated)
  deployment.cpp            zetta::Deployment — persist deployment.json
  deployment_controller.cpp zetta::DeploymentController — reconcile pass: start/self-heal a deployment's run
  node_capability.cpp       zetta::NodeCapability — GpuKind names, satisfies(), JSON boundary validation (ENT-32)
  provisioner.cpp           zetta::ProvisionedNode — JSON boundary validation for the Provisioner DTO
  solution_catalog.cpp      zetta::SolutionCatalog — built-in library + schema/IR boundary parse
  run.cpp                   zetta::Run — value, mutators, JSON-Schema boundary validation (parse_run_value)
  model.cpp                 zetta::Model — value, mutators, weights body I/O, JSON-Schema boundary validation (parse_model_value)
  principal.cpp             zetta::Principal — persist/rename principal.json + key CRUD
  access_key.cpp            zetta::AccessKey — persist key.json + authorize() decisions (resource + operation)
  scope.cpp                 zetta::Scope — permits() + JSON boundary validation
  capability.cpp            zetta::CapabilitySet — permits()/TTL + JSON boundary validation (ENT-22)
  audit_event.cpp           zetta::AuditEvent — value, JSON form, JSON-Schema boundary validation
  intent_compiler.cpp       compile_intent — templated churn/generic skeleton (NL intent → IR)
  executor.cpp              GraphExecutor + topological_order + DefaultStepExecutor (graph → Run)
  table.cpp
  jsonl_store.cpp
  uuid.cpp
  sha256.cpp                self-contained SHA-256 (no crypto dependency)
  timestamp.cpp             zetta::now_iso8601() — UTC ISO-8601 stamp
  validation.cpp            ValidationReport verdict + JSON round-trip/validation
  gate.cpp                  CorrectnessGate runner + NonEmptyGraphCheck + make_check
  graph_checks.cpp          SchemaTypeCheck + DataLeakageCheck (#309) + annotation JSON-Schema validation
  checks/
    entity_resolution_check.cpp  EntityResolutionCheck — conflation/fragmentation + schema boundary
  replicated_blob_store.cpp large-object body transfer (CRDT metadata + disk)
  ir/execution_graph.cpp    ExecutionGraph IR build + traversal
  ir/serialization.cpp      ExecutionGraph IR ↔ JSON (+ boundary validation)
  cluster.cpp               cluster networking adapter (the one Corosio user) + signal-plane transport
  signal.cpp                zetta::Signal — envelope JSON form + JSON-Schema boundary (encode/decode)
  base64.hpp                shared base64 (gossip blobs + signal payloads; internal)
  replication.cpp           replication CRDT adapter (the one automerge-cpp user)
  headless.cpp              signal-driven headless loop
  signals.cpp               SIGINT/SIGTERM stop flag (OS signals, not the signal plane)
src/gui/                    headed UI (links SDL3 + ImGui)
  app.hpp / app.cpp         zetta::gui::App — window + render loop over a Node
src/main.cpp                arg parsing + mode dispatch
modules/                    vendored deps as git submodules (capy, corosio)
tests/                      GoogleTest unit tests (CTest); link zetta-core
build/generated/            embedded font header (generated; git-ignored)
```

`src/gui/app.hpp` forward-declares SDL/ImGui types so only `app.cpp` pulls in
those heavy headers — keep it that way.

## The two non-obvious things in `src/gui/app.cpp`

These are load-bearing and easy to break:

1. **Per-monitor scaling (`App::apply_display_scale`).** SDL3 separates *pixel
   density* (Retina; the ImGui SDLRenderer backend already handles this) from
   *content scale* (the OS "make the UI bigger" hint; ImGui does NOT apply it).
   `SDL_GetWindowDisplayScale` multiplies the two — using it directly
   double-scales on Retina. We read `SDL_GetDisplayContentScale` for the content
   factor only, and re-rasterize the font atlas at framebuffer-pixel density so
   text stays crisp when the display changes.

2. **The `SDL_SetRenderScale` call before clearing.** The window uses
   `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so the framebuffer is larger than the
   logical window. The SDLRenderer3 backend does NOT set the render scale for
   us — without this line the UI paints into the top-left quadrant only.

The font (DroidSans.ttf from the ImGui tree) is embedded via
`cmake/embed_font.cmake` into `namespace zetta`, so the binary has no runtime
asset dependency.

## Known gaps / behavior notes

- **Headed mode doesn't quit on terminal Ctrl-C** — SDL3 on macOS doesn't turn
  SIGINT into a quit event here, so close the window or use Cmd+Q. Only headless
  mode installs SIGINT/SIGTERM handlers. (Wiring a handler into headed mode is a
  reasonable future tweak if it becomes annoying.)

## Conventions

### C++ conventions
- **C++26** (`set(CMAKE_CXX_STANDARD 26)`, `cxx_std_26`). Use modern
  facilities; don't reach for older idioms when a newer one is clearer. The
  build auto-selects a C++26 compiler in the pre-`project()` block in
  CMakeLists.txt — Homebrew Clang on macOS, `clang++-NN` (≥17) on Linux. System
  defaults (Apple Clang 15, Ubuntu GCC 13) can't do C++26 or build Capy/Corosio.
- **Follow the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/).**
- **Full, descriptive names — legibility first.** No 1–2 letter or cryptic
  abbreviations anywhere: namespaces, types, functions, variables, *and template
  parameters*. `runtime` not `rt`, `error_code` not `ec`, `index` not `i`,
  `Value`/`Result` not `T`. Rename existing short names when you touch them.
- **No C-style strings or buffers** — use `std::string` / `std::string_view`,
  never `char[]` / `char*` for text. ImGui text fields bind to `std::string`
  via `imgui_stdlib.h` (linked into the `imgui` target); use those overloads.
- **No naked/owning raw pointers.** `std::unique_ptr` for sole ownership,
  `std::shared_ptr` for shared/handle ownership; lookups return a
  `shared_ptr` (empty on miss), not `T*`. References (`T&`) are fine for
  non-owning params/returns. The only raw pointers allowed are unavoidable
  C-API handles (`SDL_Window*`, the ImGui font byte buffer), and those stay
  RAII-managed (e.g. owned + destroyed by `gui::App`).
- Prefer `const`, value semantics, RAII, `enum class`, and free functions over
  members where the Core Guidelines would.

### Commit messages
- **Conventional Commits** (`feat:`, `fix:`, `refactor:`, `docs:`, etc.).
- Imperative mood; subject ≤72 chars; body explains *why*.
- **Never mention "Claude" / "AI" / "Codex"** in commit text or PR bodies.
- **Never add Claude as an author or co-author.** No `Co-Authored-By: Claude`
  trailer, no "Generated with Claude Code" footer — on commits or PRs. The
  commit author is the user, full stop.

### Query layer (dialect-agnostic)
Keep the query frontend and execution separate: dialects parse to the generic
AST; the `Executor` only ever sees the generic AST. Add a database format by
implementing `Dialect`, not by special-casing the executor or UI.

### Documentation — update it with EVERY change (not optional)
Docs are **part of the change, in the same commit** — never a follow-up. After
any change to behavior, architecture, build, CLI, layout, or conventions, update
**every relevant doc surface**:

- **This `CLAUDE.md`** — architecture, code layout, run commands, conventions.
- **`README.md`** — quick start / requirements / feature list.
- **`docs/*.md`** — the matching page(s): `architecture`, `storage`, `query`,
  `large-objects`, `runtime`, `cluster`, `build` — and keep `docs/README.md`
  (the index) in sync. Long-form background/research goes in `docs/research/`
  with a provenance header (source + date + status).
- **`modules/README.md`** — when a vendored module changes.
- The relevant **`.claude/skills/*/SKILL.md`** (especially `zetta-dev`) — when a
  convention or workflow changes.

Before committing, **grep the docs for stale terms** (renamed symbols, removed
flags): if you renamed or removed something, no doc should still mention the old
name. "Update the docs" means *all* of the above, not just `docs/`.

### Committing + pushing
- **Always commit and push after any major work.** When a meaningful unit of
  work is done and verified, `git commit` it and `git push` to `origin/main` —
  don't leave finished work uncommitted or unpushed. Push directly to `main`
  (this is a personal repo; no PR ceremony).

### Knowledge lives in git — do NOT use the agent memory store
- **Do not use the file-based memory system** (the `~/.claude/.../memory/`
  directory, `MEMORY.md`, "save this to memory", etc.). It lives **outside the
  repo**, so it is **not reproducible** — a fresh clone, another machine, or
  another contributor never sees it. Anything worth remembering about this
  project goes into **git-tracked files**: `CLAUDE.md` (conventions,
  architecture, rules), `docs/*.md`, `README.md`, or the relevant
  `.claude/skills/*/SKILL.md` (those *are* committed). If asked to "remember"
  something, write it to the right git-tracked doc instead — never to memory.

### Idle loop
The headed loop uses `SDL_WaitEventTimeout` (≈30fps idle cap) rather than a busy
vsync spin — it wakes immediately on input so interaction stays full-speed but
doesn't burn CPU on identical frames. The headless loop ticks at 10 Hz. Keep any
background work cheap enough to run inside those cadences (or move it off the
loop thread).

## Pointers

- [`docs/architecture.md`](docs/architecture.md) — node, run modes, data model, module map.
- [`docs/projects.md`](docs/projects.md) — the AI project (`zetta::Project`) data model + lifecycle CRUD.
- [`docs/project-inputs.md`](docs/project-inputs.md) — project inputs (`zetta::ProjectInput`): raw intent/artifact ingestion + provenance (replicated).
- [`docs/n8n-import.md`](docs/n8n-import.md) — importing an n8n workflow JSON as an artifact input (raw bytes + sha256, schema-validated at the boundary).
- [`docs/n8n-compile.md`](docs/n8n-compile.md) — compiling an imported n8n workflow JSON into the ExecutionGraph IR (`zetta::compile_n8n_workflow`: nodes→typed steps, connections→edges; FEAT-2 / #308).
- [`docs/new-project-screen.md`](docs/new-project-screen.md) — the headed New Project screen (SCR-2): name + paste NL intent + import/drag-drop an n8n workflow artifact, in one atomic create (FEAT-1).
- [`docs/runs.md`](docs/runs.md) — the Run (`zetta::Run`) record: status/logs/outputs/lineage + JSON-Schema validation.
- [`docs/models.md`](docs/models.md) — the Model (`zetta::Model`) entity (ENT-14/FEAT-6): replicated `model:<uuid>` metadata + serialized weights as a peer-to-peer large-object body (sha256-verified) + the `system.zetta_models` registry.
- [`docs/access-control.md`](docs/access-control.md) — key-scoped access (V1): `Principal` + `AccessKey` + `Scope` (resources) + `Capability` (rights, TTL'd), deny-by-default authorization (RULE-3 / ENT-22).
- [`docs/audit-trail.md`](docs/audit-trail.md) — the immutable, append-only `AuditEvent` (ENT-23 / FEAT-9): graph change / deploy / data-access events, replicated + JSON-Schema validated.
- [`docs/admin-panel.md`](docs/admin-panel.md) — the headed **Admin & Settings panel** (`App::draw_admin`, SCR-7 / CMP-5): read-only view of principals + keys, scope + capability grants, and the audit log.
- [`docs/storage.md`](docs/storage.md) — on-disk format + storage-engine seam.
- [`docs/query.md`](docs/query.md) — the dialect-agnostic query layer + Postgres subset.
- [`docs/execution-graph.md`](docs/execution-graph.md) — the ExecutionGraph IR (nodes/edges/step-kinds).
- [`docs/intent-compiler.md`](docs/intent-compiler.md) — compiling a natural-language intent into a reviewable, editable ExecutionGraph skeleton (`zetta::compile_intent`; templated churn mapping, FEAT-2 / #311).
- [`docs/validation.md`](docs/validation.md) — the `ValidationReport` correctness-gate result type (ENT-5/FEAT-3).
- [`docs/gate.md`](docs/gate.md) — the correctness-gate runner (`zetta::CorrectnessGate`): checks → report, block deploy on failure (RULE-1).
- [`docs/gate-checks.md`](docs/gate-checks.md) — the concrete graph checks (`schema_type`, `data_leakage`): schema/type vs. source profiles + data-leakage / point-in-time for ML steps (#309).
- [`docs/entity-resolution-check.md`](docs/entity-resolution-check.md) — the entity-resolution correctness eval (`zetta::EntityResolutionCheck`, TC-4): fail the gate on account conflation (FedEx-Freight).
- [`docs/churn-dataset.md`](docs/churn-dataset.md) — the synthetic UC-1 churn dataset fixture + loader (`zetta::ChurnDataset` / `load_churn_dataset`, FEAT-6 / #402): committed accounts + comms/events (JSONL/CSV), the FedEx entity-resolution case, schema-validated, no foreign runtime.
- [`docs/deployments.md`](docs/deployments.md) — placing a graph onto a target (`DeploymentTarget`; R1 local), recorded + queryable.
- [`docs/deployment-controller.md`](docs/deployment-controller.md) — the control plane (`zetta::DeploymentController`): reconcile a local deployment desired→actual + self-heal a killed/failed run (FEAT-4/TC-3; R1 local).
- [`docs/execution.md`](docs/execution.md) — executing a graph into a Run (`zetta::GraphExecutor` / `Node::run_graph`, `StepExecutor` seam).
- [`docs/solution-catalog.md`](docs/solution-catalog.md) — the AI Solution Library (`zetta::SolutionCatalog`) + the headed Deploy panel: one-button deploy with a placement dial (FEAT-4 / SCR-5 / CMP-3).
- [`docs/signal-plane.md`](docs/signal-plane.md) — the ephemeral signal plane (`zetta::Signal`): RPC / events / tasks over the cluster transport, addressed by node id, schema-validated at the boundary (ADR-28 / ARC-10).
- [`docs/provisioner.md`](docs/provisioner.md) — the `Provisioner` seam + node capability model (`zetta::NodeCapability`): provision N nodes against a capability spec / teardown / list; cores / GPU type+throughput / memory (ARC-13 / ENT-32, #373).
- [`docs/build.md`](docs/build.md) — build, run, test.
- [`docs/research/`](docs/research/) — long-form research/design deep dives
  (e.g. [coroutines.md](docs/research/coroutines.md)).
- [`README.md`](README.md) — quick start.

## Skills

Project skills live in `.claude/skills/` (committed, shared):
- **zetta-dev** — these coding conventions as a working checklist.
- **new-input** — how to add a new input language/format (parser + executor that
  re-implements its behavior in zetta's runtime; e.g. Python `load_dataset` → a
  table).
- **skill-writer** — author new skills.
- **perplexity** — generate a long Perplexity deep-dive query from a topic.
- **github-ops** — manage issues/PRs/CI for `homer6/zetta` via `gh` (the backlog
  lives in GitHub issues; priority `p1`–`p4` + area labels).
