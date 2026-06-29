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
- `modules/oramacore-cpp` — the **C++23 port of OramaCore's full-text engine**
  (BM25F); backs the `OramaCoreFulltextIndex` search backend
  (`src/core/index/oramacore_fulltext_index.cpp`). It is a **private** repo, so its
  CMake link is **guarded** on the submodule's presence (`ZETTA_WITH_ORAMACORE`): a
  checkout without it still builds/runs green (the backend degrades to
  `Unsupported`). CPU-only.

The first three track their upstream branches and need **Clang 17+** (hence the
Homebrew-Clang toolchain). **Clone with submodules** — `git clone
--recurse-submodules`, or `git submodule update --init --recursive` in an existing
checkout — or the build fails at `add_subdirectory(modules/capy)`. (`oramacore-cpp`
is the exception: its link is guarded, so missing it only disables the full-text
backend, not the build.) Update a vendored module with `git submodule update
--remote modules/<name>` and commit the new gitlink. `modules/README.md` is the
index.

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
table, a **Trained models** section, and a lineage section with a confidence bar;
the GUI slice of FEAT-5 / SCR-6 / CMP-4). The **Trained models** section is the
FEAT-5 "surface the trained Model + metrics + ranked drivers" slice: it resolves
the `Model`(s) the run produced via the durable reverse link
(`Model::run() == run id`) and renders each one's metrics (name→number, a bar for
`[0,1]` scores) and **ranked drivers** (feature importances, strongest |influence|
first — read tolerantly from the model's free-form `feature_schema.drivers` via
`zetta::ranked_drivers`, so no new JSON boundary). See [docs/runs.md](docs/runs.md).

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

A node also owns **indexes** (`zetta::Index`) — a registered **index over a
table** (**ENT-39 / ENT-40, ADR-34**): its definition `{ table, kind, backend,
fields }`, where `kind` is the index type (`search`/`vector`/`feature`, mirroring
the `StepKind::Index` "build a search / vector / feature index" taxonomy) and
`backend` names the `IndexBackend` that builds it. Another top-level entity keyed
by id (like `Model`, **minus the weights body** — an index is metadata only):
`create_index`/`find_index`/`drop_index`/`indexes`, **no value mutators** (an
index is *rebuilt*, not re-specified — changing what it indexes is a new index).
Replicated state (flat key `index:<uuid>`, on-disk `<index-uuid>/index.json`,
projection reconciled like `Model`), **boundary-validated against a formal JSON
Schema** (`parse_index_value`, the same `pboettch` path as Run/Model; `table` +
`backend` required + non-empty, `kind` a closed enum, `fields` a non-empty array
of unique non-empty strings). The registry is `Node::indexes()` + the read-only
SQL **`system.zetta_indexes`** table. **Building / querying an index is the
pluggable `IndexBackend` seam** (`include/zetta/index/index_backend.hpp`,
namespace `zetta`) — a pure-virtual **build (over a table's partitions) / query /
drop** interface, the same shape as `StorageEngine`/`ComputeDevice`, every call
returning an `IndexStatus` rather than throwing. The **`IndexRegistry`**
(`index_registry.hpp`) is the per-node container + selector over the seam (like
`compute::DeviceRegistry`): backends register into it and it **resolves** an
`IndexSpec` — or an **IR `index` step** (`index_spec_from_node` reads the step's
`{table,kind,backend,fields}` attributes at the boundary) — to the registered
backend whose `name()` **and** `kind()` match. The **`InMemoryIndexBackend`**
(`in_memory_index_backend.hpp`, name `memory_search`) is the from-scratch
in-memory **Search** reference backend (an inverted field-value→row-ids index over
the partitions; the `HostStubDevice`/`CpuDevice` role for this seam), so the seam
+ registry are end-to-end usable; vector/semantic/feature backends are additive
behind the same interface. The **first *real* backend** is
**`zetta::OramaCoreFulltextIndex`** (`oramacore_fulltext_index.hpp`, name
`oramacore_fulltext`, kind `Search`, **#527**) — a **full-text (BM25F)** Search
backend backed by the native **oramacore-cpp** engine (the C++23 port of
OramaCore's full-text core, vendored at `modules/oramacore-cpp`): where
`InMemoryIndexBackend` is exact-value lookup, this tokenizes + stems + stop-word-
filters both sides and **ranks** rows by BM25F relevance (`build` indexes each row's
field text into an `oramacore::index::MemoryIndex`; `query` reads `term` as the
free-text query and returns row ids ranked by score). The heavy engine is held
behind a **pImpl** (no oramacore-cpp type in the header) and linked **PRIVATE**, and
the whole integration is **guarded on the submodule's presence** (`ZETTA_WITH_ORAMACORE`)
— oramacore-cpp is a *private* submodule, so a CPU-only checkout that lacks it still
configures/builds/runs green (the backend degrades to `Unsupported`;
`engine_available()` reports which). This slice is the **seam + registry + the
in-memory reference + the first real (full-text) backend + the replicated/validated/
queryable entity**. **Running a deployed `index` step** through the resolved
backend (build over the real table, serve queries) is the
**`IndexStepExecutor` slice** (`include/zetta/index/index_step_executor.hpp`,
**#528**): the `Node` now owns an `IndexRegistry` (`Node::index_registry()`, with
the reference backends registered — `memory_search` always, `oramacore_fulltext`
when `ZETTA_WITH_ORAMACORE`), and `IndexStepExecutor` is the `StepExecutor` that
resolves an IR `index` step to its backend (`index_spec_from_node`), reads the
source table's rows into partitions (`read_index_partitions`), **builds the real
index**, records the `Index` entity, and emits an `index` Run output referencing
the real id — leaving the index **queryable** for the node's lifetime
(`node.index_registry().resolve(spec)->query(...)`). Other step kinds delegate to
`DefaultStepExecutor`. The vector/semantic/feature backends remain later slices
behind the same seam. See
[docs/index-backend.md](docs/index-backend.md),
[docs/index-step-executor.md](docs/index-step-executor.md) and
[docs/oramacore-fulltext-index.md](docs/oramacore-fulltext-index.md).

The **distributed (fan-across-nodes) build + query** is the
**`zetta::DistributedFulltextIndex` slice** (`include/zetta/index/distributed_fulltext.hpp`,
**#529**, the engine half of TC-13 #503) — the **approved per-partition topology**:
where `IndexStepExecutor` builds **one consolidated** index, this builds a full-text
**sub-index per LOCAL partition** (compute-near-data — each node enumerates the
[partitions it OWNS](docs/compute-near-data.md), reads each body from local disk,
builds one sub-index per partition into an owned `IndexBackend`) and answers a query
by **scatter-gather + merge/rank**: query the node's own sub-indexes, **scatter** the
query to every peer over the `fulltext.query` [signal-plane](docs/signal-plane.md) RPC
(no new wire verb; request/response schema-validated like the `CollectiveMessage`),
**gather** each node's ranked hits, and **merge** into one global ranking (identical on
every node, an unreachable peer fails the round). The pure `merge_fulltext_hits`
(score desc, row id asc, dedup-by-max, `top_k`) is the core both the per-node and
cross-node merges run through, so single-node == multi-node. The acceptance is that
the distributed ranking **matches a single consolidated-index oracle** (the standard
sharded-search local-IDF query-then-fetch — exact for the global top-k when ranking is
tf-unambiguous; a global-statistics `dfs_query_then_fetch` pass is a later
optimization). To support the merge, the seam's `IndexQueryResult` now carries per-hit
`scores` (a ranked backend fills them, exact-match leaves them empty). It is a usable
building block alongside (not yet a replacement for) the consolidated index-step path;
wiring it as the default `index`-step behaviour is a later slice. See
[docs/distributed-fulltext.md](docs/distributed-fulltext.md).

A node also owns **compute jobs** (`zetta::ComputeJob`) — the record of **one unit
of native distributed accelerated compute** (the foundational entity of the
accelerated-compute pass for UC-1, **ARC-11 / ENT-31**): a `kind`
(`featurize`/`train`/`explain`/`score`/`ddf`), a lifecycle `status`
(`Queued`→`Running`→`Succeeded`|`Failed`), optional `project`/`graph`/`run` and
`scaling_policy` (reward) **id references** (the `ScalingPolicy` value type is
built — see below — but a job references it by id only; persisting/replicating
policies as their own entity is a later slice), and an optional `detail` note. Another
top-level, replicated entity (flat key `job:<uuid>`, on-disk `<job-uuid>/job.json`),
**modeled exactly like `Run`** but keyed by id (jobs have no human name):
`create_job`/`find_job`/`drop_job`/`jobs`, with the "update" half being the
`set_status` mutator on the handle (capture into the CRDT, persist). Boundary-
validated against a formal JSON Schema (`parse_compute_job_value`, the same
`pboettch` validator as Run). It is **audited at create + finish** via the existing
`AuditSink`: `Node::create_job` records a `job_created` event, and
`ComputeJob::set_status` records a `job_finished` event on the **first** transition
into a terminal state (the `materialize_*`/`apply_value` reconcile paths never emit
— a peer's action arrives already-audited). The registry is `Node::jobs()` + the
read-only SQL **`system.zetta_jobs`** table (job id / kind / status / refs / reward
+ observing node, computed from `Node::jobs()`). The headed app surfaces the
registry in a **read-only Compute Jobs panel** (`App::draw_jobs` — list of jobs
with a color-coded status badge + kind, and a detail pane with the selected job's
status / refs; the GUI form of `system.zetta_jobs`, FEAT-5 / SCR). The detail pane
also carries **per-job objective/scaling controls** (`App::draw_job_scaling_controls`)
— a **live preview** that scores an editable `ScalingPolicy` + sample against the
built `ScalingPolicy::evaluate()`: pick a reward objective + bounds, add/remove
nodes, and see the resulting `ScalingDecision` (recommended nodes, scale
up/hold/down, reward, cost-cap). It is a what-if calculator, **not** a write path —
nothing is persisted (policy persistence + driving this preview from the live
autoscaler control loop below are later slices).
Scheduling/running a job (the coordinator, scheduler + collectives) and the
autoscaler control loop that sizes the fleet from the `ScalingPolicy` reward model
(the **job autoscaler** below) are built; wiring the autoscaler's live observation
source into the coordinator for an end-to-end autoscaled job is a later slice. See
[docs/compute-jobs.md](docs/compute-jobs.md) and [docs/jobs-panel.md](docs/jobs-panel.md).

A compute job's `scaling_policy` ref points at a **`zetta::ScalingPolicy`** — the
per-job **reward objective an autoscaler optimizes** (**ADR-29 / ENT-34**, the
same accelerated-compute pass). A policy picks one `ScalingObjective` —
`minimize_cost`, `runtime_target` (finish by `target_seconds`, runtime/latency),
or `data_shape` (size the fleet to the work) — inside hard `ScalingBounds` (a
`min_nodes`/`max_nodes` envelope + a `cost_cap`). It is a **pure value type +
boundary validation** modeled like the sibling `NodeCapability`/`ProvisionedNode`
value types (a `to_json` / non-throwing `from_json` boundary that **mirrors**
`schemas/scaling_policy.schema.json`, **not** the `pboettch` path — that is for
top-level replicated entities), **plus** the objective evaluation:
`evaluate(ScalingSample{progress, elapsed, cost, nodes})` is a pure, deterministic
function returning a `ScalingDecision` (`reward` — higher is better, the
autoscaler maximizes it; `recommended_nodes` clamped to the bounds; a
`scale_up`/`hold`/`scale_down` `action`; `within_cost_cap`; a reason). The cost
cap is a **hard constraint** — a breach sheds the fleet to `min_nodes` regardless
of objective. This slice is the pure reward model only — no replication, no `Node`
CRUD (those are later); the control loop that *acts on* `evaluate()` is the **job
autoscaler** below. See [docs/scaling-policy.md](docs/scaling-policy.md).

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
for every **graph change / deploy / data access / compute-job lifecycle /
access-key issue+revoke / provision-burst approval** event
(ENT-23 / FEAT-9; the audit half of governance, on top of the key-scoped access
above). An `AuditEvent` (id + `action`
graph_created|graph_dropped|graph_deployed|data_access|job_created|job_finished|key_issued|key_revoked|provision_requested +
`actor` + `target` + `allowed` + `detail` + `time`) is a top-level, replicated
entity (flat key `audit:<uuid>`) — but **immutable and append-only**: no mutator,
no drop, so the set of `audit:` keys only grows and the CRDT converges trivially.
`Node` owns the trail and **is** the `AuditSink` seam: `record_audit_event` is the
append API (it stamps id + time, captures into the CRDT, persists),
`audit_events()` lists them chronologically. Emission is wired at the governed
seams — `Project::create_graph`/`drop_graph` (via the `AuditSink*` threaded into a
project like its `ReplicationManager*`), `Node::deploy_graph`, `Node::authorize`
(which is therefore **non-const**: every allow/deny decision records a
`data_access` event), the compute-job lifecycle (`Node::create_job` →
`job_created`, `ComputeJob::set_status` → `job_finished` on the first terminal
transition), the burst-identity lifecycle (`BurstIdentityManager::mint` →
`key_issued`, `revoke` → `key_revoked`; see below), plus the **provision-burst
approval gate** (`BurstApprovalGate::provision` → `provision_requested`, recorded
only when a burst exceeds the cost cap — see the Provisioning section). The
`materialize_*` reconciliation paths never emit (a peer's action arrives
already-audited as its own replicated `audit:` event).
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

A node also owns a **partition catalog** (`zetta::DatasetPartition`) — a record of
**where a slice of a table's data physically lives** (**ENT-36 / TBL-13**): its
`table` (a `tbl:` id), the `blob_ref` large-object body (a `lo:` id) holding its
bytes, the `row_range` it covers, free-form column `stats` for pruning, and the
`owner_nodes` that hold a copy. Another top-level, replicated entity (flat key
`part:<uuid>`, on-disk `<part-uuid>/partition.json`), **modeled exactly like
`ComputeJob`** but keyed by id: `create_dataset_partition`/`find_dataset_partition`/
`drop_dataset_partition`/`dataset_partitions`, boundary-validated against a formal
JSON Schema (`parse_dataset_partition_value`, the same `pboettch` validator).
The catalog is **replicate-to-all** so placement is **globally visible** — a
partition recorded on one node reconciles onto every node (`reconcile_partitions`,
alongside the `lo:` block) and is queryable everywhere via the read-only SQL
**`system.dataset_partitions`** table (computed from `Node::dataset_partitions()`).
The catalog's `owner_nodes` is now **acted on** by **sharded blob placement**
(`zetta::ReplicatedBlobStore`, **#515**): a `Partitioned`/`PartitionedWithReplicas`
table's partition **body** (`blob_ref`) is fetched + held **only by its owner
node(s)** per the catalog + replication factor (`rf` owners → exactly `rf` copies),
not replicate-to-all — `missing_blobs()` skips a non-owner's partition body (so it
never `LOREQ`s it) and `store_blob()` refuses it; a plain large object, an unplaced
partition (empty `owner_nodes`), and model weights keep replicate-to-all. The
store reads `owner_nodes` from the CRDT `part:` snapshot through the validated
boundary (network-thread-safe, no Node access). The *partitioner* that splits a
table and writes the bodies (#511) and the **rebalance** that re-places them on a
membership change (#517) are built (see below); locality-aware reads (#516) are a
later slice. See [docs/dataset-partitions.md](docs/dataset-partitions.md) and
[docs/sharded-blob-placement.md](docs/sharded-blob-placement.md).

### Partitioning (`zetta::Partitioner`)

A table can be **partitioned** behind the **`Partitioner`** seam
(`include/zetta/partitioner.hpp`, **ADR-31 / ENT-37**, #512) — the pluggable,
per-table policy that decides **data placement**, the way `StorageEngine` decides
how rows are stored. It has two pure, non-throwing responsibilities over a
**fixed partition count `P` (chosen `P >> nodes`)**: **record → partition**
(`assign_partition(record) -> PartitionId`, always in `[0, P)`) and **partition →
node(s)** (`place_partition(partition, members) -> owner node id(s)`, the first
the primary owner + any replicas, given the current cluster `members` from
`Cluster::nodes()`). Holding `P` fixed is what lets rebalancing move whole
partitions (as blobs) between nodes on a membership change rather than rehashing
every row, and determinism (every node computes the same placement, no election)
is what lets the leaderless cluster agree. A `Table` is **configured per-table**
with one via `set_partitioner`/`partitioner` (empty handle = not partitioned, the
default). The trivial baseline `zetta::SinglePartitioner` is the identity — one
partition holding every row, owned by **every** member (replicate-to-all) — so
configuring it changes nothing while exercising both halves of the seam. The two
concrete **record→partition** strategies (#513) are `zetta::HashPartitioner`
(`hash(key columns) % P` — the standard shard-by-key, deterministic for a given
key; a stable 64-bit **FNV-1a** over the key columns, never `std::hash` whose
value is unspecified across runs/nodes — and roughly uniform, so balanced for a
typical key) and `zetta::RoundRobinPartitioner` (`surrogate % P` — for **even
balance regardless of key skew**: a monotonic integer surrogate the write path
stamps on each row, default field `_seq`, so a sequence 0,1,2,… cycles across the
partitions; it stays deterministic/side-effect-free by riding the surrogate on
the row rather than a hidden counter). Both place **replicate-to-all** for now —
composing the placement half is the write path's (#511) job. The
**partition→node** strategy is **`zetta::RendezvousPlacement`**
(`include/zetta/rendezvous_placement.hpp`, **#514**) — **rendezvous /
highest-random-weight (HRW)** placement: each member is scored
`hash(partition, member)` (FNV-1a) and the `replication_factor` highest-weight
members own the partition (highest = primary, rest = replicas), so
**adding/removing one node out of N reassigns only ~1/N of partitions** (whole
partitions move, no full rehash), pure + deterministic + order-independent so the
leaderless cluster agrees with no election. Its reusable HRW core is the free
function `zetta::rendezvous_owners` (the #513 strategies will reuse it for
placement without subclassing); its `assign_partition` is a documented FNV-1a
hash-of-record stand-in. #513 also adds a **JSON config boundary** for the
record→partition strategies — `to_json()` (per-strategy, on Single/Hash/RoundRobin)
+ `partitioner_from_json()` (the hand-written non-throwing boundary mirroring
`schemas/partitioner.schema.json`, the `ScalingPolicy`/`NodeCapability` value-type
pattern, **not** the pboettch path). The **universal, polymorphic** "which
partitioner is this table using" config (covering `rendezvous` + composed
strategies, serialized through a base pointer) is deliberately the partitioned
write path's (#511) holistic concern, alongside the **partition catalog**
(`system.dataset_partitions`, #510) and rebalance on membership change (#517); a
learned **model-as-partitioner** is a documented future direction behind the same
seam. See [docs/partitioner.md](docs/partitioner.md).

The **partitioned write path** is **`zetta::write_partitioned`**
(`include/zetta/partition_writer.hpp`, **#511**) — rows → partition → blob,
placement recorded: the slice where the seam (#512–#514), the
[large-object store](docs/large-objects.md), and the [partition catalog](docs/dataset-partitions.md)
(#510) meet. For a table configured with a `Partitioner` (the operational signal a
table is partitioned), it **assigns each row to a partition** (`assign_partition`;
stamping a monotonic `_seq` surrogate = the row's table ordinal onto the
assignment record only, so `RoundRobinPartitioner` distributes evenly without the
caller pre-stamping — `HashPartitioner`/`SinglePartitioner` ignore it),
**serializes each non-empty partition's rows** to a JSON Lines blob (one
`{"_zid",doc}` line per row, the `JsonlStore` shape — a self-describing slice of
the table), **stores the bytes as a large object** (the new
`LargeObjectStore::import_bytes`, so the body is sha256-fingerprinted, its `lo:`
metadata rides in the CRDT, and it gossips peer-to-peer like any LO body), and
**records placement** as a `DatasetPartition` catalog entry per partition
(`Node::create_dataset_partition` → `part:<uuid>`, visible in
`system.dataset_partitions`): `blob_ref` (the body id), `row_range`, `owner_nodes`
(from the partitioner's `place_partition` over the cluster `members`, defaulting to
this node), and `stats` (`{partition, rows}`). The **`row_range` is the
partition's cumulative slice in partition-id order** — partition 0 = `[0, n0)`, the
next = `[n0, n0+n1)`, … so the ranges tile `[0, rows_written)` (and reduce to the
whole table for `SinglePartitioner`); a hash partition scatters its rows across the
table's original ordinals, which a single range can't name, so the catalog names
the slice in the partitioned/re-numbered row space. Only **non-empty** partitions
get a blob + catalog row (N blobs + N catalog rows — the acceptance). **No new JSON
format** is parsed back in: the catalog entry rides the existing `DatasetPartition`
schema boundary, and the blob is an LO body (opaque, size+sha256-verified — reading
partitions back is the locality-read slice, #516). A reusable library function
today (like `import_records`); a load-triggering command/DDL and driving the table's
`ReplicationStrategy` factor into placement are later slices.
See [docs/partitioned-write-path.md](docs/partitioned-write-path.md).

The **between-jobs partition rebalance** is **`zetta::rebalance_partitions`**
(`include/zetta/partition_rebalance.hpp`, **#517**; satisfies TC-10/#500) — the
sibling of `write_partitioned` that **recomputes placement on a membership change**.
When the cluster gains a node, for each **placed** catalog partition it re-runs
**rendezvous (HRW)** placement (`rendezvous_owners(stats.partition, members, rf)`,
the same core the write path placed with) **preserving the replication factor** the
entry records (`rf = owner_nodes.size()`, clamped to the live membership — the
catalog, not the table's declarative `ReplicationStrategy`, is the faithful `rf`
signal since any `Partitioner` could have placed it), and on a change captures the
new owners via the new **capturing** `DatasetPartition::set_owner_nodes` (rewrites
the value, captures `part:<id>` into the CRDT, persists). So **≈rf/N partitions move
onto the new node** (whole partitions, no full rehash) and every row stays readable
exactly once with `rf` owners. The **bodies move for free**: an updated `owner_nodes`
makes the new owner list the body in [`ReplicatedBlobStore::missing_blobs()`](docs/sharded-blob-placement.md)
(it `LOREQ`s it) and a non-owner refuses it — no new transport. **Replicate-to-all**
partitions (`SinglePartitioner`/`Hash`/`RoundRobin`, owned by exactly the previous
membership) are **left untouched** (sharded rebalance is not their job). It fires
from **two composing triggers**. (1) The **direct trigger** — the `add_nodes(Node&,
…)` overload (**#519**, below) calls `rebalance_partitions` straight after a
provision, so the data spreads onto the new permanent members **synchronously**
(`previous_members` = `Node::cluster_members()`, `members` = that set + the
just-provisioned ids) — verifiable immediately in `system.dataset_partitions`. (2)
The **auto-trigger inside `Node::tick()`**: membership is replicated state (each node
seeds its own `node:<id>` key, peers merge), so `Node::rebalance_partitions_on_membership_change`
reads the `node:` set from the snapshot it already reconciles, compares to the
baseline set in `Node::start()`, and on a delta drives `rebalance_partitions` between
jobs (no `Cluster` plumbing) — the **safety net** that re-confirms against the real
replicated membership once the new nodes gossip in (idempotent; the direct pass
leaves `last_membership_` untouched so its own document change doesn't re-fire it).
Append-only `node:` keys today mean the auto-trigger fires on **joins**
(leave-driven rebalance + physical GC of a demoted owner's stale body +
mid-job/elastic rebalance are later slices). **No new JSON format** (the catalog
entry rides the existing `DatasetPartition` boundary). **TC-10/#500** is verified
end to end by `tests/test_partition_rebalance.cpp::CatalogConvergesOnEveryNodeAfterJoin`
(two replicated nodes — the rebalanced placement converges, identical, in each
node's `system.dataset_partitions`) + `LeaveReassignsPartitionsAwayFromDepartedNode`
(the library-level leave path), and as the runnable `cluster.partition_rebalance`
test-suite case. See [docs/partition-rebalance.md](docs/partition-rebalance.md).

### Async runtime (`zetta::runtime`)

zetta's async / coroutine / networking layer lives behind
`include/zetta/runtime.hpp` (namespace `zetta::runtime`). Code uses `zetta::runtime::task<T>`,
`zetta::runtime::block_on`, `zetta::runtime::thread_pool`, etc. and **never names
`boost::capy` / `boost::corosio` directly** — so the (beta, churning) C++
Alliance backend can be swapped, or replaced by Boost.Asio, by editing that one
header. Backed by Capy today (`modules/capy`); Corosio (`modules/corosio`) adds
networking. The seam also exposes **`runtime::parallel_for`** — a blocking
data-parallel primitive (`src/core/runtime.cpp`) that tiles `[0, count)` into
contiguous chunks over one shared, process-wide thread pool and joins before
returning, so CPU-bound code (the `CpuDevice` operators today; the job scheduler /
collectives later) fans work across cores without naming `boost::capy`. Its tiling
is a pure function of `count` (so multi-pass algorithms line phases up), and
bodies **must not throw** (a throwing body aborts — zetta-core never unwinds across
the seam). Background + migration plan: [docs/runtime.md](docs/runtime.md) and
[docs/research/coroutines.md](docs/research/coroutines.md).

### Accelerated compute (`zetta::compute`)

The **`ComputeDevice` interface** (`include/zetta/compute/compute_device.hpp`,
namespace `zetta::compute`) is the seam every heavy compute operator targets —
the operator-level accelerator interface of **ADR-27 / ARC-9** (the native
distributed accelerated-compute pass for UC-1 churn, #359). Same shape as zetta's
other seams (`StepExecutor`, `Check`, `StorageEngine`): a **pure-virtual base
class** whose concrete implementations are the **backends**, so consumers call
the interface and stay unchanged when new hardware appears (**new hardware = a new
backend, nothing else**). It has two halves. (1) A **memory model**: a
`zetta::compute::DeviceBuffer` is a **typed, owning, move-only** handle to a
region of device memory (host RAM for a CPU device, GPU VRAM for a CUDA device) —
a `unique_ptr` for device memory that frees itself through its owning device
(`make_buffer`/`release_buffer`; the opaque storage handle is the one allowed raw
pointer, a C-API resource handle, fully RAII-encapsulated). A buffer carries a
runtime `DataType` tag (`Float32`/`Int32`/… — virtuals can't be templated, so a
buffer is *typed* without templating the interface; backends dispatch kernels on
the tag), and the device offers explicit host⇄device transfer
(`copy_from_host`/`copy_to_host`, with typed `upload<T>`/`download<T>` helpers)
plus `synchronize()`. (2) An **operator set** declared at the largest grain that
still lets the cluster coordinator distribute work: **map/elementwise,
reduce/scan, histogram, gemm/axpy, sort/sort_by_key, gather/scatter** (DDF and
GBDT/SHAP ops are added by their own issues against this interface). Every call
returns a `zetta::compute::ComputeStatus` rather than throwing (zetta-core is
`-fexperimental-library`; no unwinding across the seam) — `Ok` or
`Unsupported`/`InvalidArgument`/`OutOfMemory`/`BackendError` + a reason; a backend
that lacks an op returns `unsupported` (the status **is** the per-op capability —
no parallel bitmask to drift). **No SDL/CUDA type appears in any header** and the
**default build is CPU-only** (no GPU toolkit needed); a CUDA backend ships behind
a build guard, not in the public surface. The implementers today:
`zetta::compute::HostStubDevice` is a host-memory transfer stub (real allocate +
memcpy transfer, operators return `unsupported`) that lets the memory model
round-trip in tests — **not** a compute backend.
`zetta::compute::CpuDevice` (`include/zetta/compute/cpu_device.hpp`, **#360**) is
the **always-built reference backend**: it implements **every** operator on host
memory, parallelizing the data-parallel ones
(map/elementwise/reduce/scan/histogram/gemm/axpy/gather) across the
`zetta::runtime` thread pool via `runtime::parallel_for`, with `sort`/`sort_by_key`
(comparison sort) and `scatter` (write-race-free on duplicate indices) kept
sequential. Float-only ops (sqrt/exp/log, gemm, axpy) reject integer dtypes;
everything returns a `ComputeStatus`, never throwing across the seam. **Its results
are the numerical reference `zetta::compute::CudaDevice` (#361), the GPU backend,
is checked against** (correctness over throughput) — `tests/test_cuda_device.cpp`
is the CPU↔CUDA conformance suite (guarded on `ZETTA_WITH_CUDA`, skipped with no
GPU) that runs every operator on both backends and asserts they agree, including
the edge-case parity (integer-dtype rejection, safe integer divide, out-of-range
`gather`/`scatter` rejection, deterministic duplicate-index `scatter`).

The **runtime-free** compute backends (the `HostStubDevice` stub, the optional
`CudaDevice`) plus the `DeviceBuffer` memory model build into a dedicated
**`zetta-compute`** static library linked PUBLIC into `zetta-core` (the build slice
of this pass, **#363**) — the same isolation SDL/ImGui get: a heavy/optional
dependency lives in its own target and **never leaks its headers into
`zetta-core`**. (`CpuDevice` stays in `zetta-core`: it depends on `zetta::runtime`,
which `zetta-core` owns, so it can't sink into the lower-level `zetta-compute`
without a link cycle.) A **CPU-only configure is the default** (dev Macs, CI) and
needs no GPU toolkit; the **CUDA backend is opt-in** behind `-DZETTA_ENABLE_CUDA=ON`,
which `find_package(CUDAToolkit REQUIRED)`, `enable_language(CUDA)`, adds
`src/core/compute/cuda_device.cu` (Thrust + custom
`histogram`/`gemm`/`gather`/`scatter` kernels, dispatched on `DataType`, that
reproduce the `CpuDevice` reference's preconditions + edge cases), links
`CUDA::cudart` PRIVATE (so the CUDA include dirs
never reach `zetta-core`), and sets the public `ZETTA_WITH_CUDA` define consumers
(the device registry #362, the tests) guard `CudaDevice` use on. `cuda_device.hpp`
is CUDA-free (no `<cuda_runtime.h>`), like `src/gui/app.hpp` keeps SDL out of its
header. The CPU-only path is what's verified on macOS/CI; the `.cu` needs a CUDA
toolkit + GPU (a Linux host) to compile/run. See
[docs/compute-device.md](docs/compute-device.md) (interface + build) and
[docs/cpu-device.md](docs/cpu-device.md) (the CPU backend).

The **`DeviceRegistry`** (`include/zetta/compute/device_registry.hpp`, #362) is the
per-node enumeration + selection layer over that seam: backends are **registered
into** it (`add`/`find`/`has`/`at`/`devices`/`device_infos`), it **reports** each
device's `DeviceInfo` capability (and serializes the coordinator-facing report via
`to_json`), and it **selects** a backend per unit of work
(`select(DeviceRequest)` — accelerator-first by default, falling back to the CPU,
with `require_kind` + `min_memory_bytes` filters; `is_accelerator(kind)` is every
kind but `Cpu`). It is a **container + selector**, not a backend factory — it does
**not** manufacture `CpuDevice`/`CudaDevice` (those are #360/#361, and CUDA must
not leak into zetta-core), so the **"CPU always, CUDA if built+present"**
enumeration is the job of the future `zetta-compute` assembly (#363) that `add`s
the concrete backends. The new JSON boundary — a device's `DeviceInfo` serialized
for the coordinator — is **boundary-validated** (`device_info_from_json`/
`parse_device_report`, non-throwing, mirroring `schemas/device_registry.schema.json`,
the same hand-written pattern as `NodeCapability`); the `DeviceInfo` JSON form lives
in the registry module so the pure `ComputeDevice` seam header stays JSON-free. No
`Node`/SQL wiring in this slice (a standalone seam, like `Provisioner`). See
[docs/device-registry.md](docs/device-registry.md).

### DDF DataFrame (`zetta::ddf`)

The **DDF (distributed dataframe) core** (`include/zetta/ddf/data_frame.hpp`,
namespace `zetta::ddf`; **ARC-12 / GLO-11 / ENT-33**, #380) is a typed, columnar
`DataFrame` over the existing storage engine — the **substrate every DDF op operates
on**. A table is untyped JSON rows (`JsonlStore`'s `{_zid, doc}`); the DataFrame
reads a table's rows into typed **numeric** columns (the `zetta::compute::DataType`
set — float/int/uint only; non-numeric fields aren't selected), **chunked** by row
range, with each chunk a `zetta::compute::DeviceBuffer` on a `ComputeDevice` — so the
existing CPU/CUDA operators run directly over a frame's columns. A `Column` is a
sequence of row-aligned chunks and **every column in a frame shares the same chunk
layout** (chunk `i` = the same rows across columns — what DDF ops rely on); the
default `kDefaultChunkRows = 10'000` matches `JsonlStore::kRecordsPerChunk`.
`DataFrame`/`Column` are **move-only** (they own device memory via `DeviceBuffer`).
`load_data_frame(store, schema, device, chunk_rows=…)` is the loader (non-throwing —
returns a `DataFrameResult{frame, ComputeStatus}`; the value conversion is a
**boundary**: missing field / non-number / real-into-int / negative-into-unsigned /
out-of-range → `InvalidArgument`, a float column widens integer JSON); `infer_schema`
picks the dense numeric fields (Int64 if all-integer, else Float64) for
"just load the table"; `to_host<T>` reads a column back (the round-trip, mirroring
`ComputeDevice::download`). The schema config `DataFrameSchema` carries a
**validated JSON boundary** (`to_json`/`from_json` mirroring
`schemas/data_frame_schema.schema.json`, the hand-written non-throwing pattern of
`DeviceInfo`, **not** the `pboettch` path). It lives in **zetta-core** (it bridges
the storage engine + `zetta::compute`), like `CpuDevice`. No `Node`/SQL/replication
wiring in this slice — the `ComputeJob` `ddf` kind is a later issue.
See [docs/ddf.md](docs/ddf.md).

The first **DDF ops over a frame** — `zetta::ddf::scan` / `filter` / `project`
(`include/zetta/ddf/ops.hpp`, **ARC-12**, #381) — are the leaf of the DDF op
algebra. They are **DDF-level free functions built on the `ComputeDevice` operator
seam, not new methods on it** (that interface is the fixed low-level buffer-op set;
DDF ops compose it over a `DataFrame`'s chunked columns). The heavy data movement
runs **through the device's existing operators** so it is accelerated +
CUDA-parityable: **`filter`** moves the surviving rows with the device **`gather`**
op (per source chunk, then re-packs into regular output chunks), and a computed
**`project`** column runs the device **`map`** (unary) or **`elementwise`** (binary)
op. **`scan`** reads a table/store into a frame (the schema-less overload infers the
schema; both thin-wrap `load_data_frame`). The one **host-side** step is evaluating
a `filter` predicate into the selected row indices — the op set has no
comparison/stream-compaction primitive (a deliberate scope note; a select/compare
seam op is a separate issue), so the comparison runs host-side like the loader's
JSON→value conversion. A **`FilterPredicate`** is a single-column comparison
(`column <comparison> value`, each row widened to double); a **`Projection`** is an
ordered set of output columns, each a passthrough / unary / binary `ProjectColumn`
(built via factories). Both carry a **validated JSON boundary** (`to_json` /
non-throwing `from_json` mirroring `schemas/ddf_filter_predicate.schema.json` /
`schemas/ddf_projection.schema.json`, the hand-written `DataFrameSchema` pattern).
Every op is **non-throwing** (returns a `DataFrameResult`). No `Node`/SQL/scheduling
wiring yet — `groupby`/aggregate/featurize and the `ComputeJob` `ddf` kind are
later issues. See [docs/ddf-ops.md](docs/ddf-ops.md).

Alongside those, the frame-shaping ops `zetta::ddf::sort` / `concat` / `slice`
(`include/zetta/ddf/data_frame_ops.hpp`; **ARC-12**, #385): **sort** orders a frame
by a key column — the compare runs on the frame's `ComputeDevice` via `sort_by_key`
(the device returns the permutation, so a CUDA frame sorts on the GPU), the rows
gathered through it and re-chunked; **concat** is row-wise union (bag union,
schema-compatible frames appended in order); **slice** is a contiguous row range.
Each is **non-throwing** (returns a fresh, move-only `DataFrameResult`; the inputs are
read, never mutated), staging through an internal type-erased `HostFrame`
(`src/core/ddf/data_frame_internal.hpp`) so the orchestration stays dtype-generic and
the device only does the compare sort. They are the single-node building blocks of the
**distributed sort** (`zetta::ddf::DistributedSorter`,
`include/zetta/ddf/distributed_sort.hpp`) — **sample-partition + local sort + merge**
across the cluster (**ARC-12**, #385). Every node runs the same SPMD
`sort(local_shard, key, order, round_tag)` against its own shard: local-sort,
**sample** keys → [`gather`](#collectives-allreduce--broadcast--barrier--gather)
to the root which picks `P-1` splitters → [`broadcast`](#collectives-allreduce--broadcast--barrier--gather)
them, **partition** the sorted shard by the splitters, **all-to-all shuffle** bucket
`b` to participant `b` over the [signal plane](#signal-plane-rpc--events--tasks)
(the `ddf.shuffle` RPC — buffered cheaply on the network thread, fenced by a
`barrier`, so the blocking-RPC all-to-all can't deadlock), then **merge** + local-sort
the union. It returns this node's globally range-ordered, locally-sorted **partition**
(one per node; `ddf::concat` in participant order is the full global sort). A
single-node cluster returns the locally-sorted shard. The sorter **is** the cluster's
signal handler, chained over an owned `Collectives` (`ddf.shuffle` here →
`collective.*` → optional app delegate), the same register/unwind discipline as the
job coordinator; no new wire verb. The shuffle payload (`PartitionMessage`) is
**boundary-validated** against `schemas/ddf_partition.schema.json` (the `pboettch`
validator, like the `CollectiveMessage`) — values ride as doubles (exact ≤ 2^53,
the documented bound for 64-bit-integer columns). Driving these through a
`ComputeJob` of `ddf` kind via the [job coordinator](#job-coordinator-zettacompute)
is a later slice. See [docs/ddf-ops.md](docs/ddf-ops.md) and
[docs/ddf-distributed-sort.md](docs/ddf-distributed-sort.md).

Another distributed DDF **op** is the **distributed equi-join**
(`zetta::ddf::distributed_join`, `include/zetta/ddf/join.hpp`; **ARC-12**, #382): an
**inner equi-join** of two `DataFrame`s on a key column, executed across the cluster.
Each participating node holds a shard of `left` + `right` (SPMD); the join (1)
**repartitions by key via collectives** and (2) runs a **local hash or sort-merge
join per shard**. The cluster has no all-to-all/scatter collective, so the
repartition is built from the ones it has
([collectives](#collectives-allreduce--broadcast--barrier--gather)): every node
`gather`s its rows to the root, the root concatenates, a `broadcast` hands the full
relation back to everyone, and each node keeps only its partition
(`std::hash(key) % participant_count`) before joining — **correct-first** (a true
point-to-point shuffle is a later optimization behind the same API, the way
collectives ship a star reduce before a ring/tree). Because equal keys hash to the
same partition, every matching pair lands on exactly one node and is emitted once;
the **global join is the union of every node's returned shard**, so a single-node and
a multi-node run yield **identical results** (a single-node cluster keeps everything =
the whole join). It runs through the
[job coordinator](#job-coordinator-zettacompute)'s `JobRound` (the entry the
DDF/GBDT/SHAP algorithms submit work through) as a `ddf` `ComputeJob`. Values ride as
**float64** (the whole compute/collective plane is double-based — keys/payloads beyond
2⁵³ lose precision); `to_host_relation` widens a frame's typed columns to double. It
introduces **no new JSON format** — the repartition serializes rows into the existing,
schema-validated `CollectiveMessage` (`vector<double>`), and `deserialize_relation`
**range-checks the blob at the boundary** (length must be a whole number of rows) so a
malformed/truncated broadcast fails with a clear `ComputeStatus` instead of corrupting
state. Scope is **inner equi-join only** (left/right/full outer are later, behind the
same entry); no `Node`/SQL/`ddf`-kind wiring (the integration test drives it through an
existing `Ddf` job). The pure pieces (`hash_join`/`sort_merge_join`/`serialize_relation`/
`partition_index`/`filter_partition`) are unit-tested without a cluster. See
[docs/ddf-join.md](docs/ddf-join.md).

Another DDF **operator** is the **distributed groupby-aggregate**
(`zetta::ddf::group_by` / `distributed_group_by`,
`include/zetta/ddf/group_by.hpp`; **ARC-12**, #383): groupby a numeric key column +
aggregates (`count`/`sum`/`min`/`max`/`mean`, an **extensible** op set — add to
`AggregateOp` + the accumulate/merge/finalize switches), **distributed** via
partial-per-node + a cluster combine. A `GroupBySpec` (key + aggregates) carries a
**validated JSON boundary** (`schemas/group_by_spec.schema.json`, the hand-written
non-throwing `from_json` pattern of `DataFrameSchema`, **not** `pboettch`).
`group_by(frame, spec)` is the single-node op; it reads the key + aggregated columns
back through the `ComputeDevice` download seam as doubles (so it is **backend-
agnostic** — same over a CPU or CUDA frame), groups in a `std::map` keyed by the
numeric key (ascending → deterministic; a NaN key is rejected), and accumulates
op-aware (mean carries a running sum + count, divided at finalize). The distributed
half is `distributed_group_by(frame, spec, collective, tag)` — a **template** over
any collective surface exposing `gather` + `broadcast` (both `Collectives` and the
coordinator's `JobRound` qualify, so a SPMD job body calls it directly): each node's
`GroupByPartial` (per group: row count + one accumulator per aggregate) serializes
to a flat `vector<double>`, is **gathered** to the deterministic root, **merged**
there (count/sum/mean add, min/max combine — order-independent), then the merged
partial is **broadcast** back and every node finalizes the same result. The
`gather → merge-at-root → broadcast` shape (not a key-aligned allreduce) is the
natural fit for per-node group sets that differ; it introduces **no new wire
format** (the partial rides the existing schema-validated collective payload, with
`GroupByPartial::from_wire` the boundary that rejects a malformed/duplicate/NaN
partial). Single- and multi-node results are identical (the acceptance test). It
lives in **zetta-core** like the rest of `zetta::ddf`.

The groupby also **runs as a recorded `ComputeJob` of kind `ddf`**
(`zetta::ddf::run_group_by_job`, `include/zetta/ddf/group_by_job.hpp`; **FEAT-8**,
#387) — the slice that wires the op through the
[job coordinator](#job-coordinator-zettacompute) so it is durable, audited
(`job_finished`), and **appears in `system.zetta_jobs`**. It adds no new math: it
calls `distributed_group_by` with the job's `JobRound` (which satisfies the
`Collective` template) and lets the coordinator drive the lifecycle (`Running →
Succeeded | Failed`) and record the job. Called concurrently on every participating
node, each with its own `ComputeJob`; the `round_tag` must be **agreed across nodes**
(a node's own job id can't serve — each node holds a distinct `ComputeJob`) and
defaults to a deterministic spec-derived tag. Because `distributed_group_by`
broadcasts the merged result, the global aggregation lands on **every** node, not the
root alone. It introduces **no new JSON format** — the op spec rides the existing
`GroupBySpec` boundary, the partials the schema-validated `CollectiveMessage`, and the
job record its own boundary. The remaining featurize op (as-of windowing), schedulable
the same way, is a later issue. See [docs/ddf-groupby.md](docs/ddf-groupby.md).

The distributed groupby (and any DDF op) now **executes over a node's LOCAL
partitions** — **compute-near-data** (`zetta::ddf::load_local_partitions`,
`include/zetta/ddf/local_partitions.hpp`; **ARC-12 / FEAT-8**, #516 — the engine half
of TC-11 #501): instead of a shard handed in **out-of-band** (the `ComputeNode` /
`shards[i]` pattern), a node **enumerates the partitions it owns** from the
[partition catalog](#on-disk-layout) (every `DatasetPartition` whose `table` matches
and whose `owner_nodes` contains `node.node_id()`, the in-memory source of
`system.dataset_partitions`), reads only the partition **bodies it physically holds**
from its **local** large-object store (the [sharded-blob-placement](#files-large-objects--import)
guarantee — no `LOREQ`, no full-dataset pull), parses them (the existing JsonlStore
`{"_zid","doc"}` line format the [partitioned write path](#the-partitioned-write-path-is)
emits, sha256-verified at the blob layer — a structural fail-fast boundary, **no new
JSON format**), and loads them into its local `DataFrame` shard. The op then runs over
that shard, exchanging only the per-node **partial** over the collectives — the raw
rows never cross the wire. `zetta::ddf::run_near_data_group_by_job`
(`include/zetta/ddf/local_group_by_job.hpp`) is the first-class near-data op: load
the local shard → `run_group_by_job`, reporting `local_bytes_read` (read near data)
vs `wire_bytes_moved` (the partials, measured by the new `Collectives::wire_bytes()`
meter). So the acceptance is directly checkable — **bytes-moved ≈ partials (not ≈
dataset); result == the single-node oracle** — over a real loopback cluster. **Featurize
is wired the same way** (`zetta::ddf::run_near_data_featurize_job`,
`include/zetta/ddf/local_featurize_job.hpp`, **#532**): load this node's shard of a
single partitioned table → `run_featurize_job` over it (used as the accounts frame),
reporting the same near-data instrumentation — the featurize slice of UC-1 at scale
(the saas-sales-conversations **engineered columns** featurized point-in-time over the
partitioned table; the dropped embeddings never cross the wire, the broadcast feature
table matches a single-node oracle, leakage-free). Group-by + featurize are wired end
to end; `scan`/`filter` reuse the same `load_local_partitions` shard source;
rebalancing (#517) + a locality-aware fetch of a missing owned body are later slices.
See [docs/compute-near-data.md](docs/compute-near-data.md).

Another DDF **op** is the **window / as-of (point-in-time) aggregation**
(`include/zetta/ddf/as_of.hpp`, `zetta::ddf::as_of_aggregate`; **ARC-12**, #384) — the
**leakage-free featurization primitive**: for each entity, reduce the values of only
the rows whose timestamp falls inside a time window — at or before an `as_of` cutoff
(inclusive, `time <= as_of`) and, when `window_start` is set, at or after it (a
trailing window `[window_start, as_of]`; absent ⇒ the unbounded `(-inf, as_of]`
point-in-time case) — so a feature computed "as of *t0*" never sees a future event. It
operates over three numeric columns of one frame: an **integer entity-key** (the exact
group key), a numeric **timestamp**, and a numeric **value** reduced per entity via a
`compute::ReduceOp` (`sum`/`product`/`minimum`/`maximum`). The `AsOfResult` is **dense +
deterministic** — one row per distinct entity in ascending key order, each carrying the
per-entity reduction (`values`) + the qualifying-row `count`; an entity with only future
rows still appears (count 0, reduction identity). It is the **host-side reference** (read
the columns back from the frame's device, group + mask-reduce in host code — correctness
over throughput, like `CpuDevice` is the reference for `CudaDevice`; pushing the group-by
onto `sort_by_key` + segmented scan is a later optimization behind the same API), and
**non-throwing** (a missing/non-integer-entity/non-numeric column, or
`window_start > as_of`, returns a failing `ComputeStatus`). The `AsOfAggregate` spec
carries a **validated JSON boundary** (`to_json`/`from_json` mirroring
`schemas/as_of_aggregate.schema.json`, the same hand-written non-throwing
`DataFrameSchema`/`ScalingPolicy` pattern). See [docs/ddf-as-of.md](docs/ddf-as-of.md).

The DDF ops above **compose into point-in-time featurization**
(`zetta::ddf::point_in_time_featurize`, `include/zetta/ddf/featurize.hpp`; **ARC-12 /
FEAT-6**, #388) — the **leakage-free feature table for training**, expressing the
churn `churn_features.sql` shape as a DDF graph. The non-negotiable property is **no
leakage**: every feature for an entity at decision time `t0 = as_of` uses only events
with `time <= as_of` (the realized churn label + any post-`t0` signal live only in the
training target). A `FeaturizeSpec` is a small graph — the entity/time keys, the single
`as_of` cutoff, the windowed `event_features` (each a `count`/`sum`/`min`/`max`/`mean`
aggregate over `[window_start, as_of]`, optionally one event type), the static
`account_features` carried through, and the optional `label_column` (the target `y`,
never a feature). The single-node op composes the leaf ops: each windowed feature runs
through the [as-of op](#ddf-dataframe-zettaddf) (`sum`/`min`/`max`) or a
[filter](#ddf-dataframe-zettaddf) + [group-by](#ddf-dataframe-zettaddf)
(`count`/`mean`), and the per-entity results attach to the account dimension with the
[equi-join op](#ddf-dataframe-zettaddf) (`hash_join`). Output is a `FeatureTable` —
**dense + left-join**: one row per entity in the *accounts* frame (the universe to
score), ascending, an account with no qualifying events filled to 0 (the SQL
`COALESCE(agg, 0)`; a `min`/`max` over an empty window collapses to 0, not `±inf`), with
the columns also materializable into a device-backed feature `DataFrame`
(`FeatureTable::to_data_frame`). It runs **distributed as a recorded `featurize`
`ComputeJob`** (`zetta::ddf::run_featurize_job`, `include/zetta/ddf/featurize_job.hpp`)
through the [job coordinator](#job-coordinator-zettacompute): the literal `accounts ⋈
events … GROUP BY account` shape — `distributed_join` repartitions+enriches the events
shard with the accounts shard, then per-feature `filter` + `distributed_group_by` combine
per entity (broadcast back, so the global table is identical on every node), recorded in
`system.zetta_jobs`. Single-node and multi-node produce identical tables; the
`distributed_join` is inner-only (an account with no events isn't emitted by the
distributed path — the fixture has events for every account; a true left join lands when
the join gains outer support). The `FeaturizeSpec` carries a **validated JSON boundary**
(`schemas/featurize_spec.schema.json`, the hand-written non-throwing pattern). The
acceptance featurizes the [synthetic churn fixture](docs/churn-dataset.md) and asserts
every feature matches a leakage-free ground truth — the deliberate post-`t0` events
(`acct_fedex_office`'s 4th payment failure, `acct_globex`'s 2nd support ticket) never
enter a feature. Training a `Model` from the table is the **GBDT trainer**
(`zetta::ml`, below); explaining its predictions is the **exact TreeSHAP** slice
(`zetta::ml::tree_shap`, below).
The spec also has a **static (carry-through) mode** (**#532**): `event_features` may be
empty — a featurization of already-engineered numeric columns (the
saas-sales-conversations **engineered columns**: `customer_engagement` /
`sales_effectiveness` / …, embeddings dropped) where each row is one entity, the
`account_features` are carried through, and `label_column` is the target. The
point-in-time guarantee still holds **where applicable**: with a `time_column` set the
row universe is filtered to `time <= as_of` (a post-cutoff row is excluded — no
leakage); with none there is no time dimension. In static mode `time_column` is optional
and the events frame/join are skipped; with windowed `event_features` (the churn shape)
a `time_column` is required — and a spec must produce at least one feature column (an
event or account feature). The **compute-near-data** variant over a single partitioned
table is `zetta::ddf::run_near_data_featurize_job`
(`include/zetta/ddf/local_featurize_job.hpp`, **#532**) — load this node's owned
partitions → `run_featurize_job` over the local shard, reporting the same
`local_bytes_read`/`wire_bytes_moved` instrumentation, matching a single-node oracle
(the UC-1-at-scale featurize slice; siblings #530/#533 train, #531 E2E). See
[docs/ddf-featurize.md](docs/ddf-featurize.md).

### Model training — distributed histogram GBDT (`zetta::ml`)

The **GBDT trainer** (`zetta::ml`, `include/zetta/ml/gbdt.hpp`; **FEAT-6 / ENT-14**,
#390) trains a model on the customer's own data — the differentiator of the native
distributed accelerated-compute pass. It implements **histogram gradient-boosted
decision trees** with a **binary logistic (log-loss)** objective: a model is a base
score plus an ordered ensemble of small `RegressionTree`s, each boosting round
fitting one tree to the gradient/Hessian of the log-loss (`g = p - y`, `h = p(1-p)`)
at the current predictions. A tree is grown level by level by **binning** each
feature's values (equal-width over globally-agreed edges) and accumulating per-bin
sums of gradients + Hessians — the **gradient histogram** — then taking the bin
split that maximizes the regularized gain `½[G_L²/(H_L+λ) + G_R²/(H_R+λ) - G²/(H+λ)]
- γ`; a leaf's output is the Newton step `-G/(H+λ)` shrunk by the learning rate.
`GbdtParams` carries the knobs (`num_trees`/`max_depth`/`max_bins`/`learning_rate`/
λ/γ/`min_child_hessian`) behind a **validated JSON boundary** (the hand-written
`from_json` pattern of `FeaturizeSpec`); `GbdtMetrics`/`evaluate_binary` give
AUC/log-loss/accuracy.

**Distributed by histogram allreduce.** The training rows (one per entity — the
design matrix `X` + label `y`) are sharded across the cluster, and the trainer is
**SPMD**: every node runs the same boosting loop over its shard, combining per-node
statistics across the cluster each round via the round's
[collectives](#collectives-allreduce--broadcast--barrier--gather) — global bin edges
(`allreduce` Min/Max), the global base rate (`allreduce` Sum), and the per-level
gradient histograms (`allreduce` Sum). Because split-finding then runs off the
**global** histograms (identical on every node), every node grows the **same trees**
with no further coordination, so single-node and multi-node produce equivalent
models (bit-identical within a run; equal up to floating-point across different
shardings). The combine is injected as an `AllReduce` callable, so the single-node
path (identity reduction) and the distributed job share one boosting implementation.
Like the [as-of](#ddf-dataframe-zettaddf)/[group-by](#ddf-dataframe-zettaddf) ops it
is a **host-side reference over device-backed data** — it reads the feature columns
back through the `ComputeDevice` download seam (so it runs the same over a CPU or
CUDA frame) and accumulates the histograms host-side via `runtime::parallel_for`; a
device-native weighted-histogram kernel is a later optimization behind the same API.
Non-throwing throughout.

It runs as a recorded `train` **ComputeJob** through the
[job coordinator](#job-coordinator-zettacompute) and **produces a `Model`**:
`zetta::ml::run_train_job` (`include/zetta/ml/gbdt_train_job.hpp`) drives `job` (kind
`train`) through the lifecycle (so it appears in `system.zetta_jobs`), computes global
metrics (gather the per-node `(prob, label)` pairs to the root, evaluate over the
union, broadcast back), and on the deterministic root registers the trained model as
a `zetta::Model` (kind `classifier`) — the GBDT weights body is the `GbdtModel` JSON
(**boundary-validated** like the Run record via the `pboettch` draft-07 validator,
`schemas/gbdt_model.schema.json`, because the body is read back from disk / a peer),
attached via `set_weights` (fingerprinted + replicated peer-to-peer like any
large-object body), with the metrics in `metrics` and the per-feature gains as the
ranked `feature_schema.drivers` (read by `zetta::ranked_drivers`). It introduces **no
new wire format** — the histograms ride the schema-validated `CollectiveMessage`, the
weights body carries its own validated boundary, and the job + model records are
validated at their own boundaries. Trained on the
[synthetic churn fixture](docs/churn-dataset.md) (via featurization) it reaches a
strong AUC and surfaces `failed_payments` as a churn driver. Per-prediction SHAP
explanation is the **exact TreeSHAP** slice below; device-native histograms are a
later slice. See [docs/gbdt.md](docs/gbdt.md).

### Model explanation — exact TreeSHAP (`zetta::ml`)

The **TreeSHAP explainer** (`zetta::ml`, `include/zetta/ml/tree_shap.hpp`; **FEAT-6**,
#393) is the explainability the rented-model approach can't give — it attributes a
[GBDT](docs/gbdt.md) prediction to its features per entity, so a per-entity **dominant
churn driver** falls out. It implements **exact path-dependent TreeSHAP** (Lundberg,
Erion, Lee 2018, Algorithm 2): a single polynomial-time (`O(trees·leaves·depth²)`)
recursive traversal per tree that tracks, for every feature subset, the proportion of
permutations consistent with it (the `extend_path`/`unwind_path`/`unwound_path_sum`
bookkeeping). A "missing" feature is integrated out by following **both** children
weighted by the training **cover** that flowed each way (`cover[child]/cover[node]`) —
the per-node Σ-Hessian the trainer already computes, now **recorded on every
`TreeNode`** (`TreeNode::cover`, serialized in the `gbdt_model` weights body +
schema), so the model is **self-describing** and no background dataset is needed.
`tree_shap(model, x)` returns a `ShapExplanation` — the per-feature `φ`, the model's
cover-weighted `base_value`, and `predicted` — in the model's **raw-score (margin /
logit) space**, where SHAP is additive, so the headline **efficiency** invariant
`base_value + Σ φ = predict_raw(x)` holds exactly per entity (the acceptance test);
`dominant_driver` is the feature with the largest `|φ|` (signed). It is **pure +
deterministic + non-throwing** and adds **no new JSON boundary** (it reads the
already-validated `GbdtModel`).

It runs as a recorded **`explain` ComputeJob** through the
[job coordinator](docs/job-coordinator.md): `zetta::ml::run_explain_job`
(`include/zetta/ml/gbdt_explain_job.hpp`) drives `job` (kind `explain`) through the
lifecycle (so it appears in `system.zetta_jobs`), distributed for **batch
explanation** of a whole entity universe. Per-entity SHAP has no cross-entity
dependency (the model is replicated, identical on every node), so each node fans
`tree_shap` across its shard's rows (intra-node `runtime::parallel_for`, the trainer's
pattern) labelling each by its entity key, and **one allreduce** combines the per-node
`Σ|φ|` + row counts into a global, population-level **mean-|φ| driver ranking**
(identical on every node) — so single-node and multi-node produce identical drivers.
No splitter/repartition is needed (the model carries the cover background it integrates
over), and there is **no new wire format** (the `Σ|φ|` partials ride the
schema-validated `CollectiveMessage`; the job record is validated at its own
boundary). Trained + explained on the [churn fixture](docs/churn-dataset.md),
`failed_payments` is the dominant driver of the worst-failure account and the top
global driver. Persisting the per-entity scores as a dataset / Run output, writing the
SHAP ranking onto the `Model`'s `feature_schema.drivers`, an interventional
(background-dataset) variant, and a GPU TreeSHAP kernel are later slices. See
[docs/tree-shap.md](docs/tree-shap.md).

The GBDT's output probabilities are **calibrated** (`zetta::ml::Calibrator`,
`include/zetta/ml/calibration.hpp`; **FEAT-6**, #391) so the risk scores are
trustworthy — the reference is a *calibrated* XGBoost. The model orders accounts
well (a strong AUC) but `sigmoid(raw)` is not necessarily well-calibrated;
calibration fits a **monotone** `raw score → probability` map on a **held-out**
split, so reliability (Brier / ECE) improves while the ranking (AUC) is preserved.
Two methods, both pure/host-side/non-throwing: **Platt scaling** (a two-parameter
`sigmoid(a·raw+b)` fit by the robust Newton method of Lin, Lin & Weng 2007 — the
identity `a=1,b=0` is the no-op, so an uncalibrated model is plain `sigmoid(raw)`)
and **isotonic regression** (a non-decreasing step function fit by PAVA).
`fit_calibrator(raw_scores, labels, method)` fits it, `Calibrator::calibrate(raw)`
applies it, and `calibration_metrics` gives the **Brier score + ECE**. A fitted
`Calibrator` is **part of a `GbdtModel`** (optional `calibration` field; when present
`predict_proba` routes through it, and it rides inside the schema-validated weights
body — a calibrated model is self-contained); `calibrate_model(model, holdout, …)`
fits + attaches in one step. The distributed `train` job calibrates the produced
`Model` when a `CalibrationConfig{enabled, method, holdout_fraction}` is passed to
`run_train_job` (off by default → uncalibrated, unchanged): each node holds out a
slice of its shard by a **content hash of the row** (sharding-invariant, so the same
physical rows are held out under any sharding → single-node and multi-node grow the
same trees and fit the same calibrator), trains on the train rows, and the held-out
`(raw score, label)` pairs are **gathered to the root, fit there, and broadcast
back** (best-effort — a single-class held-out set leaves the model uncalibrated, the
job never fails). **No new wire format** (the pairs + calibrator ride the
schema-validated `CollectiveMessage`; the calibrator serializes into the GBDT weights
body). Its JSON boundary mirrors `schemas/calibration.schema.json` (the pboettch
validator, like the model body it rides inside). See
[docs/calibration.md](docs/calibration.md).

### Batch scoring — rank-at-risk over a Model (`zetta::ml`)

The **inference counterpart** to the GBDT trainer (`zetta::ml`,
`include/zetta/ml/gbdt_score.hpp` + `gbdt_score_job.hpp`; **FEAT-6 / CMD-10**, #392):
batch-score a set of entities through a trained [`GbdtModel`](docs/gbdt.md) to produce
a **calibrated churn risk** per entity, then rank them into an **at-risk list**
(highest risk first) — the "rank-at-risk" deliverable a retention team acts on.
**Calibrated for free:** the model is binary-logistic (trained on log-loss, a proper
scoring rule), so its `predict_proba` (sigmoid of the raw ensemble score) *is* a
calibrated risk in `[0, 1]` — no Platt/isotonic step. The ranking is those
probabilities sorted descending, ties broken by entity ascending (**deterministic**,
input-order-independent). `score_gbdt` is the pure single-node scorer — it aligns the
provided columns to the model's own `feature_names` **by name** (so input columns may
be in any order / carry extras), with `predict_risk` the unranked half and a `top_k`
cap; `load_gbdt_model` reads a registered [`Model`](docs/models.md)'s serialized
weights body back through the **GBDT model JSON Schema boundary**
(`GbdtModel::from_json`, the same `schemas/gbdt_model.schema.json` the trainer writes
— re-validated because the body is read from disk / a peer). It runs as a recorded,
audited **`score` ComputeJob** (`run_score_job`) through the
[job coordinator](docs/job-coordinator.md) — **SPMD** like training: each node scores
its own shard locally (the small model is identical on every node, so no model
broadcast), the per-node `(entity, risk)` pairs are **gathered** to the root, **ranked
on the device** via the `sort_by_key` operator (the op the device header documents as
"sort entities by risk score, carrying their ids" — GPU when the frame lives there,
CPU fallback — embodying the [GPU-native, CPU-fallback, distributed](#ai-compute--gpu-native-cpu-fallback-distributed-direction)
direction), truncated to `top_k`, then **broadcast** back, so every node holds the
**identical global at-risk list** (single-node == multi-node). The tree-predict
numerics are the host-side reference today (device-native predict kernel deferred
behind the same API, like the trainer's histograms). It introduces **no new JSON /
wire format** — the pairs ride the schema-validated `CollectiveMessage` (values as
doubles; entity keys exact ≤ 2⁵³) and the weights body its existing `GbdtModel`
boundary. The deliverable is the in-memory ranked list (persisting scores as an
entity / SQL + GUI is a later slice). See [docs/gbdt-score.md](docs/gbdt-score.md).

### Native correctness oracle (`zetta::ml::load_golden_oracle`)

The **native correctness oracle** (`tests/test_native_oracle.cpp` + the
`zetta::ml::GoldenOracle` format, `include/zetta/ml/golden_oracle.hpp`; **RSK-8**,
#395) proves the from-scratch native [featurize](docs/ddf-featurize.md) /
[GBDT-train](docs/gbdt.md) / [TreeSHAP](docs/tree-shap.md) reimplementations are
numerically correct **without running any reference implementation at test time** — no
Python, no XGBoost, no foreign runtime, in the engine, the build, or CI (ADR-17). Three
self-contained C++ layers, all over the [synthetic churn fixture](docs/churn-dataset.md):
(1) **golden numeric fixtures** — expected featurize / train / SHAP outputs committed as
static data (`tests/fixtures/golden/churn_oracle.json`, generated once offline by zetta's
own engine via an env-gated `ZETTA_WRITE_GOLDEN=1` write mode; the generator is **not** a
repo/build/CI dependency) and asserted within tolerance, *and* cross-checked in the same
run against an independent leakage-free ground truth (featurize) + the SHAP **efficiency
identity** `base_value + Σ φ == predicted` (so the golden is proven correct, not merely
pinned); (2) **analytic micro-tests** — tiny hand-built datasets whose GBDT split + leaf
Newton step and TreeSHAP attributions are computable by hand (a balanced single-split
gives leaves exactly ∓2 with `base_value = 0`; an asymmetric-cover depth-2 tree gives
hand-derived φ), asserted to closed form; (3) **cross-cluster equivalence** —
single-node == 2-node == 3-node identical within tolerance (featurize bit-exact, train
equivalent up to floating-point reduction order at the `1e-4` cross-shard tolerance,
explain bit-stable on the replicated model), each equal to the single-node golden. The
golden is a **schema-validated boundary** like the [churn dataset](docs/churn-dataset.md):
`load_golden_oracle` is non-throwing, validates against
`schemas/golden_oracle.schema.json` (the embedded `pboettch` draft-07 validator) then the
cross-field structural rules, so a malformed fixture fails the load with a path-qualified
reason. Depends on #390 (GBDT) + #393 (TreeSHAP); ships **no** reference Python. See
[docs/native-oracle.md](docs/native-oracle.md).

### Retention playbook — driver→action→owner + constrained narrator (`zetta::ml`)

The **retention playbook** (`zetta::ml`, `include/zetta/ml/playbook.hpp`; **FEAT-6**,
#397) is the optional *use-case skin* that turns the churn model's quantitative output
— a [calibrated risk](docs/gbdt-score.md) per account + the
[per-entity dominant churn driver](docs/tree-shap.md) — into a **CRM-ready playbook** a
retention team acts on, plus an **outcome log** for the feedback loop. **The model and
the math do the quantitative work; the narrator does only language + play-selection** —
that division is the whole point. Three layers, in strict order: (1) a **deterministic
driver→action→owner mapping** — a `PlaybookPolicy` (the editable driver dictionary)
maps each churn driver (a feature name) to a recommended `play`, an owning role
(`PlaybookOwner` CSM/AM/Billing/Support/Marketing), and a **save-rate prior**
`P(save | driver)` (the heuristic spirit of `compile_intent`/`EntityResolver`); (2) the
**expected-retained-value priority** — rank by `P(churn) × ARR × P(save | dominant
driver)`, computed **in code** and **frozen before the narrator runs**, so a narrator
can never change the ranking (descending, ties by entity ascending → deterministic;
`top_k` caps the list a team can work); (3) a **constrained narrator behind a seam**
(`PlaybookNarrator`) — the production binding is **Claude**, which writes the narrative
+ selects the play within a **strict JSON schema** but **never invents numbers** (the
probability/ARR/priority are computed above and **absent from the narrator's output
schema**, so the model physically cannot emit or change them), with a deterministic
`RulesEngineNarrator` **fallback** so the pipeline runs **end-to-end offline**, no API
key, fully testable. `ConstrainedNarrator` calls an **injected `NarratorCompletion`**
callable (zetta-core carries no HTTP/LLM SDK — the `CommandRunner`/`FleetSource`
injection pattern; the production binding wires it to Claude Opus 4.8 structured
outputs), validates the model's JSON at the boundary, and falls back on **any** failure
(offline, non-JSON, schema/semantic violation). It adds **two JSON boundaries**: the
editable `PlaybookPolicy` (hand-written non-throwing `from_json`, the
`FeaturizeSpec`/`ScalingPolicy` pattern, `schemas/playbook_policy.schema.json`) and the
strict narrator-output contract (`playbook_entry_schema()`, the **pboettch draft-07
validator like the Run record**, `schemas/playbook_entry.schema.json` — the LLM/API
response enters here). `generate_playbook` produces the ranked `Playbook` of
`PlaybookEntry` rows (the frozen numbers + the narrative + play + a `narrator`
provenance field), and `outcome_log_from_playbook` seeds the append-only **outcome log**
(one pending row per entry; a later job resolves `renewed`/`action_taken` when the churn
horizon closes — the dataset that turns the static save-rate priors into learned
save-rate-by-driver). A **pure library** like `score_gbdt`/`tree_shap` (non-throwing, in
zetta-core); proven on the [churn fixture](docs/churn-dataset.md) deterministically (the
offline fallback path) — the worst-failed-payments account runs the billing play under
the Billing owner. Wiring it as a recorded `playbook` `ComputeJob` (into
`system.zetta_jobs`), a `system.zetta_*` read view, persistence, a GUI panel, and a real
Anthropic-Messages-API `NarratorCompletion` binding are later slices. See
[docs/playbook.md](docs/playbook.md).

### Work-stealing scheduler (`zetta::compute`)

The **work-stealing scheduler** is the task pool the native distributed compute
pass runs on (**ARC-10**, #366): a job is split into many small tasks; idle/faster
workers **steal** pending tasks from busy ones, so the work self-balances with
**no fixed per-worker capability model** — a faster worker (or GPU node) naturally
processes more, a slower one fewer (the "fluid, heterogeneity-tolerant" split).
The **local** half is `zetta::compute::WorkStealingScheduler`
(`include/zetta/compute/work_stealing_scheduler.hpp`): a fixed pool of worker
threads, **each owning a deque** of pending tasks, with stealing between them — the
owner pushes/pops its deque's **back** (LIFO locality), a thief takes a victim's
**front** (FIFO, least contention), made legible with a per-deque mutex rather
than a lock-free structure. `submit(task)` fire-and-forgets a unit of work;
`parallel_map(inputs, fn)` runs `fn` over N inputs across the pool and returns the
results **in order**, blocking until all complete (so call it off-worker). It is a
deliberate **sibling to `zetta::runtime::thread_pool`** (the Capy seam), which
schedules off a single shared queue and so has no observable per-worker steal to
test or reason about; task bodies may still use `zetta::runtime` coroutines. A
zero-worker pool is valid (a pure task *sink*); `stats()` exposes per-worker
counts + a steal count, `current_worker_index()` tells a task which worker it is
on. The **cross-node** half is `zetta::compute::DistributedScheduler`
(`include/zetta/compute/distributed_scheduler.hpp`), built on the local one:
idle/faster *nodes* steal pending tasks from peers over the
[signal plane](#signal-plane-rpc--events--tasks). The node that calls
`map(method, inputs)` **owns** the job (the authoritative pending queue); its
local drainers and idle peers both pull from it — a peer sends a `steal` RPC, the
owner replies with an **offer** of leased tasks, the peer runs them on its own
pool and returns a `steal_result`. **Single-hop** (a node only hands out its own
jobs' tasks, never re-steals), and **idempotent** completion so a late result is
ignored. It is **elastic mid-job** (ARC-10 / #367) — the fleet can change while a
job runs and the job still finishes correctly: a node that **joins** mid-job
starts stealing on its next tick (the steal loop re-reads `Cluster::peers()` each
pass, so an autoscaled-in helper speeds an in-flight job up with no extra wiring),
and a node that **leaves/fails** mid-job has its in-flight tasks **re-queued** —
`sweep_lost_assignments()` recovers a held task when its peer **drops out of
cluster membership** (the fast, *detected* path — a crash closes its sockets,
silence is dropped after `peer_idle_timeout_seconds`; counted in
`Stats::lost_node_requeued`) **or** when its per-task **lease** expires
(`Config::steal_lease`, the backstop for a hung-but-connected helper) — so `map`
never strands work on a lost helper while any compute capacity remains. The
application supplies one **executor** (`string(method, payload)`) registered
identically on every node; the three steal messages are **schema-validated at the
boundary** (`schemas/steal_{request,offer,result}.schema.json`, same `pboettch`
validator as the [signal envelope](#signal-plane-rpc--events--tasks)); elastic
membership adds **no new wire message** (it reads the in-memory peer table). It is
the cluster's `SignalHandler` for `steal`/`steal_result` and forwards every other
signal to a fallback. Scheduling a `ComputeJob` / driving collectives is the
**job coordinator** (#372, below), which submits work *through* this. See
[docs/work-stealing-scheduler.md](docs/work-stealing-scheduler.md).

### Job coordinator (`zetta::compute`)

The **job coordinator** (`zetta::compute::JobCoordinator`,
`include/zetta/compute/job_coordinator.hpp`; **ARC-11**, #372) is the **single
entry the native distributed accelerated-compute algorithms (DDF / GBDT / SHAP)
submit work through** — it joins the three pieces of the compute pass: the
[work-stealing scheduler](#work-stealing-scheduler-zettacompute) (split a job into
tasks, run across cores + nodes), [collectives](#collectives-allreduce--broadcast--barrier--gather)
(combine per-node partials at a round boundary), and the
[`ComputeJob`](#compute-jobs) lifecycle record. An algorithm hands the coordinator
a **job body** (`std::function<Outcome(JobRound&)>`); `run(job, body)` sets the job
**Running**, runs the body, then sets the terminal status from the body's `Outcome`
(Succeeded with a `detail` summary, or Failed with the reason) — so a job is never
left stuck in `Running`, and the terminal transition emits the `job_finished`
audit through the job's `AuditSink`. The body sees only a **`JobRound`** work
surface: `parallel_map` (intra-node data parallelism on the local pool — the
per-node partial), `map` (cross-node dynamic task stealing for not-pre-sharded
work), the four collectives (`allreduce`/`broadcast`/`barrier`/`gather` at round
boundaries), and topology (`participants`/`node_id`/`is_root`). The model is
**SPMD** (every node runs the same body against its own shard; the collective
combines partials), the way distributed GBDT/SHAP run; `map` is the owner-centric
alternative for embarrassingly-parallel work. The coordinator **owns** a
`Collectives` + a `DistributedScheduler` and wires the **shared signal-handler
chain** (`steal` → scheduler, `collective.*` → collectives, else → an optional app
fallback) — member declaration order makes construction register it and
destruction unwind it cleanly, which is why it owns (not references) those pieces.
It introduces **no new JSON format** — `map` rides the schema-validated steal
messages, the collectives ride the schema-validated `CollectiveMessage`, and the
job record is schema-validated at its own boundary. The autoscaler that sizes the
fleet from a job's `ScalingPolicy` is the **job autoscaler** below (wiring the
loop's observation source + tick into the coordinator for a live, end-to-end
autoscaled job is the remaining, later slice). See
[docs/job-coordinator.md](docs/job-coordinator.md).

### Job autoscaler (`zetta::JobAutoscaler`)

The **autoscaler control loop** (`zetta::JobAutoscaler`,
`include/zetta/job_autoscaler.hpp`; **ADR-29 / ARC-11**, #371) is the piece that
finally **acts on** a job's reward objective — the `ScalingPolicy` slice computes
a `ScalingDecision`, this loop closes it. Each `tick()` it (1) reads the current
allocation from
`Provisioner::provisioned()`, (2) calls an injected **`ObservationSource`**
callable for the running [`ComputeJob`](#compute-jobs)'s live signals
(`progress`/`elapsed_seconds`/`cost`), (3) scores them with the job's
`ScalingPolicy::evaluate()`, (4) applies **hysteresis**, and (5) — if a move is
confirmed — **adds/removes nodes through the `Provisioner` seam** (`provision(count
= target)` to grow, `teardown(node_id)` on the surplus to shrink), inside the
policy's bounds. It is **tick-driven and owns no thread** — the caller drives it
each heartbeat, exactly like the deployment controller's `reconcile()` — which
makes it deterministic and unit-testable. Because it reads `provisioned()` **every
tick**, it reflects live, **elastic membership** for free (a node that left simply
stops appearing → the loop re-provisions toward the target). **Hysteresis** is the
anti-thrash guard: a direction must be recommended on `confirm_ticks` consecutive
ticks (default 2) before the loop acts (a hold / a flip resets the streak), so an
alternating signal never moves the fleet — **except** a cost-cap breach, which
sheds **immediately** (`shed_on_cost_breach`, default true; the cap is a hard
safety constraint). On **given hardware** (`LocalProvisioner` — can't add,
no-op teardown) it degrades to **best-effort max utilization** (records the
shortfall in the tick `detail`, never fails). It programs against the abstract
`Provisioner&`, so the `BurstApprovalGate` decorator drops in transparently to
cost-cap every scale-up. It introduces **no new
replicated state** (the policy is passed in by value — a `ComputeJob` references
it by id only); the one JSON boundary is the **`AutoscalerConfig`** DTO
(`node_spec` + `confirm_ticks` + `shed_on_cost_breach`), boundary-validated like
`NodeCapability`/`BurstCap` (`from_json` mirroring
`schemas/autoscaler_config.schema.json`). Wiring the observation source to a real
progress/cost meter + mounting the tick into the coordinator/`Node::tick()` for a
live job are later slices. See [docs/job-autoscaler.md](docs/job-autoscaler.md).

### Burst-job eviction tolerance — checkpoint/restart (`zetta::compute`)

zetta bursts compute onto **spot/preemptible** nodes (every cloud backend requests
spot), which can be **reclaimed mid-job**. The **checkpoint/restart** capability
(`include/zetta/compute/checkpoint.hpp`, **RULE-5 / RSK-9**, #468) is the
**job-level** complement to the [work-stealing scheduler](#work-stealing-scheduler-zettacompute)'s
**task-level** lost-peer re-queue: it durably snapshots a long-running job's progress
and **resumes from the last snapshot** after the driving node dies, instead of
restarting from round 0. A **standalone seam** (no `Node`/SQL/cluster wiring of its
own — the altitude of `Provisioner`/`DeviceRegistry`); the long-running job bodies +
the restart trigger compose it. Three pieces: (1) **`JobCheckpoint`** — the durable
snapshot `{ job_id, kind, sequence (= units completed = the resume point), total,
state (opaque application bytes, base64'd + sha256-fingerprinted), state_sha256,
created }`, **boundary-validated like Run/Model/ComputeJob** (schema-as-code
`job_checkpoint_schema()` → generated `schemas/job_checkpoint.schema.json`,
drift-checked) **plus** a cross-field SHA-256 **integrity check** (`parse_job_checkpoint`
rejects a torn/corrupt body, the large-object-body pattern); the `job_id` is the
ComputeJob id for a single-node job but the cross-node-agreed **`round_tag`** for a
distributed (SPMD) job. (2) **`CheckpointStore`** — the durability seam
(`save`/`latest`/`clear`/`job_ids`, non-throwing `CheckpointStatus`), with the
reference **`FileCheckpointStore`** writing per-job folders under a root dir
(atomic temp-write + rename so an eviction mid-write can't corrupt the last good
checkpoint; `latest` falls back past a corrupt head; only the 2 newest retained) —
local to one node's filesystem (a *shared/replicated* store for a *replacement*
burst node is a later slice; the store is **injected** so that swap is transparent).
(3) **`run_resumable_rounds`** — the driver: load the latest checkpoint and resume
from its `sequence` (a corrupt checkpoint fails hard, not silently from scratch),
run the remaining rounds checkpointing every N, and **drain gracefully** when an
optional `stop_requested()` fires (the spot-eviction-notice pattern — finish the
round in flight, checkpoint, exit). Re-running over the same store resumes with **no
recompute** of committed rounds. The first consumer is per-round
[GBDT](#model-training--distributed-histogram-gbdt-zettaml) model checkpointing; the
node owns a store under `<data_dir>/checkpoints` surfaced as `system.checkpoints`
(ephemeral, no CRDT backing); auto-restart from the autoscaler on a *detected*
eviction is a later slice. See [docs/job-checkpoint.md](docs/job-checkpoint.md).

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
[docs/cluster.md](docs/cluster.md). [docs/true-hybrid.md](docs/true-hybrid.md)
describes how the cluster, compute, and provisioner seams compose into **true
hybrid** — one cluster spanning every vendor and architecture.

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

### Collectives (allreduce / broadcast / barrier / gather)

**Collective communication** over the signal plane (`zetta::Collectives`,
`include/zetta/collectives.hpp`; ADR-28 / ARC-10, #365) — the primitive the
native distributed accelerated-compute pass for UC-1 churn uses to **combine
per-node compute partials** (gradient histograms, partial aggregates) across the
cluster. Four MPI-style collectives: **`allreduce`** (every node contributes a
vector → every node gets their elementwise reduction — sum/product/min/max),
**`broadcast`** (the root's vector → every node), **`barrier`** (all block until
all arrive, then proceed), and **`gather`** (every node contributes → the **root**
gets all of them ordered; classic MPI gather, result only at root). It rides
entirely on the signal plane's RPC (`Cluster::request`) — no new wire verb, no new
transport — and is **ephemeral** like every signal. **Deterministic, independent
of node order**: the participants are this node + connected peers, the *root* is
the **lexicographically-smallest node id** (leaderless — every node computes the
same root, no election), and the root **reduces once in node-id order and pushes
the single result to everyone**, so every node ends with bit-identical bytes even
though floating-point addition isn't associative. The round is a **star** (a
depth-1 tree: root gathers from all, broadcasts the result; a k-ary tree / ring is
a later optimization behind the same API), driven from the calling application
thread (which blocks until the round completes / times out) while the
`SignalHandler` (`on_request`, on the network thread) only records an
arrival/result and wakes the waiter — **never blocking**, honoring the signal
plane's contract. `Collectives` registers itself as the cluster's signal handler
(forwarding non-collective signals to an optional `delegate`). The two RPC methods
(`collective.contribute` / `collective.result`) carry a `zetta::CollectiveMessage`
payload, **boundary-validated** against a draft-07 JSON Schema
(`schemas/collective_message.schema.json`, mirrored embedded in
`src/core/collectives.cpp`, the same `pboettch` validator as the signal envelope);
`encode_collective_message`/`decode_collective_message` are the serialization seam.
Assumes a **fully-connected** membership (a partitioned cluster times out the
round). See [docs/collectives.md](docs/collectives.md).

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
(`src/core/catalog.hpp`): `node:<id>` (membership), `db:<uuid>`, `tbl:<uuid>`
(each carrying an optional `replication` field — the per-db/table
`ReplicationStrategy`, ENT-35, omitted when the default `ReplicateToAll`),
`row:<uuid>`, `lo:<uuid>`, `project:<uuid>`, `input:<uuid>` (a project input —
metadata only; its payload body stays on disk), `graph:<uuid>`,
`deployment:<uuid>`, `run:<uuid>`, `model:<uuid>` (a trained model — metadata
only; its serialized weights body stays on disk like an `lo:` body),
`job:<uuid>` (a compute job — ARC-11/ENT-31),
`index:<uuid>` (a registered index over a table — metadata only; ENT-39/ENT-40),
`principal:<uuid>` (an authenticated actor), `key:<uuid>` (an access key carrying
its parent `principal` ref + its `scope` + its `capabilities`),
`audit:<uuid>` (an immutable, append-only audit event), and
`part:<uuid>` (a dataset partition — the partition catalog: `{table, blob_ref,
row_range, stats, owner_nodes}`, replicate-to-all so placement is globally
visible — ENT-36/TBL-13) — each entity owns its key
by UUID so concurrent edits never collide.
`Node` is the integration point: it **captures** local DDL/row mutations into the
document (via `Node`/`Database`/`Table`, which hold the `ReplicationManager*`),
**reconciles** the on-disk projection from the document on each `tick()` (so peer
changes land on disk), and **persists** it to `data_dir/replication.automerge`.
Node-wide policies plug in via `ReplicationPolicy`; the first is
`ReplicateToAllNodes` (full copy on every node). Each **database/table** also
carries a per-entity **`zetta::ReplicationStrategy`** (ENT-35 —
`ReplicateToAll` | `Partitioned` | `PartitionedWithReplicas(replication_factor)`,
default `ReplicateToAll`, a validated JSON boundary), recorded + persisted +
replicated in its `db:`/`tbl:` value (default omitted, so today's representation
is unchanged); a table inherits its database's strategy unless it overrides. This
slice records the choice — the partitioned-placement engine that *acts* on a
non-default strategy is a later slice. See
[docs/replication-strategy.md](docs/replication-strategy.md). Done + tested: CRDT
core, cluster gossip + per-node status, **and full DDL+row data replication to
disk** (`tests/test_data_replication.cpp`). See
[docs/replication.md](docs/replication.md).

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
  <index-uuid>/              ← one folder per index (UUIDv4 = its id)
    index.json               ← { id, table, kind, backend, fields, created }
                                 (metadata only — no body)
  <job-uuid>/                ← one folder per compute job (UUIDv4 = its id)
    job.json                 ← { id, kind, status, project, graph, run,
                                  scaling_policy, created, updated, detail }
  <part-uuid>/               ← one folder per dataset partition (UUIDv4 = its id)
    partition.json           ← { id, table, blob_ref, row_range:{start,end},
                                  stats, owner_nodes, created }
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
    database.json            ← { id, name, replication? }  (replication = the
                                ReplicationStrategy, ENT-35; omitted when default)
    <table-uuid>/            ← one folder per table (UUIDv4 = its id)
      table.json             ← { id, name, replication? }  (replication strategy,
                                omitted when the default ReplicateToAll)
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
  metadata + on-disk paths (no Node access → safe on the network thread). Bodies
  default to **replicate-to-all**, except a body backing a
  [dataset partition](docs/dataset-partitions.md) on a `Partitioned`/`PartitionedWithReplicas`
  table, which is held **only on the partition's owner node(s)** — **sharded blob
  placement** (#515; [docs/sharded-blob-placement.md](docs/sharded-blob-placement.md)).
  Detail in [docs/large-objects.md](docs/large-objects.md).

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

### Parquet reader (`zetta::parquet`)

The native, read-only **Parquet reader** (ADR-32, #481 — hand-rolled, **no
Apache Arrow / parquet-cpp**; refines ADR-17's no-foreign-runtime rule) lives in
its own `parquet/` module (`include/zetta/parquet/`, `src/core/parquet/`,
namespace `zetta::parquet`, in `zetta-core`). Its first slice is the
**footer / metadata parser** (`zetta::parquet::parse_parquet_metadata`,
`include/zetta/parquet/metadata.hpp`, #505): given the **whole file's bytes**, it
checks the `PAR1` magic, reads the trailing little-endian footer length, slices
out the Thrift `FileMetaData` blob, and decodes it — the file's **schema**
(column names + physical types via `FileMetaData::leaf_columns()`), its **row
groups**, and per-**column-chunk** layout (offsets / sizes / encodings / codecs /
dictionary-page presence). It decodes **no data** itself — decompressing the page
bodies (the **Snappy** decoder #507) and projecting the decoded values into DDF
columns (the [reader → DDF slice](docs/parquet-reader.md) #508, which removes the
`ChurnDataFormat::Parquet` "not yet supported" error) are the sibling slices;
reading the page bodies into typed values is the **column-chunk decoders** (#506,
below). The
parser carries a small, self-contained **Thrift compact-protocol** decoder
(varint field-id deltas, zigzag ints, length-prefixed binary, compact
list/map framing) that dispatches on the parquet.thrift field ids it models and
**skips** the rest (statistics, logical types, column orders, encryption …), so
files carrying fields this slice ignores still parse. The footer bytes are a
**trust boundary**: zetta-core is `-fexperimental-library` (no exceptions across
the seam), so it is **non-throwing** and every read is bounds-checked — a
too-small / wrong-magic / encrypted-footer (`PARE`) / truncated / malformed file
returns a failing `ParquetMetadataResult` with a reason instead of reading out of
bounds (the binary-format analog of schema-at-the-boundary). The JSON projection
`to_json` emits **does** carry a schema — `parquet_metadata_schema()` (the
schema-as-code source of truth, generated to
`schemas/parquet_metadata.schema.json`, drift-checked by `test_schemas`) +
`validate_parquet_metadata_json` (the pboettch boundary the committed test golden
re-enters through). The acceptance (`tests/test_parquet_metadata.cpp`) parses a
committed fixture (`tests/fixtures/parquet/churn_sample.parquet`) and asserts its
`to_json` matches a **pyarrow-dumped golden** field for field — an *independent*
reference, regenerated by the dev-time-only `scripts/gen-parquet-fixture.py`
(needs pyarrow; **not** a build/CI dependency). See
[docs/parquet-metadata.md](docs/parquet-metadata.md).

The **column-chunk decoders** (`zetta::parquet::decode_column` /
`decode_column_chunk`, `include/zetta/parquet/column_decoder.hpp`, #506) read a
column's **data pages** into typed values, on top of the footer parser (#505) +
the Snappy codec (#507). Per column chunk they walk its pages (an optional
**dictionary page** + one or more **data pages**), decompress each body, and
decode the values for the encodings the churn datasets use: **PLAIN** (fixed-width
little-endian for int32/int64/float/double, bit-packed `boolean`,
length-prefixed `byte_array`) and **dictionary** (a PLAIN dictionary page + data
pages of **RLE/bit-packed** integer indices into it). **Definition levels** — the
RLE/bit-packed-hybrid null markers an OPTIONAL column carries — decode into a
per-row **validity bitmap** (a REQUIRED column carries none → all present); the
max def/rep levels come from the schema, computed in the footer parser's leaf walk
and exposed on `LeafColumn` (`max_definition_level`/`max_repetition_level`). A
`DecodedColumn` stores values the **Arrow-native** way — dense present-only typed
vectors + the validity bitmap — the split the #508 DDF projection consumes. Scope
is **flat columns, page v1** (pyarrow's default), codecs **uncompressed + snappy**
(zstd/gzip → a clear "unsupported codec" error until their decoder lands), PLAIN +
dictionary encodings; a v2 page / nested column / unsupported encoding / codec /
out-of-range index fails the decode with a reason rather than producing wrong
values (non-throwing, the binary analog of schema-at-the-boundary). The shared
**Thrift compact reader** moved to `src/core/parquet/thrift_compact.hpp` (used by
both the footer and page-header parsers). The JSON projection `to_json` emits
(`{path,type,values:[…]}`, nulls as JSON null) carries a schema —
`parquet_columns_schema()` (schema-as-code, generated to
`schemas/parquet_columns.schema.json`, drift-checked by `test_schemas`) +
`validate_parquet_columns_json`. The acceptance
(`tests/test_parquet_column_decoder.cpp`) decodes a dedicated, decodable-codec
fixture (`tests/fixtures/parquet/churn_decode.parquet`, separate from the
zstd/gzip-mixing metadata fixture) and matches a pyarrow-authored values golden
field for field — every dtype, PLAIN + dictionary, required + optional, across 2
row groups. **No DDF/SQL/run wiring** (that is #508). See
[docs/parquet-column-decoders.md](docs/parquet-column-decoders.md).

The **reader → DDF columns** slice (`zetta::ddf::load_parquet_data_frame`,
`include/zetta/ddf/parquet_reader.hpp`, #508) is the **capstone**: it assembles
the footer parser (#505) + column decoders (#506) + Snappy codec (#507) into a
reader that materializes a **requested subset** of a file's columns straight into
a typed, chunked [DDF `DataFrame`](docs/ddf.md) (DeviceBuffer-backed columns).
**Column projection** is the point — the `DataFrameSchema` names *which* leaf
columns to read (and their target dtype), and **only those column chunks are
decoded** — so it loads the engineered numeric columns + `outcome` of the
saas-sales-conversations dataset while **dropping `embedding_0..3071`** (3072
columns) without paying to decode them (each `ColumnSchema`'s `source_field()` is
the parquet leaf path, `name` the frame column; the chunk layout matches
`load_data_frame`). A DataFrame is **numeric + dense**, so each column converts to
its target dtype validated at the boundary (integer widens into float; a
real-into-integer / negative-into-unsigned / out-of-range / byte_array-into-numeric
/ **null** value fails with a reason), and `infer_parquet_schema` is the
metadata-only "load every number" convenience. Its untyped sibling
**`zetta::parquet::read_parquet_rows`** (`include/zetta/parquet/reader.hpp`)
decodes **every** leaf and transposes the values into one `nlohmann::json` object
per row — the inverse of the JsonlStore row shape — so a `.parquet` file drops
into any JSON-row consumer. That is how the **churn dataset loader**
(`load_churn_dataset(..., ChurnDataFormat::Parquet)`) now reads Parquet
(`accounts.parquet`/`events.parquet` → `read_parquet_rows` → the same validators +
`from_json` as JSONL/CSV; the `parquet format not yet supported` error is gone),
all three formats round-tripping to the identical `ChurnDataset`. The TC-9 golden
(`{num_rows,columns:[{name,dtype,values}]}`) carries a schema —
`parquet_data_frame_schema()` (schema-as-code, generated to
`schemas/parquet_data_frame.schema.json`, drift-checked by `test_schemas`) +
`validate_parquet_data_frame_json` — and the input projection rides the existing
validated `DataFrameSchema` boundary (no new runtime JSON format). The acceptance
(`tests/test_parquet_reader.cpp`, **TC-9 / #499 parity**) loads the projected
subset of `tests/fixtures/parquet/saas_sample.parquet` (5 engineered/outcome
columns + 8 `embedding_*`, scaled down from 3072) and matches a pyarrow-authored
DataFrame golden **on all four axes TC-9 names — schema (column names + order),
row count, dtypes, and values** — where dtype parity is both *produced* (the
reader's `dtype()` per column equals the golden's) **and** *inferred*
(`InfersDtypesMatchingGolden` asserts `infer_parquet_schema`, footer-only, agrees
with pyarrow per column, so the check isn't circular on the golden-imposed load),
asserting the embeddings are dropped (yet *seen* by inference as `float32`); the
churn parity is `ParquetFormatLoadsIdentically`. The **same parity is a
GUI-runnable [in-app test-suite](docs/test-suite.md) case**
(`ddf.parquet_reader_parity`, #462): the testkit runs headless / in the shipped GUI
with no on-disk fixture, so the `saas_sample.parquet` bytes (base64) + golden JSON
are **embedded** in `src/core/testkit/parquet_fixture_data.hpp` (generated by
`gen-parquet-fixture.py` from the same fixture — one source of truth). No
deploy/`ddf`-kind wiring yet; zstd is the follow-up codec. See
[docs/parquet-reader.md](docs/parquet-reader.md).

### Query layer

Queries are **dialect-agnostic**: text → a `Dialect` parser → a generic,
dialect-neutral AST (`zetta::query::Statement`, a `std::variant`) → the
`Executor` (which runs the AST against a `Node`). `PostgresDialect` is the first
frontend; `QueryService` is the parse+execute facade the UI calls. Supporting
another database format = writing another `Dialect` that targets the same AST —
**never** add dialect specifics to the executor. Supported today: basic DDL
(CREATE/DROP DATABASE, CREATE/DROP TABLE) and query (INSERT, SELECT with a single
`WHERE` predicate over the comparison set `=` / `!=` / `<>` / `<` / `<=` / `>` /
`>=`, projection, and **`GROUP BY` aggregation that runs through the
[DDF engine](docs/ddf.md)** — ARC-12 / API-2, #386: a grouped `SELECT` loads the
table into a `DataFrame`, applies `WHERE` via `ddf::filter`, and computes
`COUNT`/`SUM`/`MIN`/`MAX`/`AVG` per group with `ddf::group_by`, materializing the
result back into rows; numeric-only, one numeric key column, results in ascending
key order). The executor also serves a virtual, read-only **`system`**
database (à la `pg_catalog`) whose tables are computed at query time — first
**`system.zetta_nodes`** (cluster + replication status, when a `Cluster*` is
wired into the `Executor`/`QueryService`; bare `zetta_nodes` still resolves) and
**`system.zetta_deployments`** (recorded graph→target placements, sourced from
`Node::deployments()`; qualified name only) and **`system.zetta_models`** (the
model registry — trained models + their kind / metrics / weights fingerprint,
sourced from `Node::models()`; qualified name only) and **`system.zetta_indexes`**
(the index registry — one row per registered index over a table + its kind /
backend / fields, sourced from `Node::indexes()`; qualified name only) and
**`system.zetta_jobs`**
(the compute-job registry — one row per unit of accelerated compute + its
lifecycle, sourced from `Node::jobs()`; qualified name only) and
**`system.zetta_cloud_providers`** (the Cloud Inspector's provider catalog — pure
metadata, reads nothing from any cloud) and **`system.zetta_cloud_status`** (the
cached results of consent-gated cloud inspects, from `Node::cloud_inspections()`;
querying it never triggers a cloud call) and the run + governance read views
(RULE-7, the queryable counterpart of the GUI Runs / Admin panels):
**`system.zetta_runs`** (the run registry, `Node::runs()`),
**`system.zetta_principals`** (authenticated actors, `Node::principals()`),
**`system.zetta_keys`** (the access keys principals own — scope + capabilities as
readable strings, no secret material) and **`system.zetta_audit`** (the immutable
audit trail, `Node::audit_events()`) — plus **`system.dataset_partitions`** (the
partition catalog, `Node::dataset_partitions()`: where each table's data slices
live — table / blob / row range / stats / owner nodes; ENT-36/TBL-13) and
**`system.checkpoints`** (burst-job eviction-tolerance checkpoints,
`Node::checkpoints()`: the latest resume checkpoint per job in this node's local
store — kind / sequence / total / progress; ephemeral + local + not replicated, like
`system.zetta_cloud_status`; RULE-5/#468). `system` is
reserved (can't be created/dropped/written; no real db may be named it) and is
where future system tables (e.g. `system.leadership`) live — **surfacing observable
state as a `system.<table>` table is the default**, see the convention below
(name them plainly — no `zetta_` prefix; the legacy `zetta_*` tables await the
rename in [`#538`](https://github.com/oramasearch/zetta/issues/538)). Full detail in
[docs/query.md](docs/query.md).

### REPL console (`zetta::repl::ConsoleModel` + `zetta::gui::ReplConsole`)

The **REPL console** (**CMP-6**, #524) is the interactive SQL / simulated-Python
console behind the headed **Data → Console** view. Like the
[test suite](docs/test-suite.md) it is split **model-in-zetta-core /
GUI-as-thin-renderer**, so the same REPL session backs the GUI component today and
a **future CLI** unchanged. **`zetta::repl::ConsoleModel`**
(`include/zetta/repl/console_model.hpp`) is the engine-agnostic *session* — the
selected `Frontend` (`Sql` / `SimPython`), the input buffer, the scrollback
**transcript**, and `submit(run)` — with **no SDL/ImGui** (it lives in zetta-core,
so it is headless-testable, `tests/test_repl_console.cpp`). It **owns no engine**:
running a statement is a host-supplied **`RunHandler`** callback — the "run event"
the console emits — so the component is decoupled from any single frontend.
`submit` invokes the handler with the selected frontend + input, appends the
`(frontend, source, Result)` to the transcript, and resets the prompt; a
blank/whitespace input or a null handler is a no-op (mirrors `capture_intent`
rejecting empty intent), and both success and error results are recorded so errors
show inline. The frontend-agnostic **`Result`** (messages + columns + grid rows +
error + elapsed) holds SQL rows and sim-Python output alike;
`format_result`/`format_transcript` are the reusable text projections a CLI
prints. **`zetta::gui::ReplConsole`** (`src/gui/repl_console.hpp`) is the thin Dear
ImGui renderer (frontend selector, multiline editor, run/clear, transcript, result
grid); `App::draw_console` (`src/gui/app_explorer.cpp`) hosts it over the live node
and is the **governed Console/REPL screen** (SCR-8, #525, realizing UC-3): it routes
every statement through the **`zetta::repl::GovernedConsole`**
(`include/zetta/repl/governed_console.hpp`, also zetta-core / no-SDL) run handler —
**capability-scoped** against the acting key (ENT-22; `api:query` for SQL,
`orchestration:start_run` for sim-Python; an empty key is an *unauthenticated local*
session that still audits), **simulated-Python lowered to the IR** (`PythonFrontend`)
+ run through the [correctness gate](docs/gate.md) (RULE-1, a failing gate blocks)
then executed via `Node::run_graph` (a real Run), and **every statement recorded in
the [audit trail](docs/audit-trail.md)** as one `data_access` event
(allowed/blocked + a summary; reuses the existing audit action, so **no audit-schema
change**). The screen adds a **"Run as"** selector over the node's keys (default
*unauthenticated local*). It adds **no new JSON boundary** (the transcript is
ephemeral; SQL enters through `QueryService`, sim-Python through `ir::from_json` +
the gate, audit through the AuditEvent schema). See
[docs/repl-console.md](docs/repl-console.md) (the component) and
[docs/governed-console.md](docs/governed-console.md) (the governed screen).

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

### Language frontends (input language → ExecutionGraph IR)

A **`zetta::input::LanguageFrontend`** (`include/zetta/input/language_frontend.hpp`,
**ENT-38 / ADR-33**) turns a source snippet in some input language into the
`ir::ExecutionGraph` — the **IR-targeting analogue of the query layer's
`zetta::query::Dialect`** (which turns SQL text into the generic query AST). Add a
new input language by implementing the interface (`name()` + `compile(source) →
FrontendResult{graph, error}`); everything downstream (the gate, the
`GraphExecutor`, the GUI) is reused as-is. **Non-throwing** like the rest of
zetta-core.

The first implementation is the **simulated-Python frontend**
(`include/zetta/input/python_frontend.hpp`, `zetta::input::PythonFrontend`, **#520**):
it parses a small Python subset — **assignments, function calls, attribute/method
access + chaining, literals** (strings incl. triple/`f`/`r`-prefix, numbers,
`True`/`False`/`None`, lists, dicts), plus `import`/`from-import` (recorded as
nothing) — and lowers it to the IR. It is **not** a Python interpreter (runs no
Python, no libraries — the [new-input](.claude/skills/new-input/SKILL.md)
principle: re-implement, don't run), the way `compile_n8n_workflow` maps an n8n
workflow. Two lowering rules: (1) **every call becomes a typed step** — its
`StepKind` from the called verb via `python_step_kind` (the documented verb table,
like `n8n_step_kind`; an unmapped verb defaults to `Clean`, a generic transform, so
an arbitrary script still yields a well-formed graph), node id a deterministic
unique slug from the verb; (2) **data flow becomes `DataFlow` edges** — an
assignment binds a variable to the node a call produces, and a later call
referencing it (as an argument, inside a list/dict argument, or as a **method
receiver**) gets an edge from the producer (so `a = f(); b = g(a)` is `f → g`, and
`read_csv(…).dropna().fillna(0)` is `read_csv → dropna → fillna`). Parsed call
arguments ride under a namespaced **`python`** attribute bag (`call`/`assigned_to`/
`args`/`kwargs`, like n8n's `n8n` provenance). Unsupported constructs
(`def`/`for`/`if`/binary operators) are **explicitly rejected** with a reason, not
mis-parsed. It is built like `PostgresDialect` — a tokenizer + recursive-descent
parser — but **non-throwing** (a parse failure is an error string, never an
exception). It adds **no new JSON format**: the input is Python source text and the
only JSON it emits is the already-schema-validated IR JSON form (the `ir::from_json`
trust boundary), so the output round-trips through `ir::from_json` and passes
`default_gate()` by construction, executing through `Node::run_graph` like any
other frontend's. Wiring it into a top-level input dispatch (GUI auto-detect
Python vs SQL) is a later slice. See [docs/python-frontend.md](docs/python-frontend.md).

The **cluster + ML verbs are bound to the engine** (#522): the verb table is
**engine-faithful**, lowering each verb to the stage whose real operation it names,
so the lowered step runs the right work end-to-end. Training and inference are
**different stages** — `train`/`fit`/`calibrate` → `model` (the native
[GBDT trainer](docs/gbdt.md) the [`ChurnStepExecutor`](#graph-execution-run-a-graph--a-run)
runs in the model step), while `score`/`predict`/`rank` → `index` (the distributed
[batch-scoring / rank-at-risk](docs/gbdt-score.md) job it runs in the index step) —
`explain` → `serve` (exact [TreeSHAP](docs/tree-shap.md), the churn-driver job it
runs in the serve step), and `add_nodes` → `act` (provisioning is a cluster side
effect). The three ML verbs need **no new execution code** — a lowered sim-Python
churn script runs through [`run_churn_solution`](docs/churn-solution.md) like the
catalog blueprint, producing a real `Model` + ranked at-risk list + drivers.
`cluster.add_nodes(n, provider=)` is the one **control** verb (not a data step): the
frontend lowers it to an `act` node (recording `n` + `provider=` in the `python`
provenance bag), and the matching execution binding
**`zetta::input::run_add_nodes_step`** (`include/zetta/input/cluster_bindings.hpp`)
reads the count and drives the permanent-add control action
[`zetta::add_nodes`](docs/add-nodes.md). **Cloud stays consent-gated** — the
`provider=` value is provenance only; the `Provisioner&` is **injected** by the host
(which owns the per-action cloud consent gate), never constructed from the string.
**No new JSON format** (the provenance bag rides the validated IR boundary; the
`AddNodesRequest` has its own). See [docs/python-frontend.md](docs/python-frontend.md).

The **`datasets` library shim** (#521) is the data-load counterpart: a sim-Python
`from datasets import load_dataset; load_dataset("<path>", columns=[…])` already
lowers to a `connect` node carrying the parquet path + a `columns=` projection in the
`python` bag, and the matching execution binding
**`zetta::input::run_load_dataset_step`** (`include/zetta/input/datasets_bindings.hpp`)
reproduces the *meaning* of `load_dataset` with zetta's own primitives — decode the
referenced Parquet bytes with the native [Parquet reader](docs/parquet-reader.md)
(`read_parquet_rows`, no Apache Arrow / parquet-cpp), apply the column projection, and
populate a (optionally [partitioned](docs/partitioned-write-path.md)) table named for
the dataset (via `write_partitioned` when a `Partitioner` is configured). The
`DatasetStepExecutor` (a `StepExecutor` like `ChurnStepExecutor`/`IndexStepExecutor`)
runs the load step inside a `GraphExecutor` (delegating non-load steps to
`DefaultStepExecutor`). **HuggingFace `datasets` is the fidelity reference only —
never run**: file/network IO lives behind the injectable **`DatasetResolver`** seam
(offline default = a local filesystem path; a Hub-name resolver is a host-injected
later slice), so the engine/build/CI never touch the network. **No new JSON format**
(the spec rides the validated IR boundary; the Parquet bytes are bounds-checked by the
native reader; rows are stored as ordinary free-form table rows). See
[docs/datasets-shim.md](docs/datasets-shim.md).

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

### Entity resolution in featurization (`zetta::EntityResolver`)

The **producer** the entity-resolution gate above was built to guard
(`include/zetta/entity_resolution.hpp`, **FEAT-6 / TC-4**, `#394`) — the
make-or-break **data step before churn featurization**: it maps the raw
account-name strings on comms events (the `mention`s) to a canonical CRM account so
a customer's events aggregate against the right account, division-correct — "FedEx
Freight" resolves to its own account **without conflating** the 5–6 FedEx siblings.
Resolver **produces** the `entity_resolution` annotation; the
[gate](#entity-resolution-correctness-eval-zettaentityresolutioncheck)
**verifies** it before deploy (RULE-1), so a buggy resolver cannot ship. A
`ResolutionCandidate` is one candidate account (`account_id` target + `name`/`brand`
match keys + ground-truth `entity` that *labels* output, never *decides*);
`EntityResolver(candidates, strategy).resolve(mention)` returns the matched
`account_id`. It is **transparent + deterministic** — pure normalized string
matching, no ML/LLM (the same heuristic spirit as `compile_intent`); real
fuzzy/embedding resolution plugs in behind the same `resolve` front door later. Two
strategies: **`FullName`** (default) matches the normalized full account name
(`normalize_entity_name`: ASCII-lowercase + fold punctuation/whitespace), so the
variant "Fedex Freight" still lands on `acct_fedex_freight` while "FedEx Ground"
stays separate and the near-collision "Umbrella Logistics" is **not** fooled into
"FedEx Logistics" — a mention that doesn't exactly match a division name is left
**unresolved** (the conservative choice); **`Brand`** is the deliberately coarse
trap that collapses every "FedEx …" mention onto one `acct_brand_fedex` bucket,
producing the conflation the gate must fail (a *real resolver behavior*, not a
hand-mutated payload). `entity_resolution_annotation(candidates, mentions, strategy)`
emits the schema-shaped payload (each `LabeledMention` pairs a raw mention with the
held-out ground-truth entity; an unresolved mention gets a deterministic per-mention
`unresolved:<…>` account, never a silent drop). The churn fixture's
`entity_resolution_attribute(dataset, strategy = FullName)`
([churn_dataset.hpp](include/zetta/churn_dataset.hpp)) now routes through this
resolver — so "division-correct" is **earned** (matched from the mention string),
not asserted by construction as before. It introduces **no new JSON format**: the
output rides the existing `schemas/entity_resolution.schema.json`, validated where
it enters the gate (the resolver's inputs are typed structs, not parsed JSON). This
is the resolution/clean step, **not** the numeric `point_in_time_featurize` op
(which operates on already-resolved integer keys). See
[docs/entity-resolution.md](docs/entity-resolution.md).

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
Csv (a flat sibling), Parquet (read by the native no-Arrow
[Parquet reader](docs/parquet-reader.md) via `read_parquet_rows`, #508) }` — all
three round-trip to the identical dataset (no foreign runtime: the `.parquet`
fixtures are generated dev-time by `scripts/gen-parquet-fixture.py`, not at
build/CI). `entity_resolution_attribute(dataset)`
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
on an empty graph, a non-local target, **or a graph that fails the correctness
gate** — RULE-1 is enforced at the deploy seam itself: `deploy_graph` runs
`default_gate()` and places nothing on a failing report, recording a blocked
`graph_deployed` audit event, so a *programmatic* deploy can't bypass the gate the
GUI Deploy panel runs), `find_deployment(id)`, `deployments()`,
`deployment_count()`. A `Deployment`
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

The catalog **churn** blueprint is **wired to the real engine** (`zetta::run_churn_solution`,
`include/zetta/churn_solution.hpp`; **FEAT-4 / UC-1**, #398) — so the same graph
that deploys also *runs* the real featurize→train→explain→rank→playbook pipeline
rather than the stubbed `DefaultStepExecutor`. The wiring has two parts: (1) the
catalog churn graph gained the **`index` step** so its shape
(`connect×2→clean→model→index→serve→act`) matches the real
[`ChurnStepExecutor`](#graph-execution-run-a-graph--a-run) pipeline (the
`index→score/rank` stage the `act` retention-playbook step needs); and (2) the
reusable bridge `run_churn_solution(node, coordinator, device, graph, dataset,
options)` **encodes** a [`ChurnDataset`](#synthetic-churn-dataset-uc-1-fixture--loader)
into numeric DDF shards (string ids / ISO dates / categorical event types → numeric
columns), builds the standard featurize spec + GBDT params, **injects** a
`ChurnStepExecutor`, and **executes** the graph — returning the Run plus the real
`Model` id + ranked at-risk list + drivers + playbook. It adds **no new JSON
format** (rides the validated `ChurnDataset` / catalog / Run / ComputeJob / GBDT
boundaries). The Deploy panel offers a **"Run on real engine"** action
(`App::run_churn_demo`) for the churn blueprint — synchronous on the UI thread (like
the Cloud Inspector), over `demo_churn_dataset()` (a built-in, no-files demo
dataset), on an **isolated single-node loopback `JobCoordinator`** so the SPMD
collectives never block on this node's real peers; the produced Model + jobs
register on the node (visible in the Runs/Jobs panels). Single-node here; the
single-node == multi-node equivalence is proven in the per-op job suites. See
[docs/churn-solution.md](docs/churn-solution.md).

### Cloud Inspector (permission-gated cloud provider inspection)

The headed **Cloud Inspector** (`App::draw_cloud`, **Operations → Cloud
Inspector**) is the GUI surface for the deployment-target backends — local,
on-prem bare-metal, Kubernetes, and the managed clouds (AWS / GCP / Azure / OCI) —
where a user **inspects a cloud** before deploying onto it (the GUI counterpart to
the `Provisioner` + CLI-control-plane layer). It is built around the **hard rule**
in *Conventions → Cloud providers — permission-gated*: touching a provider reads
credential/config files and can call real cloud APIs, so it is **deny-by-default,
ask-every-time, never-remembered** consent — a **load-bearing consent gate** that
names exactly what an inspect *would* read/call **before** anything happens.

The catalog is `zetta::CloudProviderCatalog` (`include/zetta/cloud_provider.hpp`),
a **pure-metadata** library (mirrors `SolutionCatalog`'s built-in singleton +
`find`): each `CloudProvider` carries identity/presentation plus a *description* of
what an inspect would touch — its `cli`, the `config_paths` the CLI may **read**
(read-only), and the read-only `inspect_commands` zetta would **run** (each an
`InspectCommand` = a `display` string + the no-shell `argv`; never
provision/deploy/teardown). The catalog performs **no cloud access** — no SDK,
CLI, file read, or network call; `requires_consent()` is false only for Local
(which reads nothing — it is this node), and `runnable()` is true only for the CLI
providers (bare-metal is described but not runnable — its `ssh <host>` needs a
host). The catalog has no untrusted input, so no JSON boundary to validate.

The **real read-only inspect** is `zetta::CloudInspector`
(`include/zetta/cloud_inspector.hpp`, **#445 follow-up slice**): behind the
consent gate it runs a provider's `inspect_commands` through the shared
`CommandRunner` seam (argv, no shell — testable with a fake, no CLI installed) and
projects each vendor's JSON into a `CloudInspectReport` (reachable / authenticated
/ identity / locations / resources / per-command outcomes). It carries **no
credentials** — it runs the read-only commands but never reads or prints the
`config_paths` files. **Every vendor command's JSON is parsed non-throwing and
boundary-validated against an embedded draft-07 schema before any field is read**
(`schemas/cloud_inspect_report.schema.json` `$defs`), so a missing CLI / non-zero
exit / malformed output degrades gracefully; the `CloudInspectReport` is itself
boundary-validated (`parse_cloud_inspect_report`) like the Run record. Consent is
done by the GUI, never by the inspector: `CloudInspector::inspect` performs no
permission check itself.

The panel (list + detail, like Runs/Admin) tags each provider `[open]`/`[lock]`,
resets consent when the selection changes, and gates the "Allow inspection of
`<name>`" action behind the named commands; on consent `App::run_cloud_inspect`
runs the inspect (synchronously, on the UI thread that owns the node) and caches
the report on the node, then renders identity / regions / nodes / per-command
status, with **Re-inspect** / **Revoke**. Following *Conventions → System
tables*, the inspector is also queryable: **`system.zetta_cloud_providers`** (the
catalog as a table — pure metadata, reads nothing) and **`system.zetta_cloud_status`**
(the cached consent-gated inspect results from `Node::cloud_inspections()` — an
**ephemeral, non-replicated** in-memory cache; querying it never triggers a cloud
call). Writes (provision/deploy/teardown) stay out of the inspector entirely. See
[docs/cloud-inspector.md](docs/cloud-inspector.md).

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

The first **real** `StepExecutor` is **`zetta::ChurnStepExecutor`**
(`include/zetta/churn_step_executor.hpp`, **#396**; FEAT-5 / FEAT-6 / UC-1) — it
replaces `DefaultStepExecutor`'s logical-ref placeholders with actual **native
distributed accelerated compute** for the UC-1 churn pipeline, **behind the same
seam and without touching the orchestrator**. It interprets the same
`connect→clean→model→index→serve→act` taxonomy as *real* work, each heavy step a
recorded [`ComputeJob`](#job-coordinator-zettacompute): **connect→load to DDF**
(`load_data_frame`), **clean→featurize** (`run_featurize_job`), **model→train GBDT +
calibrate** (`run_train_job`, which **registers a real `Model`** on the node),
**index→build a real index + score/rank** (builds a real, queryable entity index
over the accounts shard via the resolved `IndexBackend` —
[docs/index-step-executor.md](docs/index-step-executor.md), **#528** — then
`run_score_job` for the at-risk list), **serve→explain**
(`run_explain_job` exact TreeSHAP — the per-entity + population churn drivers), and
**act→retention playbook** (a host-side join of the at-risk list with each entity's
dominant SHAP driver — the one non-job step). So `run_graph`'s churn graph produces a
`Run` whose outputs reference a **real `Model` + ranked at-risk list + drivers** (the
acceptance). Because the seam's `run_step` is a pure-per-step function but the pipeline
threads artifacts (feature table → model → ranked list → drivers), the executor
**carries that state in a pImpl** (mutated through a `unique_ptr` member, so `run_step`
stays `const`) and **holds the dependencies the seam can't supply** — the `Node`, a
started `JobCoordinator`, a `ComputeDevice`, and this node's numeric input shards + the
specs. One instance drives **one** execution; it is used by **injection into a
`GraphExecutor`** (not `Node::run_graph`, which can't conjure a coordinator/data). It
is **SPMD** — on a multi-node cluster every node runs the same graph against its shard
and the composed jobs combine partials over the collectives, so single-node and
multi-node produce equivalent artifacts. It adds **no new JSON format**: every JSON it
touches rides an existing validated boundary (the Run, the ComputeJob records, the GBDT
weights body, the FeaturizeSpec/GbdtParams). See
[docs/churn-step-executor.md](docs/churn-step-executor.md).

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
cloud SDK lives behind the seam.** The first concrete backend is
**`zetta::LocalProvisioner`** (`include/zetta/local_provisioner.hpp`, #374) — the
**given-hardware** path: it does **not** stand up VMs but reports/uses the nodes
**already in the cluster**, best-effort. It reads the live fleet through an
injected **fleet source** (a `std::function<std::vector<ProvisionedNode>()>`
re-consulted each call, so it reflects self-healing membership) rather than owning
a `Cluster`, keeping it in zetta-core and unit-testable; production wiring binds
the source to `Cluster::nodes()` + each node's reported capability once capability
reporting lands (a later slice — `NodeStatus` carries none today). `provision`
selects up to `count` existing nodes that `satisfies` the spec (best-effort: fewer
+ a `detail` note when short — given hardware can't add capacity; a no-floor spec
returns the current cluster nodes); `teardown` is a **no-op returning false**
(given hardware isn't ours to release); `provisioned` lists the whole cluster.
The autoscaler that consumes the seam is unbuilt, and nothing here
persists/replicates yet. See [docs/provisioner.md](docs/provisioner.md).

The **permanent-add control entry point** is **`zetta::add_nodes`**
(`include/zetta/add_nodes.hpp`, **#518**) — the plain "add 10 nodes to this
cluster" action over the `Provisioner` seam. `add_nodes(provider, count, spec)`
drives a provisioner (the cloud-burst `AzureProvisioner`, or any backend) to bring
up N nodes that **auto-join via the seed-peer handshake and persist as ordinary
`node:<id>` members** — the existing cluster-membership + replication path (so it
keeps **no parallel roster**), now automatic with the cloud provisioner. It is the
**permanent sibling** of the [job autoscaler](#job-autoscaler-zettajobautoscaler):
both consume the same `Provisioner&`, but `add_nodes` **only provisions and reports
— it never tears a member down or revokes an identity**, so the members it adds
stay (elastic / autoscaler / cost-cap burst is deliberately deferred). Identity is
the provisioner's concern (its `IdentityHooks`, unrevoked because nothing is torn
down). Vendor-neutral (programs against the abstract seam); best-effort (the
`AddNodesResult` lists what came up). The control command rides a validated
`AddNodesRequest` (`{ count, spec }`) JSON boundary (the hand-written non-throwing
`from_json` pattern of `NodeCapability`/`AzureConfig`, mirroring
`schemas/add_nodes_request.schema.json`). The **`Node`-aware overloads**
(`add_nodes(node, provider, …)`, **#519**) additionally **wire the membership change
to the [between-jobs rebalance](#the-between-jobs-partition-rebalance-is)**: after
the provision they drive `rebalance_partitions(node, grown, previous)`
(`previous` = `Node::cluster_members()`, `grown` = that set + the new member ids) so
**sharded partitions spread onto the new permanent nodes** — visible immediately in
`system.dataset_partitions`, with the bodies following over the sharded-blob-placement
seam (#515). Skipped when nothing new came up; the `tick()` auto-trigger stays the
safety net. See [docs/add-nodes.md](docs/add-nodes.md).

The **`BurstApprovalGate`** (`include/zetta/burst_approval_gate.hpp`) is the first
consumer built on the seam — the **large-burst cost-cap approval gate** (APR-3,
guarding **RSK-9** "runaway burst cost"; #379). It is a `Provisioner` **decorator**
(it *is* a `Provisioner` that wraps another), so it drops in transparently
wherever the autoscaler holds a `Provisioner&`. A provision request **within** a
configured `BurstCap` (two independent, optional axes — `max_nodes` and `max_cost`,
0 = unlimited) passes straight through; a request **over** the cap consults a
`BurstApprover` (deny-by-default — no approver wired ⇒ blocked), **records the
decision in the audit trail** (a new `provision_requested` `AuditAction`), and
either delegates (approved) or returns a blocked, unsatisfied result **without
touching the inner backend** (so nothing comes up, no cost is incurred). `teardown`
/ `provisioned` are not gated (they reduce/report cost). Cost is a pluggable
`CostEstimator`; the default (`default_burst_cost`) is coarse relative compute
units (`count × (cores + gpu tflops)`, so GPU bursts weigh heavily) — real cloud
pricing plugs in with the cloud backends. `BurstCap` carries a **validated JSON
boundary** (`schemas/burst_cap.schema.json`) like the other provisioner DTOs. The
gate audits only the **exception** (an over-cap burst), not every small provision.
See [docs/burst-approval-gate.md](docs/burst-approval-gate.md).

The first concrete **cloud** backend is the **`zetta::AzureProvisioner`**
(`include/zetta/azure_provisioner.hpp`) — the **reference** cloud-burst backend
(**ARC-13 / ADR-20**, #376; AWS #377 / GCP #378 **mirror** its shape). It realizes
the burst lifecycle: **provision** N ephemeral Azure nodes → **mint** bulk burst
identities so they can join the cluster → run cluster-parallel → **tear down** +
**revoke**. It owns the orchestration but **none of the Azure mechanics**,
delegating those to an injected **`AzureControlPlane`** seam (`bring_up`/`destroy`/
`list`) — so it stays in `zetta-core` with **no cloud SDK** and is fully
unit-testable against a fake; the control plane is the **source of truth** for what
is up (`provisioned()` + the drain check read it, no parallel fleet copy). Bulk
identity is wired through two optional **`IdentityHooks`** callables
(`mint`-on-provision / `revoke`-on-full-teardown) — the `FleetSource`/`CostEstimator`
injection style, production-bound to a [`BurstIdentityManager`](docs/burst-identity.md)
for the job, not a hard dependency. The reference `AzureControlPlane` is
**`zetta::AzCliControlPlane`** (`include/zetta/azure_cli_control_plane.hpp`), which
borrows the **VMSS + Spot shape** (not the SDK) by shelling out to the `az` CLI
through the **shared `CommandRunner`** (`include/zetta/command_runner.hpp` — argv,
no shell, testable without `az` installed; the default `fork`/`execvp`s and
captures JSON; the same exec seam the AWS/GCP CLI bindings reuse); its `list`
output is parsed at the **`ProvisionedNode` boundary**. `AzureConfig` (subscription
/ resource group / region required; pool name / VM SKU / image / spot + max-price /
seed peer / managed identity optional) carries a **validated JSON boundary**
(`schemas/azure_config.schema.json`) like the other provisioner DTOs. No live-cloud
CI (the acceptance is the full *shape*, verified against fakes), no
persistence/replication, no autoscaler wiring yet. **AWS (#377) / GCP (#378) mirror
this shape** — the recipe (the four pieces + shared infra + the lifecycle→CLI
mapping; **CLI for now**) is [docs/cloud-provisioner-pattern.md](docs/cloud-provisioner-pattern.md);
the cited `az` command basis is
[docs/research/azure-cli-compute.md](docs/research/azure-cli-compute.md). See
[docs/azure-provisioner.md](docs/azure-provisioner.md).

**`zetta::AwsProvisioner`** (`include/zetta/aws_provisioner.hpp`, #377) is the
**AWS mirror** of that reference, following the
[cloud-provisioner pattern](docs/cloud-provisioner-pattern.md) — the four pieces
(`AwsProvisioner` / `AwsControlPlane` seam / `AwsCliControlPlane` CLI binding /
`AwsConfig`) plus the shared `CommandRunner` + `IdentityHooks`, all reused. The
`AwsProvisioner` orchestration is structurally a **near-copy** of
`AzureProvisioner` (only the config type differs): provision → `bring_up` + mint,
teardown → `destroy` + revoke-on-drain, `provisioned` → `list`. The
**`AwsCliControlPlane`** is the reference binding, borrowing the **EC2 Auto Scaling
Group + Spot shape** (the AWS analog of Azure's VMSS) by shelling out to the `aws`
CLI through the shared `CommandRunner` — `create-auto-scaling-group` /
`set-desired-capacity` (idempotent re-burst on the stable pool name) →
`terminate-instance-in-auto-scaling-group` → `describe-instances` (parsed at the
`ProvisionedNode` boundary). **`AwsConfig`** (region / launch template / subnets
required; pool name / instance-type override / spot + max-price / seed peer /
profile optional) carries a **validated JSON boundary**
(`schemas/aws_config.schema.json`) like the other provisioner DTOs; Spot rides on
the ASG's mixed-instances policy and the per-node IAM instance profile + join
bootstrap live in the launch template. Same scope caveats as Azure: no live-cloud
CI (verified against fakes), no persistence/replication, no autoscaler wiring; the
cited `aws` command basis is
[docs/research/aws-cli-compute.md](docs/research/aws-cli-compute.md). See
[docs/aws-provisioner.md](docs/aws-provisioner.md).

**`zetta::GcpProvisioner`** (`include/zetta/gcp_provisioner.hpp`, **ARC-13 /
ADR-20**, #378) is another **mirror** of that reference — the same four-piece
shape (the [CLI pattern](docs/cloud-provisioner-pattern.md)) on GCP.
`GcpProvisioner` is the burst orchestration (provision+mint / teardown+revoke,
structurally identical to `AzureProvisioner` but carrying a `GcpConfig`),
delegating to an injected **`GcpControlPlane`** seam (`bring_up`/`destroy`/`list`)
with the same optional `IdentityHooks`. Its reference binding
**`zetta::GcpCliControlPlane`** (`include/zetta/gcp_cli_control_plane.hpp`) borrows
the **Managed Instance Group + Spot shape** by shelling out to `gcloud` through the
**shared `CommandRunner`** — `bring_up` ensures an instance template (machine type
/ image / network / service account / `--provisioning-model=SPOT` / a
startup-script seed peer), then creates **and** resizes the MIG to N (create-or-
resize makes a re-burst idempotent on the stable pool name) and lists its
instances; `destroy` is `instance-groups managed delete-instances`; `list` parses
`gcloud … --format json` at the **`ProvisionedNode` boundary** (skip-malformed).
`GcpConfig` (`project_id` + `zone` required; pool/MIG name, instance template,
machine type, image, network, subnetwork, service account, spot, seed peer
optional) carries a **validated JSON boundary** (`schemas/gcp_config.schema.json`)
like `AzureConfig`. No live-cloud CI (the `gcloud` exec is the only path CI can't
run — argv + parsing are fully faked), no persistence/replication/autoscaler wiring
yet. The cited `gcloud` command basis is
[docs/research/gcp-cli-compute.md](docs/research/gcp-cli-compute.md). See
[docs/gcp-provisioner.md](docs/gcp-provisioner.md).

**`zetta::OciProvisioner`** (`include/zetta/oci_provisioner.hpp`, **ARC-13 /
ADR-20**, #432) is another mirror — the same four-piece shape on **Oracle Cloud
Infrastructure**. `OciProvisioner` is the burst orchestration (provision+mint /
teardown+revoke, structurally identical to Azure/GCP but carrying an `OciConfig`),
delegating to an injected **`OciControlPlane`** seam with the same `IdentityHooks`.
Its reference binding **`zetta::OciCliControlPlane`**
(`include/zetta/oci_cli_control_plane.hpp`) borrows the **Instance Pool +
preemptible shape** by shelling out to `oci` through the **shared `CommandRunner`**.
The OCI-specific wrinkle: OCI resources are **OCID-addressed, not name-addressed**,
so `bring_up` first **resolves the pool's OCID from its stable display name**
(`instance-pool list --query data[0].id --raw-output`), then create-or-resizes — an
empty result takes the **create path** (`instance-configuration create
--instance-details <json>` carrying the per-node shape incl. `preemptibleInstanceConfig`
for spot + a `user_data` seed, then `instance-pool create … --size N`), a non-empty
one the **resize path** (`instance-pool update --size N`); `destroy` is
`instance-pool-instance detach --is-decrement-size --is-auto-terminate`; `list`
unwraps the `{"data":[…]}` envelope at the **`ProvisionedNode` boundary**.
`OciConfig` (`compartment_id` + `availability_domain` + `subnet_id` required;
region, pool name, shape + ocpus/memory, image, spot, seed peer optional) carries a
**validated JSON boundary** (`schemas/oci_config.schema.json`). No live-cloud CI
(argv + parsing fully faked), no persistence/replication/autoscaler wiring yet. The
cited `oci` command basis is
[docs/research/oci-cli-compute.md](docs/research/oci-cli-compute.md). See
[docs/oci-provisioner.md](docs/oci-provisioner.md).

**`zetta::K8sProvisioner`** (`include/zetta/k8s_provisioner.hpp`, **ARC-13 /
ADR-20**, #434) is another mirror — the same four-piece shape over **Kubernetes**.
Unlike the cloud backends (which create VMs), it bursts **worker pods into an
existing cluster**: the burst pool is a **Deployment** scaled to N replicas (the
self-healing, maintain-N analog of VMSS/ASG/MIG/Instance Pool), and the cluster is
the kubeconfig **context** (configured out-of-band, e.g. `get-credentials`). This is
distinct from the `DeploymentTarget` k8s placement (where a *graph* runs) — this
provisions *compute nodes*. `K8sProvisioner` is the burst orchestration
(provision+mint / teardown+revoke, structurally identical to the siblings but
carrying a `K8sConfig`), delegating to an injected **`K8sControlPlane`** seam with
the same `IdentityHooks`. Its reference binding **`zetta::K8sCliControlPlane`**
(`include/zetta/k8s_cli_control_plane.hpp`) shells out to `kubectl` through the
**shared `CommandRunner`** — `bring_up` ensures the Deployment (`kubectl create
deployment --image`), applies the per-pod shape (`set env ZETTA_SEED_PEER`, `set
serviceaccount`, a spot `patch` nodeSelector), then `scale`s to N (the gate) and
lists pods (`get pods --selector app=<name> -o json`, unwrapping the `{"items":[…]}`
envelope at the **`ProvisionedNode` boundary**); `destroy` is `kubectl delete pod`;
every call carries `--namespace`/`--context`. `K8sConfig` (`image` required;
namespace, deployment name, context, service account, spot, seed peer optional; the
`namespace` JSON key maps to the C++ `namespace_name`) carries a **validated JSON
boundary** (`schemas/k8s_config.schema.json`). No live-cluster CI (argv + parsing
fully faked), no persistence/replication/autoscaler wiring yet. The cited `kubectl`
command basis is
[docs/research/kubectl-workloads.md](docs/research/kubectl-workloads.md). See
[docs/k8s-provisioner.md](docs/k8s-provisioner.md).

**`zetta::BareMetalProvisioner`** (`include/zetta/bare_metal_provisioner.hpp`,
**ARC-13 / ADR-20**, #436) is another mirror — the same four-piece shape over the
**local host**. Bare metal is the local machine (not a remote cloud/cluster), and
each worker runs as a **local container** via the host runtime (`docker`/`podman`):
`docker run -d` starts a detached worker and **returns immediately**, which is what
makes a long-lived worker launchable through the *synchronous* `CommandRunner`
(fork/execvp). It is distinct from `LocalProvisioner` (#374, which only *reports*
existing cluster nodes and never creates any) — this actually **brings up** local
workers. `BareMetalProvisioner` is the burst orchestration (provision+mint /
teardown+revoke, structurally identical to the siblings but carrying a
`BareMetalConfig`), delegating to an injected **`BareMetalControlPlane`** seam with
the same `IdentityHooks`. Its reference binding **`zetta::BareMetalCliControlPlane`**
(`include/zetta/bare_metal_cli_control_plane.hpp`) shells out to the runtime through
the **shared `CommandRunner`** — `bring_up` counts the labelled burst workers
(`<runtime> ps --filter label=<pool> --format '{{json .}}'`), launches only the
**shortfall** (`<runtime> run -d --label <pool> --name <prefix>-<uuid> [--network …
--gpus all --env ZETTA_SEED_PEER=…] <image>`, idempotent on the label), then lists;
`destroy` is `<runtime> rm -f <id>`; `list` parses the runtime's **NDJSON** (one JSON
object per line) at the **`ProvisionedNode` boundary**. `BareMetalConfig` (`image`
required; runtime docker/podman, pool label, name prefix, network, gpus, seed peer
optional) carries a **validated JSON boundary** (`schemas/bare_metal_config.schema.json`).
No live-runtime CI (argv + parsing fully faked), no persistence/replication/autoscaler
wiring yet; a raw-process (systemd/launchd) variant is a possible future sibling. The
cited command basis is
[docs/research/baremetal-compute.md](docs/research/baremetal-compute.md). See
[docs/bare-metal-provisioner.md](docs/bare-metal-provisioner.md).

### Bulk burst identity + teardown (`zetta::BurstIdentityManager`)

When the cluster **bursts** compute nodes for a job, each burst node needs an
identity to authenticate. The **`BurstIdentityManager`**
(`include/zetta/burst_identity.hpp`) mints those **in bulk on provision** and
**revokes them in bulk on teardown**, **reusing** the existing identity model
(`Principal` + `AccessKey`, #304) — no new identity system (ARC-13; mitigates
RSK-9, #375). `mint(job_id, nodes, scope, capabilities)` creates one **service**
`Principal` + one `AccessKey` per `ProvisionedNode` — scoped to the job (pass a
`Scope` naming its project) and short-lived (TTL the capabilities) — and returns a
`BurstIdentity` (`{ node_id, principal_id, key_id }`) per node; `revoke(job_id)`
bulk-drops them. The job↔identity link is **derived, not a parallel copy of
state**: the service principal is named `burst/<job-id>/<node-id>`, so `minted()`
and `revoke()` reconstruct the set from the node's replicated principals (it
survives restart). Both edges are **audited** via the Node's `AuditSink` —
`key_issued` per mint, `key_revoked` per revoke (the two new `AuditAction`s above).
Both are **idempotent** (re-mint returns the existing identity; revoke of an
unknown job is a no-op). Pairs with the `Provisioner` seam; getting the key onto
the burst node is a later slice. See [docs/burst-identity.md](docs/burst-identity.md).

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
    zetta-core's libc++ ABI (propagated from Boost::capy). **Schema as code:** the
    embedded draft-07 schema in the C++ (exposed via a `*_schema()` accessor) is the
    single source of truth; `schemas/*.json` is **generated** from it and
    **drift-checked** by `tests/test_schemas.cpp` (regenerate with
    `ZETTA_WRITE_SCHEMAS=1`) — see [`docs/schemas.md`](docs/schemas.md).
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
CMakeLists.txt              build graph: imgui lib, zetta-compute lib (+ optional CUDA), zetta-core lib, zetta exe
cmake/embed_font.cmake      bakes a .ttf into a C++ header at configure time
assets/fonts/               embedded Geist + Geist Mono TTFs (SIL OFL 1.1) + README
scripts/install-deps.sh     install system build tools (macOS / Ubuntu)
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the node; CRUD over databases + projects + deployments + runs + models + indexes + jobs + dataset partitions + principals; the per-node IndexRegistry (index_registry(), #528); the per-node checkpoint store (checkpoint_store()/checkpoints(), <data_dir>/checkpoints, RULE-5/#468); run_graph; the audit sink; cluster_members() (the replicated node:<id> membership, #519)
  database.hpp              zetta::Database — owns tables (disk-backed)
  project.hpp               zetta::Project — an AI project; owns inputs + execution graphs
  project_input.hpp         zetta::ProjectInput — raw intent/artifact input + provenance
  project_graph.hpp         zetta::ProjectGraph — a named ExecutionGraph under a project
  deployment_target.hpp     zetta::DeploymentTarget — a placement (ENT-6; R1 local)
  deployment.hpp            zetta::Deployment — a graph placed onto a target
  deployment_controller.hpp zetta::DeploymentController — reconcile local deployments desired→actual + self-heal (FEAT-4/TC-3)
  node_capability.hpp       zetta::NodeCapability/Gpu/GpuKind — node compute capability model (cores/GPU/memory) + satisfies (ENT-32)
  provisioner.hpp           zetta::Provisioner/ProvisionedNode — provision N nodes against a capability spec / teardown / list (ARC-13; #373)
  add_nodes.hpp             zetta::add_nodes/AddNodesRequest/AddNodesResult — permanent-add control entry point: drive a Provisioner to bring up N nodes that auto-join + persist as members; provision-only (never teardown/revoke), the autoscaler's permanent sibling; validated AddNodesRequest JSON boundary (#518). The add_nodes(Node&, …) overloads also trigger the between-jobs partition rebalance onto the new members (#519)
  burst_provisioner.hpp     zetta::BurstProvisioner<Config, ControlPlane> — the shared cloud-burst orchestration template (provision+mint / teardown+revoke-on-drain / provisioned); every cloud backend subclasses it so the orchestration lives once (#464)
  local_provisioner.hpp     zetta::LocalProvisioner — given-hardware Provisioner backend: uses existing cluster nodes best-effort, teardown is a no-op (ARC-13; #374)
  azure_provisioner.hpp     zetta::AzureProvisioner/AzureConfig/AzureControlPlane — reference cloud-burst backend: burst N Azure nodes + mint identities + teardown/revoke, behind an injectable control-plane seam, no cloud SDK (ARC-13/ADR-20; #376)
  azure_cli_control_plane.hpp zetta::AzCliControlPlane — reference AzureControlPlane binding: shells out to the `az` CLI (VMSS/Spot shape) via the shared CommandRunner (#376)
  command_runner.hpp        zetta::CommandRunner/CommandResult/default_command_runner — shared, vendor-neutral exec seam every CLI cloud binding runs through (argv, no shell; fork/execvp; #376)
  aws_provisioner.hpp       zetta::AwsProvisioner/AwsConfig/AwsControlPlane — AWS cloud-burst backend mirroring the Azure pattern: burst N nodes + mint identities + teardown/revoke behind an injectable control-plane seam, no cloud SDK (ARC-13/ADR-20; #377)
  aws_cli_control_plane.hpp zetta::AwsCliControlPlane — reference AwsControlPlane binding: shells out to the `aws` CLI (EC2 Auto Scaling Group / Spot shape) via the shared CommandRunner (#377)
  gcp_provisioner.hpp       zetta::GcpProvisioner/GcpConfig/GcpControlPlane — GCP cloud-burst backend mirroring Azure: burst N + mint identities + teardown/revoke behind the control-plane seam, no cloud SDK (ARC-13/ADR-20; #378)
  gcp_cli_control_plane.hpp zetta::GcpCliControlPlane — reference GcpControlPlane binding: shells out to `gcloud` (Managed Instance Group/Spot shape) via the shared CommandRunner (#378)
  oci_provisioner.hpp       zetta::OciProvisioner/OciConfig/OciControlPlane — OCI cloud-burst backend mirroring Azure: burst N + mint identities + teardown/revoke behind the control-plane seam, no cloud SDK (ARC-13/ADR-20; #432)
  oci_cli_control_plane.hpp zetta::OciCliControlPlane — reference OciControlPlane binding: shells out to `oci` (Instance Pool/preemptible shape, OCID-resolved by name) via the shared CommandRunner (#432)
  k8s_provisioner.hpp       zetta::K8sProvisioner/K8sConfig/K8sControlPlane — Kubernetes cloud-burst backend mirroring Azure: burst N pods into an existing cluster + mint identities + teardown/revoke behind the control-plane seam, no client lib (ARC-13/ADR-20; #434)
  k8s_cli_control_plane.hpp zetta::K8sCliControlPlane — reference K8sControlPlane binding: shells out to `kubectl` (Deployment/scale shape) via the shared CommandRunner (#434)
  bare_metal_provisioner.hpp zetta::BareMetalProvisioner/BareMetalConfig/BareMetalControlPlane — bare-metal (local-host) cloud-burst backend mirroring Azure: burst N local worker containers + mint identities + teardown/revoke behind the control-plane seam, no container SDK; bursts (vs LocalProvisioner which reports) (ARC-13/ADR-20; #436)
  bare_metal_cli_control_plane.hpp zetta::BareMetalCliControlPlane — reference BareMetalControlPlane binding: shells out to docker/podman (local-container shape, launch-the-shortfall) via the shared CommandRunner (#436)
  burst_identity.hpp        zetta::BurstIdentityManager/BurstIdentity — bulk-mint/revoke short-lived service Principal+AccessKey per burst node, audited (ARC-13/RSK-9; #375)
  scaling_policy.hpp        zetta::ScalingPolicy — per-job autoscaler reward objective (cost/runtime/data-shape) + bounds + evaluate() + JSON boundary (ADR-29/ENT-34; #370)
  burst_approval_gate.hpp   zetta::BurstApprovalGate/BurstCap — large-burst cost-cap approval gate: a Provisioner decorator, block over-cap bursts pending approval + audit (APR-3/RSK-9; #379)
  job_autoscaler.hpp        zetta::JobAutoscaler/AutoscalerConfig — autoscaler control loop: observe a running ComputeJob, score its ScalingPolicy, add/remove nodes via the Provisioner within bounds, hysteresis + cost-cap shed, elastic membership, best-effort on given hardware (ADR-29/ARC-11; #371)
  solution_catalog.hpp      zetta::SolutionCatalog — AI Solution Library (deployable blueprints) + schema-validated parse
  cloud_provider.hpp        zetta::CloudProvider/CloudProviderCatalog — pure-metadata catalog of deployment-target backends for the permission-gated Cloud Inspector (no cloud access); each inspect command is an InspectCommand (display + no-shell argv)
  cloud_inspector.hpp       zetta::CloudInspector/CloudInspectReport — the real read-only inspect: run a provider's inspect_commands via the shared CommandRunner (behind GUI consent), parse each vendor's JSON at a validated boundary → identity/regions/nodes; no creds read (#445)
  run.hpp                   zetta::Run — one graph execution (status/logs/outputs/lineage) + Run JSON Schema
  model.hpp                 zetta::Model — a trained model (metadata + weights fingerprint) + Model JSON Schema (ENT-14/FEAT-6)
  index/
    index_kind.hpp          zetta::IndexKind — the search/vector/feature index-type taxonomy (header-only enum + names) (ENT-39/ENT-40)
    index_backend.hpp       zetta::IndexBackend/IndexSpec/IndexStatus/IndexPartition/IndexQuery — the build(over partitions)/query/drop seam (ADR-34; no-throw status)
    index_registry.hpp      zetta::IndexRegistry + index_spec_from_node — register backends, resolve an IndexSpec / IR `index` step → a backend (like compute::DeviceRegistry); the Node owns one (Node::index_registry(), #528)
    index_step_executor.hpp zetta::IndexStepExecutor + read_index_partitions — the StepExecutor that runs a deployed IR `index` step: resolve backend → read table rows into partitions → build a real index → record the Index entity → emit an `index` output (queryable via the node registry); non-index kinds delegate to DefaultStepExecutor (#528)
    in_memory_index_backend.hpp zetta::InMemoryIndexBackend — the from-scratch in-memory Search reference backend (inverted field→row-ids over partitions; name "memory_search")
    oramacore_fulltext_index.hpp zetta::OramaCoreFulltextIndex — the first real backend: a full-text (BM25F) Search backend over the native oramacore-cpp engine (name "oramacore_fulltext"); pImpl over the engine, ZETTA_WITH_ORAMACORE-guarded so a checkout without the private submodule still builds (#527)
    distributed_fulltext.hpp zetta::DistributedFulltextIndex/merge_fulltext_hits/FulltextHit — per-partition distributed full-text: build a sub-index per LOCAL owned partition (compute-near-data) into an owned IndexBackend, answer a query by scatter-gather (the fulltext.query signal-plane RPC, schema-validated) + merge/rank across nodes → a global ranking matching the consolidated-index oracle; the SignalHandler chained over a delegate (DistributedSorter pattern) (#529, TC-13 #503)
    index.hpp               zetta::Index/IndexValue — the top-level replicated index entity {table,kind,backend,fields} + Index JSON Schema (ENT-39/ENT-40)
  compute_job.hpp           zetta::ComputeJob — one unit of accelerated compute (kind/lifecycle) + ComputeJob JSON Schema (ARC-11/ENT-31)
  dataset_partition.hpp     zetta::DatasetPartition — a partition catalog entry: where a table's data slice lives (table/blob_ref/row_range/stats/owner_nodes) + JSON Schema; replicate-to-all; set_owner_nodes is the capturing re-placement mutator the rebalance uses (ENT-36/TBL-13; #517)
  principal.hpp             zetta::Principal — an authenticated actor; owns access keys
  access_key.hpp            zetta::AccessKey — a scoped + capability-bearing login credential + authorize()
  scope.hpp                 zetta::Scope/Resource — deny-by-default key-scoped access, the resource axis (RULE-3)
  capability.hpp            zetta::Capability/CapabilitySet/Operation — deny-by-default rights, the verb axis, TTL'd (ENT-22)
  audit_event.hpp           zetta::AuditEvent/AuditSink — immutable audit trail + JSON Schema (ENT-23)
  intent_compiler.hpp       zetta::compile_intent — NL intent → templated ExecutionGraph skeleton (FEAT-2)
  executor.hpp              zetta::GraphExecutor / StepExecutor — orchestrate a graph into a Run
  churn_step_executor.hpp   zetta::ChurnStepExecutor — the real churn StepExecutor: connect→load-to-DDF, clean→featurize, model→train+calibrate (registers a Model), index→build a real entity index (#528) + score/rank, serve→explain (TreeSHAP), act→playbook, each heavy step a ComputeJob; behind the seam, orchestrator untouched (FEAT-5/FEAT-6/UC-1; #396)
  churn_solution.hpp        zetta::run_churn_solution — wire the catalog churn blueprint to the real engine: encode a ChurnDataset → numeric DDF shards, build the standard featurize spec + GBDT params, inject a ChurnStepExecutor, run the graph → real Model + ranked at-risk list + drivers + playbook; + demo_churn_dataset (no-files demo) (FEAT-4/UC-1; #398)
  table.hpp                 zetta::Table — owns a storage engine (its rows); configurable per-table with a Partitioner
  partitioner.hpp           zetta::Partitioner/SinglePartitioner — the partitioning seam: record→partition + partition→node(s) over a fixed P>>nodes, pluggable per table (ADR-31/ENT-37; #512); + the record→partition strategies HashPartitioner (FNV-1a hash(key columns)%P) / RoundRobinPartitioner (surrogate%P for even balance) + the partitioner_from_json/to_json config boundary (#513)
  rendezvous_placement.hpp  zetta::RendezvousPlacement/rendezvous_owners — rendezvous (highest-random-weight) partition→node placement: a membership change moves only ~1/N of partitions, deterministic + leaderless (ADR-31/ENT-37; #514)
  partition_writer.hpp      zetta::write_partitioned/PartitionWriteResult — the partitioned write path: assign each row to a partition (Partitioner), serialize each non-empty partition to a JSON Lines blob, store it as a LargeObject (import_bytes), record a DatasetPartition catalog entry per partition (blob ref + cumulative row range + place_partition owners); N blobs + N catalog rows (ADR-31/ENT-37; #511)
  partition_rebalance.hpp   zetta::rebalance_partitions/RebalanceResult — between-jobs rebalance on a membership change: recompute rendezvous placement for the sharded partitions preserving rf=owner count, update changed owner_nodes (set_owner_nodes captures → bodies move via #515); skip replicate-to-all; ≈rf/N move on a join. Two triggers: directly from add_nodes(Node&, …) (#519) and auto-triggered in Node::tick off node:<id> membership keys (ADR-31/ENT-37; #517, TC-10/#500)
  large_object.hpp          zetta::LargeObjectStore — files as blobs (per database); import_file (from a path) + import_bytes (from in-memory bytes, used by the partitioned write path)
  import.hpp                zetta::import_records — JSON/JSONL files → table rows
  churn_dataset.hpp         zetta::ChurnDataset / load_churn_dataset — synthetic UC-1 churn fixture loader (FEAT-6 / #402)
  n8n_import.hpp            zetta::import_n8n_workflow — n8n workflow JSON → artifact ProjectInput (schema-validated)
  n8n_compile.hpp           zetta::compile_n8n_workflow — n8n workflow JSON → ExecutionGraph IR (nodes→steps, connections→edges; #308)
  input/
    language_frontend.hpp   zetta::input::LanguageFrontend/FrontendResult — interface: lower a source language to the ExecutionGraph IR (the IR-targeting analogue of query::Dialect); non-throwing (ENT-38/ADR-33; #520)
    python_frontend.hpp     zetta::input::PythonFrontend/python_step_kind — simulated-Python frontend: parse a Python subset (assignments/calls/attribute+method chaining/literals) → IR (calls→typed steps via the engine-faithful verb table — train→model, score/predict/rank→index, explain→serve, add_nodes→act; refs/receivers→data-flow edges); not a Python runtime (#520/#522)
    cluster_bindings.hpp    zetta::input::run_add_nodes_step/add_nodes_count — execute a lowered sim-Python cluster.add_nodes IR node against the permanent-add control action zetta::add_nodes (provider injected, cloud consent-gated) (#522)
    datasets_bindings.hpp   zetta::input::run_load_dataset_step/DatasetStepExecutor/dataset_load_spec/DatasetResolver — the `datasets` library shim: execute a lowered sim-Python load_dataset("<path>", columns=…) IR node by decoding the parquet via the native reader (read_parquet_rows) into a (partitioned) table; file/network IO behind the injectable DatasetResolver seam (offline default = local path); HF datasets is the fidelity reference only (#521)
  uuid.hpp                  zetta::new_uuid() — v4 UUID for folder names
  sha256.hpp                zetta::sha256_hex — large-object body fingerprint
  timestamp.hpp             zetta::now_iso8601() — UTC ISO-8601 stamp (created/updated)
  validation.hpp            zetta::ValidationReport — correctness-gate result (ENT-5/FEAT-3)
  gate.hpp                  zetta::CorrectnessGate — gate runner: checks → report (RULE-1)
  graph_checks.hpp          zetta::SchemaTypeCheck + DataLeakageCheck — the #309 concrete checks
  checks/
    entity_resolution_check.hpp  zetta::EntityResolutionCheck — division-correct resolution (TC-4)
  entity_resolution.hpp     zetta::EntityResolver — resolve raw comms mentions → CRM accounts division-correct (FullName) / coarse Brand trap; emit the entity_resolution annotation the gate verifies (FEAT-6/TC-4; #394)
  bytes.hpp / duration.hpp  human-readable size / time formatters
  runtime.hpp               zetta::runtime — async/coroutine seam over Capy (task/block_on/thread_pool + parallel_for data-parallel fan-out)
  runtime/net.hpp           networking slice of the seam (Corosio; adapter-only)
  cluster.hpp               zetta::Cluster — equal-peer membership over TCP + the signal-plane API
  signal.hpp                zetta::Signal/SignalHandler — ephemeral RPC/event/task signals + envelope encode/decode (ADR-28)
  collectives.hpp           zetta::Collectives — allreduce/broadcast/barrier/gather over the signal plane (#365)
  replication.hpp           zetta::ReplicationManager — Automerge CRDT state
  replication_strategy.hpp  zetta::ReplicationStrategy — per-table/db replication strategy (ReplicateToAll | Partitioned | PartitionedWithReplicas(factor)) + JSON boundary; recorded/persisted/replicated, default ReplicateToAll (ENT-35; #509)
  blob_store.hpp            zetta::BlobStore — large-object body transfer seam
  replicated_blob_store.hpp zetta::ReplicatedBlobStore — BlobStore from CRDT + disk; partition-aware placement: a partition body is fetched/held only by its owner node(s) per the catalog + replication factor, not replicate-to-all (#515)
  headless.hpp              zetta::run_headless(Node&, Cluster&)
  signals.hpp               install_signal_handlers / stop_requested
  storage/
    engine.hpp              zetta::StorageEngine interface
    json_store.hpp          zetta::JsonStore — in-memory vector<json> (header-only)
    jsonl_store.hpp         zetta::JsonlStore — JSONL on disk, 10k/chunk
  parquet/                  zetta::parquet — the native Parquet reader (ADR-32; no Arrow/parquet-cpp)
    snappy.hpp              zetta::parquet::snappy_decompress — Snappy raw-block page decoder (Parquet's default codec), non-throwing + bounds-checked (#507)
  ir/
    step_kind.hpp           zetta::ir::StepKind — connect→…→act taxonomy (header-only)
    execution_graph.hpp     zetta::ir::ExecutionGraph — typed step nodes + edges
    serialization.hpp       zetta::ir::to_json/from_json — the graph's JSON form
  parquet/                  zetta::parquet — native (no Arrow) Parquet reader (ADR-32/#481)
    metadata.hpp            zetta::parquet::parse_parquet_metadata — read a file's Thrift FileMetaData (schema + row-group/column-chunk layout, incl. per-leaf max def/rep levels); Thrift compact decoder, non-throwing, JSON projection + schema boundary (#505)
    column_decoder.hpp      zetta::parquet::decode_column/decode_column_chunk — read a column's data pages into typed values + a validity bitmap: PLAIN (int/float/bool/byte_array) + dictionary (RLE/bit-packed indices) + definition levels; uncompressed + snappy codecs (zstd/gzip rejected); non-throwing, JSON projection + schema boundary (#506)
    reader.hpp              zetta::parquet::read_parquet_rows — assemble metadata+decoders into a whole-file reader: decode every leaf column → one JSON object per row (the inverse of the JsonlStore row shape); the untyped-rows reader the churn loader uses (#508)
  compute/                  zetta::compute — the accelerator seam (ADR-27/ARC-9; no SDL/CUDA in headers)
    data_type.hpp           zetta::compute::DataType — element-type tag + byte_size/data_type_of<T> (header-only)
    compute_status.hpp      zetta::compute::ComputeStatus — the no-throw call result (header-only)
    device_buffer.hpp       zetta::compute::DeviceBuffer — typed, owning, move-only device memory
    compute_device.hpp      zetta::compute::ComputeDevice — the seam: info + memory + the operator set
    host_stub_device.hpp    zetta::compute::HostStubDevice — host-memory transfer stub (ops unsupported)
    cpu_device.hpp          zetta::compute::CpuDevice — the always-built reference backend (every op on host memory, multi-threaded; #360)
    cuda_device.hpp         zetta::compute::CudaDevice — the GPU backend (CUDA-free header; guarded by ZETTA_WITH_CUDA, #363)
    device_registry.hpp     zetta::compute::DeviceRegistry — per-node enumerate/report/select devices (ARC-9; #362)
    work_stealing_scheduler.hpp  zetta::compute::WorkStealingScheduler — local per-worker-deque work-stealing task pool (ARC-10/#366)
    distributed_scheduler.hpp  zetta::compute::DistributedScheduler — cross-node steal over the signal plane, elastic mid-job membership (ARC-10/#366/#367)
    job_coordinator.hpp  zetta::compute::JobCoordinator/JobRound — split a ComputeJob into tasks on the scheduler, drive collectives at round boundaries, drive job lifecycle (ARC-11/#372)
    checkpoint.hpp  zetta::compute::JobCheckpoint/CheckpointStore/FileCheckpointStore/run_resumable_rounds — burst-job eviction tolerance: durable sha256-fingerprinted progress snapshot (validated boundary) + a durable store seam + a resume-from-last-checkpoint driver (graceful spot-drain), so an evicted job resumes without recomputing committed rounds (RULE-5/RSK-9; #468)
  ddf/
    data_frame.hpp          zetta::ddf::DataFrame/Column/DataFrameSchema — typed columnar frame over a table, chunked, ComputeDevice-backed + loader/infer/JSON boundary (ARC-12/ENT-33; #380)
    parquet_reader.hpp      zetta::ddf::load_parquet_data_frame/infer_parquet_schema — native Parquet → DataFrame reader with column projection: decode only the requested leaf columns into DeviceBuffers (drop embedding_0..3071), numeric/dense boundary + parquet_data_frame schema for the golden (ADR-32; #508)
    ops.hpp                 zetta::ddf::scan/filter/project — DDF ops over a DataFrame, built on the ComputeDevice seam (gather-based filter, map/elementwise computed columns) + FilterPredicate/Projection JSON boundaries (ARC-12; #381)
    join.hpp                zetta::ddf::distributed_join — DDF distributed inner equi-join: repartition by key via collectives (gather+broadcast) then local hash/sort-merge join per shard (ARC-12; #382)
    data_frame_ops.hpp      zetta::ddf::sort/concat/slice — frame-shaping DDF ops: order by a key column (device sort_by_key), row-wise union, contiguous row range (ARC-12; #385)
    distributed_sort.hpp    zetta::ddf::DistributedSorter/PartitionMessage — distributed sort across the cluster (sample-partition + local sort + merge) over collectives + the signal-plane all-to-all shuffle (ARC-12; #385)
    group_by.hpp            zetta::ddf::group_by/distributed_group_by — DDF groupby-aggregate (count/sum/min/max/mean), partial-per-node + gather/merge/broadcast combine + GroupBySpec JSON boundary (ARC-12; #383)
    as_of.hpp               zetta::ddf::as_of_aggregate/AsOfAggregate — DDF window / as-of (point-in-time) op: per-entity reduce of rows in a time window (time <= as_of), the leakage-free featurization primitive + JSON boundary (ARC-12; #384)
    group_by_job.hpp        zetta::ddf::run_group_by_job — run the distributed groupby-aggregate as a recorded ComputeJob (kind ddf) through the job coordinator (drives distributed_group_by on the JobRound; result on every node) (FEAT-8; #387)
    local_partitions.hpp    zetta::ddf::load_local_partitions/LocalShardResult — compute-near-data shard loader: enumerate this node's OWNED partitions of a table (owner_nodes ∋ node_id) from the catalog, read only the locally-held blob bodies, parse the JsonlStore {_zid,doc} lines, load into a local DataFrame; instrumented bytes_read (near data, not moved) (ARC-12/FEAT-8; #516)
    local_group_by_job.hpp  zetta::ddf::run_near_data_group_by_job/NearDataGroupByResult — the first-class compute-near-data op: load the local shard via load_local_partitions → run_group_by_job, reporting local_bytes_read vs wire_bytes_moved (the partials, via Collectives::wire_bytes()); result == single-node oracle (ARC-12/FEAT-8; #516)
    featurize.hpp           zetta::ddf::point_in_time_featurize/FeaturizeSpec/FeatureTable — point-in-time (leakage-free) featurization: churn_features.sql as a DDF graph (as-of + group-by + filter + join) → a feature table for training (time <= as_of) + JSON boundary; also a static carry-through mode (empty event_features → carry engineered columns + label, point-in-time row filter where a time_column is set, #532) (ARC-12/FEAT-6; #388)
    featurize_job.hpp       zetta::ddf::run_featurize_job — run the point-in-time featurization as a recorded `featurize` ComputeJob across the cluster (distributed_join + distributed_group_by on the JobRound; global table on every node) (ARC-12/FEAT-6; #388)
    local_featurize_job.hpp zetta::ddf::run_near_data_featurize_job/NearDataFeaturizeResult — compute-near-data featurize over a single partitioned table: load this node's owned partitions via load_local_partitions → run_featurize_job over the local shard; the engineered-columns slice of UC-1 at scale, result == single-node oracle (ARC-12/FEAT-8; #532)
  ml/
    gbdt.hpp                zetta::ml — histogram GBDT (binary logistic): GbdtParams/GbdtModel/RegressionTree (+ JSON boundaries), evaluate_binary (AUC/log-loss), train_gbdt (host-side reference, gradient/Hessian histograms allreduced via an injected AllReduce — single-node == multi-node) (FEAT-6/ENT-14; #390)
    gbdt_train_job.hpp      zetta::ml::run_train_job — run distributed GBDT training as a recorded `train` ComputeJob through the job coordinator and produce a Model (weights body + metrics + ranked drivers, registered on the root); opt-in held-out probability calibration via CalibrationConfig; opt-in per-round checkpoint/restart via TrainCheckpointConfig (resume the partial model from the last checkpoint after a spot eviction, cluster-agreed resume round; RULE-5/#468) (FEAT-6/ENT-14; #390/#391)
    gbdt_score.hpp          zetta::ml — batch scoring / rank-at-risk: score_gbdt/predict_risk (calibrated risk = predict_proba, ranked desc) + load_gbdt_model (weights body read back through the GbdtModel JSON Schema boundary) (FEAT-6/CMD-10; #392)
    gbdt_score_job.hpp      zetta::ml::run_score_job — run distributed batch scoring as a recorded `score` ComputeJob through the job coordinator: SPMD per-shard predict, gather + device sort_by_key rank + broadcast → identical global at-risk list on every node (FEAT-6/CMD-10; #392)
    calibration.hpp         zetta::ml — probability calibration: Calibrator (Platt sigmoid(a·raw+b) / isotonic PAVA step) + fit_calibrator + Brier/ECE metrics + JSON/wire boundaries; attaches to a GbdtModel (predict_proba applies it) via calibrate_model (FEAT-6; #391)
    tree_shap.hpp           zetta::ml — exact path-dependent TreeSHAP over a GbdtModel: tree_shap (per-feature φ + cover-weighted base_value, raw/margin space — efficiency base+Σφ=predict_raw), dominant_driver; pure, uses TreeNode::cover, no new JSON boundary (FEAT-6; #393)
    gbdt_explain_job.hpp    zetta::ml::run_explain_job — run distributed TreeSHAP as a recorded `explain` ComputeJob: per-entity SHAP over a shard (parallel_for) + an allreduced global mean-|φ| driver ranking; single-node == multi-node (FEAT-6; #393)
    golden_oracle.hpp       zetta::ml::GoldenOracle/load_golden_oracle — the committed golden featurize/train/SHAP expectations format (validated boundary), for the native correctness oracle (RSK-8; #395)
    playbook.hpp            zetta::ml — UC-1 retention playbook: generate_playbook (deterministic driver→action→owner via PlaybookPolicy + frozen expected-retained-value priority risk×ARR×save_rate, ranked) behind a constrained PlaybookNarrator seam (default-Claude ConstrainedNarrator over an injected NarratorCompletion, validated at the playbook_entry schema boundary, never invents numbers; RulesEngineNarrator offline fallback) + OutcomeLog (FEAT-6; #397)
  testkit/                  zetta::testkit — the in-app test-suite catalog (headless-runnable; #462)
    test_suite.hpp          TestCase/TestOption/TestOptions/TestResult/TestSuite + run_test_case + builtin() — the catalog model the GUI renders
    harness.hpp             ScratchDir + LoopbackCluster (a real N-node loopback cluster) + the in-code synthetic churn dataset + numeric encoding
    example_script.hpp      zetta::testkit::ExampleScript/ExampleExpectations/ExampleRunReport + run_example_script + example_scripts() — the sim-Python example-script harness: run a script end to end (parse->IR->gate->run) + assert on outputs, surfacing the offending statement on failure (the example programs ARE the integration tests; #523)
  repl/
    console_model.hpp       zetta::repl::ConsoleModel — the engine-agnostic REPL session (frontend + input + transcript + submit) behind the CMP-6 console; no SDL/ImGui, so it backs the GUI component AND a future CLI (#524)
    governed_console.hpp    zetta::repl::GovernedConsole — the governed run handler behind the SCR-8 Console/REPL screen: capability-scope a statement, lower sim-Python to the IR + run the correctness gate, execute (SQL via QueryService / sim-Python via run_graph), record one data_access audit event; zetta-core / no-SDL (#525)
src/core/                   zetta-core impl
  catalog.hpp               flat-key scheme for the replicated catalog (internal)
  node.cpp                  load_databases()/load_projects()/load_deployments()/load_runs()/load_jobs()/load_dataset_partitions() + replication capture/reconcile/persist
  database.cpp
  project.cpp               zetta::Project — persist/rename project.json + input/graph CRUD
  project_input.cpp         zetta::ProjectInput — persist input.json + payload body
  project_graph.cpp         zetta::ProjectGraph — persist a graph's json projection
  churn_dataset.cpp         load_churn_dataset — JSONL/CSV churn fixture reader + 3 embedded draft-07 schemas (boundary validation)
  n8n_import.cpp            zetta::import_n8n_workflow — n8n workflow JSON Schema + validate + store as artifact input
  n8n_compile.cpp           zetta::compile_n8n_workflow — n8n node-type→StepKind table + connections→edges → ExecutionGraph IR
  input/
    python_frontend.cpp     zetta::input::PythonFrontend — non-throwing tokenizer + recursive-descent parser (Python subset) + AST→IR lowering + the engine-faithful python_step_kind verb table (#520/#522)
    cluster_bindings.cpp    zetta::input::run_add_nodes_step/add_nodes_count — read the count from a lowered cluster.add_nodes IR node + drive zetta::add_nodes (#522)
    datasets_bindings.cpp   zetta::input::run_load_dataset_step/DatasetStepExecutor — read the source + column projection from a lowered load_dataset IR node, resolve bytes via the DatasetResolver seam, decode with the native parquet reader, project, and populate a (partitioned) table (#521)
  deployment_target.cpp     zetta::DeploymentTarget — names + JSON (boundary-validated)
  deployment.cpp            zetta::Deployment — persist deployment.json
  deployment_controller.cpp zetta::DeploymentController — reconcile pass: start/self-heal a deployment's run
  node_capability.cpp       zetta::NodeCapability — GpuKind names, satisfies(), JSON boundary validation (ENT-32)
  provisioner.cpp           zetta::ProvisionedNode — JSON boundary validation for the Provisioner DTO
  add_nodes.cpp             zetta::add_nodes — permanent-add control action over the Provisioner seam (provision + report, no teardown) + AddNodesRequest JSON boundary (#518); the Node& overloads drive rebalance_partitions onto the new members (#519)
  local_provisioner.cpp     zetta::LocalProvisioner — given-hardware backend: best-effort select over the live cluster fleet, no-op teardown (#374)
  azure_provisioner.cpp     zetta::AzureProvisioner — burst lifecycle (provision+mint / teardown+revoke) over the AzureControlPlane seam + AzureConfig JSON boundary (#376)
  azure_cli_control_plane.cpp zetta::AzCliControlPlane — `az vmss` argv construction + JSON parsing at the ProvisionedNode boundary (#376)
  command_runner.cpp        zetta::default_command_runner — the shared fork/execvp/pipe exec runner (argv, no shell; #376)
  aws_provisioner.cpp       zetta::AwsProvisioner — burst lifecycle (provision+mint / teardown+revoke) over the AwsControlPlane seam + AwsConfig JSON boundary (#377)
  aws_cli_control_plane.cpp zetta::AwsCliControlPlane — `aws autoscaling`/`aws ec2` argv construction + JSON parsing at the ProvisionedNode boundary (#377)
  gcp_provisioner.cpp       zetta::GcpProvisioner — GCP burst lifecycle (provision+mint / teardown+revoke) over the GcpControlPlane seam + GcpConfig JSON boundary (#378)
  gcp_cli_control_plane.cpp zetta::GcpCliControlPlane — `gcloud` MIG argv construction (template/create/resize/list/delete-instances) + JSON parse at the ProvisionedNode boundary (#378)
  oci_provisioner.cpp       zetta::OciProvisioner — OCI burst lifecycle (provision+mint / teardown+revoke) over the OciControlPlane seam + OciConfig JSON boundary (#432)
  oci_cli_control_plane.cpp zetta::OciCliControlPlane — `oci` Instance Pool argv (resolve-by-name/instance-config/create-or-resize/list-instances/detach) + JSON parse at the ProvisionedNode boundary (#432)
  k8s_provisioner.cpp       zetta::K8sProvisioner — Kubernetes burst lifecycle (provision+mint / teardown+revoke) over the K8sControlPlane seam + K8sConfig JSON boundary (#434)
  k8s_cli_control_plane.cpp zetta::K8sCliControlPlane — `kubectl` Deployment argv (create/set env+sa/patch spot/scale/get pods/delete pod) + JSON parse at the ProvisionedNode boundary (#434)
  bare_metal_provisioner.cpp zetta::BareMetalProvisioner — bare-metal burst lifecycle (provision+mint / teardown+revoke) over the BareMetalControlPlane seam + BareMetalConfig JSON boundary (#436)
  bare_metal_cli_control_plane.cpp zetta::BareMetalCliControlPlane — docker/podman argv (ps --filter label/run -d shortfall/rm -f) + NDJSON parse at the ProvisionedNode boundary (#436)
  burst_identity.cpp        zetta::BurstIdentityManager — bulk mint/revoke burst identities over the Node's principal CRUD + audit sink (#375)
  scaling_policy.cpp        zetta::ScalingPolicy — objective names, evaluate() per objective + cost cap, JSON boundary validation (ADR-29/ENT-34)
  burst_approval_gate.cpp   zetta::BurstApprovalGate — cost-cap gate logic: estimate, default cost, audit the decision, BurstCap JSON boundary (APR-3)
  job_autoscaler.cpp        zetta::JobAutoscaler — the tick() control loop (observe → evaluate → hysteresis → provision/teardown) + AutoscalerConfig JSON boundary (ADR-29/ARC-11)
  solution_catalog.cpp      zetta::SolutionCatalog — built-in library + schema/IR boundary parse
  cloud_provider.cpp        zetta::CloudProviderCatalog — built-in provider metadata (local/bare-metal/k8s/aws/gcp/azure/oci); no cloud access
  cloud_inspector.cpp       zetta::CloudInspector — run a provider's read-only commands, per-vendor JSON parse + boundary validation → CloudInspectReport, report JSON boundary (#445)
  run.cpp                   zetta::Run — value, mutators, JSON-Schema boundary validation (parse_run_value)
  model.cpp                 zetta::Model — value, mutators, weights body I/O, JSON-Schema boundary validation (parse_model_value)
  index/
    index.cpp               zetta::Index — IndexValue to_json/parse + Index JSON Schema (pboettch) + persist/apply (parse_index_value) (ENT-39/ENT-40)
    index_registry.cpp      zetta::IndexRegistry resolve + IndexSpec::key() + index_spec_from_node (IR `index` step → IndexSpec boundary)
    index_step_executor.cpp zetta::IndexStepExecutor — run an IR `index` step: resolve backend (node registry) → read table rows into partitions → build → record Index entity → `index` output; + read_index_partitions (shared) (#528)
    in_memory_index_backend.cpp zetta::InMemoryIndexBackend — inverted-index build over partitions / query / idempotent drop (the Search reference backend)
    oramacore_fulltext_index.cpp zetta::OramaCoreFulltextIndex — full-text BM25F build over partitions / ranked query (now carrying per-hit scores) / drop via the oramacore-cpp MemoryIndex (pImpl); ZETTA_WITH_ORAMACORE-guarded engine-absent stub (#527)
    distributed_fulltext.cpp zetta::DistributedFulltextIndex — per-partition build from owned partitions, scatter-gather query over the fulltext.query RPC, merge_fulltext_hits + the request/response JSON-Schema boundary (#529)
  compute_job.cpp           zetta::ComputeJob — value, set_status (+ finish audit), JSON-Schema boundary validation (parse_compute_job_value)
  dataset_partition.cpp     zetta::DatasetPartition — value, apply_value, JSON-Schema boundary validation (parse_dataset_partition_value)
  rendezvous_placement.cpp  zetta::RendezvousPlacement/rendezvous_owners — rendezvous (HRW) partition→node placement: FNV-1a weight, top-k by weight, ~1/N moved on membership change (#514)
  partition_writer.cpp      zetta::write_partitioned — the partitioned write path: bucket rows by assign_partition (stamping the round-robin surrogate), serialize each non-empty partition to JSON Lines, store via LargeObjectStore::import_bytes, record a DatasetPartition per partition (cumulative row range + place_partition owners) (#511)
  partition_rebalance.cpp   zetta::rebalance_partitions — between-jobs rebalance: per placed partition recompute rendezvous_owners(stats.partition, members, rf=owner count) and set_owner_nodes on a change; skip replicate-to-all (vs previous_members); the Node::tick auto-trigger + Node::cluster_members live in node.cpp (#517); the add_nodes direct trigger lives in add_nodes.cpp (#519)
  principal.cpp             zetta::Principal — persist/rename principal.json + key CRUD
  access_key.cpp            zetta::AccessKey — persist key.json + authorize() decisions (resource + operation)
  scope.cpp                 zetta::Scope — permits() + JSON boundary validation
  capability.cpp            zetta::CapabilitySet — permits()/TTL + JSON boundary validation (ENT-22)
  audit_event.cpp           zetta::AuditEvent — value, JSON form, JSON-Schema boundary validation
  intent_compiler.cpp       compile_intent — templated churn/generic skeleton (NL intent → IR)
  executor.cpp              GraphExecutor + topological_order + DefaultStepExecutor (graph → Run)
  churn_step_executor.cpp   ChurnStepExecutor — the per-kind real churn compute, each heavy step a ComputeJob (composes featurize/train/score/explain); index step also builds a real entity index via the node registry (#396/#528)
  churn_solution.cpp        run_churn_solution — the ChurnDataset→numeric encoder + standard churn pipeline config + the runner (inject ChurnStepExecutor into a GraphExecutor) + demo_churn_dataset (#398)
  table.cpp
  partitioner.cpp          zetta::HashPartitioner/RoundRobinPartitioner assign_partition (FNV-1a key hash / surrogate mod) + to_json + partitioner_from_json config boundary (#513)
  jsonl_store.cpp
  uuid.cpp
  sha256.cpp                self-contained SHA-256 (no crypto dependency)
  timestamp.cpp             zetta::now_iso8601() — UTC ISO-8601 stamp
  validation.cpp            ValidationReport verdict + JSON round-trip/validation
  gate.cpp                  CorrectnessGate runner + NonEmptyGraphCheck + make_check
  graph_checks.cpp          SchemaTypeCheck + DataLeakageCheck (#309) + annotation JSON-Schema validation
  checks/
    entity_resolution_check.cpp  EntityResolutionCheck — conflation/fragmentation + schema boundary
  entity_resolution.cpp     EntityResolver — normalized full-name / brand matching + entity_resolution_annotation builder (FEAT-6/TC-4; #394)
  replicated_blob_store.cpp large-object body transfer (CRDT metadata + disk)
  parquet/thrift_compact.hpp zetta::parquet::detail::CompactReader — the shared, non-throwing Thrift compact-protocol cursor (used by both the footer and page-header parsers; internal header)
  parquet/metadata.cpp      zetta::parquet — Thrift compact-protocol decoder + FileMetaData footer decode + to_json + parquet_metadata_schema/validate boundary (zetta-core; #505)
  parquet/snappy.cpp        zetta::parquet::snappy_decompress — Snappy raw-block decoder (preamble varint + literal/copy elements, bounds-checked) for Parquet data pages (#507)
  parquet/column_decoder.cpp zetta::parquet — page-header parse + RLE/bit-packed-hybrid + PLAIN/dictionary value decode + definition levels → DecodedColumn (dense values + validity); parquet_columns_schema/validate boundary (zetta-core; #506)
  parquet/reader.cpp        zetta::parquet::read_parquet_rows — decode every leaf column + transpose to one JSON object per row (the untyped whole-file reader; powers load_churn_dataset Parquet) (zetta-core; #508)
  ir/execution_graph.cpp    ExecutionGraph IR build + traversal
  ir/serialization.cpp      ExecutionGraph IR ↔ JSON (+ boundary validation)
  compute/device_buffer.cpp DeviceBuffer lifetime (free via device) + ComputeDevice::make_buffer (→ zetta-compute lib)
  compute/host_stub_device.cpp  HostStubDevice — host-memory allocate/transfer; operators unsupported (→ zetta-compute)
  compute/cuda_device.cu    CudaDevice — CUDA runtime memory model + Thrust/custom operator kernels, CpuDevice-conformant (→ zetta-compute; CUDA build only, #363/#361)
  compute/cpu_device.cpp    CpuDevice — every operator on host memory, multi-threaded via runtime::parallel_for (zetta-core; #360)
  compute/device_registry.cpp  DeviceRegistry — enumerate/report/select devices + DeviceInfo JSON boundary (zetta-core; #362)
  compute/work_stealing_scheduler.cpp  WorkStealingScheduler — worker threads, per-worker deques + the steal loop (zetta-core; #366)
  compute/distributed_scheduler.cpp  DistributedScheduler — the cross-node steal protocol + its JSON-Schema boundary + elastic membership re-queue (zetta-core; #366/#367)
  compute/job_coordinator.cpp  JobCoordinator — lifecycle driving (Running→Succeeded|Failed) + the shared signal-handler chain over Collectives + DistributedScheduler (zetta-core; #372)
  compute/checkpoint.cpp  JobCheckpoint value + JSON-Schema + sha256 integrity boundary, FileCheckpointStore (atomic save / latest / clear / job_ids), run_resumable_rounds (resume + checkpoint + graceful drain) (zetta-core; #468)
  ddf/data_frame.cpp        DataFrame loader (table→typed columns, chunked, device-backed), schema inference + DataFrameSchema JSON boundary (zetta-core; #380)
  ddf/parquet_reader.cpp    load_parquet_data_frame (decode requested leaf columns → DeviceBuffers, column projection + numeric/dense boundary), infer_parquet_schema, parquet_data_frame_schema/validate boundary (zetta-core; #508)
  ddf/ops.cpp               DDF scan/filter/project ops over a DataFrame (gather-based filter + repack, map/elementwise computed columns) + FilterPredicate/Projection JSON boundaries (zetta-core; #381)
  ddf/join.cpp              distributed_join + to_host_relation + hash/sort-merge join + collective repartition (gather/broadcast all-to-all) + deserialize boundary check (zetta-core; #382)
  ddf/data_frame_ops.cpp    zetta::ddf::sort/concat/slice + the internal HostFrame host-staging (data_frame_internal.hpp) shared with the distributed sort (zetta-core; #385)
  ddf/distributed_sort.cpp  zetta::ddf::DistributedSorter — sample-sort across the cluster + the ddf.shuffle all-to-all + PartitionMessage JSON-Schema boundary (zetta-core; #385)
  ddf/group_by.cpp          group_by/local_partial/GroupByPartial — groupby-aggregate accumulate/merge/finalize, the partial wire form + GroupBySpec JSON boundary (zetta-core; #383)
  ddf/as_of.cpp             as_of_aggregate — host-side per-entity windowed/point-in-time reduction (group key + time mask) + AsOfAggregate JSON boundary (zetta-core; #384)
  ddf/group_by_job.cpp      run_group_by_job — drive the distributed groupby-aggregate as a recorded ddf ComputeJob through the coordinator (calls distributed_group_by on the JobRound) (zetta-core; #387)
  ddf/local_partitions.cpp  load_local_partitions — enumerate this node's owned partitions from Node::dataset_partitions(), read each owned blob body from the local LargeObjectStore, parse the JsonlStore lines, load into a DataFrame; instrumented bytes_read (zetta-core; #516)
  ddf/local_group_by_job.cpp run_near_data_group_by_job — load the local shard via load_local_partitions then drive run_group_by_job, computing wire_bytes_moved from Collectives::wire_bytes() before/after (zetta-core; #516)
  ddf/featurize.cpp         point_in_time_featurize — the single-node featurization (as-of + group-by + filter + hash-join composition), FeatureTable::to_data_frame + FeaturizeSpec JSON boundary (zetta-core; #388)
  ddf/featurize_job.cpp     run_featurize_job — drive the distributed featurization as a recorded `featurize` ComputeJob (distributed_join enrich → per-feature filter + distributed_group_by; static mode skips the join and carries account_features over the time-filtered shard) through the coordinator (zetta-core; #388/#532)
  ddf/local_featurize_job.cpp run_near_data_featurize_job — load the local shard via load_local_partitions then drive run_featurize_job, reporting local_bytes_read vs wire_bytes_moved (zetta-core; #532)
  ml/gbdt.cpp               zetta::ml — histogram GBDT trainer (binning + split-finding + boosting loop), the binary-logistic model + GbdtParams/GbdtMetrics JSON boundaries (gbdt_model via pboettch) (zetta-core; #390)
  ml/gbdt_train_job.cpp     run_train_job — read the device-backed feature frame, drive the boosting loop with the JobRound's allreduce, compute global metrics (gather/broadcast), register the Model on the root; opt-in held-out calibration (content-hash split → gather/fit/broadcast); opt-in checkpoint/restart (resume the partial model, allreduce-min the cluster resume round, checkpoint each round via TrainHooks; #468) (zetta-core; #390/#391)
  ml/gbdt_score.cpp         zetta::ml — pure scorer (align columns to the model's feature order, predict_proba, rank desc + top_k) + load_gbdt_model (weights body → GbdtModel JSON Schema boundary) (zetta-core; #392)
  ml/gbdt_score_job.cpp     run_score_job — read device-backed columns, predict the shard, gather (entity,risk) to root, rank on the device (sort_by_key), broadcast the global at-risk list (zetta-core; #392)
  ml/feature_columns.hpp    zetta::ml::detail::read_column_as_double — shared device-download helper (frame column → host doubles, dtype-widening) used by the train + score + explain job bodies (zetta-core internal; #392/#393)
  ml/calibration.cpp        zetta::ml — Platt (robust Newton, Lin et al. 2007) + isotonic (PAVA) fit, Calibrator apply/JSON/wire boundary, Brier/ECE metrics (zetta-core; #391)
  ml/tree_shap.cpp          zetta::ml — exact TreeSHAP recursion (extend/unwind/unwound_path_sum), model_base_value (cover-weighted expectation), dominant_driver (zetta-core; #393)
  ml/gbdt_explain_job.cpp   run_explain_job — read the device-backed feature shard, per-entity tree_shap (parallel_for), allreduce the global mean-|φ| driver ranking, drive the `explain` job lifecycle (zetta-core; #393)
  ml/golden_oracle.cpp      load_golden_oracle — parse + schema-validate (pboettch draft-07) the committed golden oracle fixture at the boundary + structural rules, GoldenOracle::to_json (zetta-core; #395)
  ml/playbook.cpp           zetta::ml — playbook engine: PlaybookPolicy churn dictionary + hand-written JSON boundary, expected-retained-value priority + ranking, RulesEngineNarrator + ConstrainedNarrator (pboettch playbook_entry schema validation + semantic checks + fallback), generate_playbook, OutcomeLog (zetta-core; #397)
  runtime.cpp               zetta::runtime::parallel_for / parallel_chunks — data-parallel fan-out over the shared Capy thread pool
  cluster.cpp               cluster networking adapter (the one Corosio user) + signal-plane transport
  signal.cpp                zetta::Signal — envelope JSON form + JSON-Schema boundary (encode/decode)
  collectives.cpp           zetta::Collectives — collective rounds (star reduce/broadcast) + message JSON-Schema boundary
  base64.hpp                shared base64 (gossip blobs + signal payloads; internal)
  replication.cpp           replication CRDT adapter (the one automerge-cpp user)
  replication_strategy.cpp  zetta::ReplicationStrategy — kind names, JSON boundary validation, replication_strategy_from_meta reader (ENT-35; #509)
  headless.cpp              signal-driven headless loop
  signals.cpp               SIGINT/SIGTERM stop flag (OS signals, not the signal plane)
  testkit/                  zetta::testkit — the in-app test-suite catalog (no SDL; #462)
    test_suite.cpp          TestCase/TestOption/TestOptions/TestResult/TestSuite + run_test_case + builtin()
    harness.cpp             ScratchDir + LoopbackCluster (real N-node loopback) + sample_churn_dataset + numeric encoding
    cases_*.cpp             the canned cases per category (compute/ddf/ml/distributed/governance/pipeline/examples)
    example_script.cpp      run_example_script — parse->IR->gate->run + expectation checks + offending-statement reconstruction (#523)
    cases_examples.cpp      example_scripts() built-in catalog + register_example_cases (each sim-Python example a TestCase under TestCategory::Examples)
  repl/console_model.cpp    zetta::repl::ConsoleModel — submit/clear logic + Frontend names + format_result/format_transcript text projections (CMP-6; #524)
  repl/governed_console.cpp zetta::repl::GovernedConsole — the governance pipeline (capability check → sim-Python lower+gate → execute → data_access audit) + required_operation + the SQL Outcome→Result grid mapping (SCR-8; #525)
src/gui/                    headed UI (links SDL3 + ImGui)
  app.hpp                   zetta::gui::App — the one class declaring every panel method (forward-declares SDL/ImGui types)
  app.cpp                   lifecycle: ctor/dtor, init/run/render_frame, apply_display_scale, the menu bar + view switcher, file ingestion, the g_mono_font definition + the two load-bearing rendering bits
  app_internal.hpp          shared GUI-only helpers: the Orama design tokens + wash/status_dot/eyebrow/mono_line/primary_button + extern g_mono_font (not in the public include/zetta surface)
  app_home.cpp / app_explorer.cpp / app_pipeline.cpp / app_operations.cpp / app_governance.cpp
                            the App::draw_* panels split by area (home/data, pipeline, operations, governance) — each an App member TU over app.hpp + app_internal.hpp (#464)
  repl_console.hpp/.cpp     zetta::gui::ReplConsole — the thin Dear ImGui renderer over repl::ConsoleModel: frontend selector, multiline editor, run/clear, scrollback transcript, result grid (CMP-6; #524). draw_console (in app_explorer.cpp) hosts it over the live node
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

The fonts — **Geist** (UI sans) + **Geist Mono** (labels / counts / metrics /
code) — are embedded from `assets/fonts/` via `cmake/embed_font.cmake` into
`namespace zetta` (symbols `kGeistSans` / `kGeistMono`), so the binary has no
runtime asset dependency. `App::mono_font_` holds the mono face for `PushFont`.

The GUI follows the **Orama design system** (light surfaces, ink text, Geist
typography, coral used surgically, semantic green/amber/red for status, sentence
case, no em-dashes). It is **not** the old dark theme — `init()` calls
`StyleColorsLight()` then `apply_orama_theme()`, which paints the exact tokens.
The rules + token table + component grammar live in
[docs/design-system.md](docs/design-system.md); **read it before touching the
GUI.** The headed app opens on a project-first **Home** landing (`App::draw_home`
— hero + New project + your projects), with the other views reached from the menu
bar.

## Known gaps / behavior notes

- **Headed mode doesn't quit on terminal Ctrl-C** — SDL3 on macOS doesn't turn
  SIGINT into a quit event here, so close the window or use Cmd+Q. Only headless
  mode installs SIGINT/SIGTERM handlers. (Wiring a handler into headed mode is a
  reasonable future tweak if it becomes annoying.)

## Conventions

### Cloud providers — permission-gated (HARD RULE, all UI work)

**Any UI that touches a cloud provider is permission-gated. No exceptions, ever.**
This is a non-negotiable safety rule for *all* UI work (the SDL3 + ImGui app, any
future control plane) — and it binds both the **product** and **Claude when
building it**.

- **Never read a cloud provider's config or credential files without asking for
  explicit permission first** — `~/.aws/config`, `~/.aws/credentials`,
  `~/.kube/config`, `~/.config/gcloud/**`, `~/.azure/**`, credential env vars
  (`AWS_*`, `GOOGLE_APPLICATION_CREDENTIALS`, `AZURE_*`), the cloud
  instance-metadata endpoint (`169.254.169.254`), etc.
- **Never read from or write to a cloud provider without asking for explicit
  permission first** — any API / SDK / CLI call to AWS, Azure, GCP, Kubernetes,
  or any other provider (`aws`, `gcloud`, `az`, `kubectl`, their SDKs, raw REST).
  Writes (deploy, create, delete) are especially sensitive — confirm intent.
- **Ask every time.** Consent is per-action, deny-by-default, and *not* remembered
  across actions unless the user explicitly says so. The safe/default state reads
  nothing and calls nothing.
- **In the product:** gate the action behind an explicit, in-UI consent step that
  names exactly what will be read or called (which files, which API, read vs.
  write) *before* anything happens. No silent background reads of cloud state.
- **For Claude:** before writing UI code that reads cloud creds/config or calls a
  cloud, ask the developer first. When in doubt, surface what *would* be
  read/called and ask — never touch the cloud to "just check."

### AI compute — GPU-native, CPU fallback, distributed (DIRECTION)

**All AI/ML compute should be GPU-native with a CPU fallback, and distributed.**
The target for every heavy AI op (featurize / train / score / explain) is: it runs
on the accelerator when one is present, falls back to the CPU when it is not, and
scales across the cluster — there is no CPU-only AI path that a GPU build leaves on
the table. The mechanism is already in the architecture: route the op **through the
`ComputeDevice` seam** (so it executes on whatever device the data lives on — GPU on
a CUDA frame, CPU otherwise) and through the [job coordinator](docs/job-coordinator.md)
collectives (so it is SPMD across nodes). New hardware = a new backend, nothing else.

> **Current reality vs. the rule.** Today the ML *numerics* are **host-side
> references over device-backed data** — the [GBDT trainer](docs/gbdt.md) and the
> [scorer](docs/gbdt-score.md) read their columns back through the device download
> seam and compute host-side, and the device-native (GPU) kernels are the documented
> optimization *behind the same API*. The data already lives on the device, the
> distribution is real (allreduce / gather / broadcast), and parts are already
> device-native (e.g. scoring ranks the at-risk list with the device `sort_by_key`
> operator — GPU when present, CPU fallback). Move things *toward* this rule (push
> the numerics onto device kernels), never away (never add a new AI op that bypasses
> the `ComputeDevice` seam).

### Design system — follow it for all GUI work

The headed app follows the **Orama design system** (light surfaces, ink text,
**Geist + Geist Mono**, coral used *surgically* as the "you are here" accent,
semantic green/amber/red for status — never coral/blue, **sentence case**, **no
em-dashes** in product copy — use `·`/colons). The tokens, typography/copy rules,
coral discipline, and component grammar (cards, ghost buttons, charcoal primary
actions, status dots, mono metadata strips, tinted callouts) — and how they map
to `src/gui/app.cpp` (`apply_orama_theme`, `mono_font_`, the `eyebrow` /
`status_dot` / `wash` helpers) — are documented in
[`docs/design-system.md`](docs/design-system.md). **Read it before adding or
changing GUI**, and run its self-check before shipping. Canonical source: the
`orama-work` repo's `orama_design_review/design-system` + the Orama Zetta
marketing site.

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

### System tables — surface observable state as `system.<table>` by default
**Any state the product exposes for inspection or observability defaults to a
virtual, read-only table in the reserved `system` database** — the cluster/node
list, deployments, models, compute jobs, **dataset partitions**, and
**cloud/provider state** (the Cloud Inspector, provisioned bursts, …) all belong
as `system.<table>` tables (à la `pg_catalog`), the way `system.dataset_partitions`
does. **Name system tables plainly — no `zetta_` prefix.** The `system` database is
reserved and zetta-controlled, so a `zetta_` prefix is **redundant** (`system.jobs`,
not `system.zetta_jobs`). The legacy `zetta_*` tables (`zetta_nodes`, `zetta_jobs`,
…) predate this rule and are kept until the system-wide rename in
[`#538`](https://github.com/oramasearch/zetta/issues/538) — new system tables follow
the unprefixed convention now (`system.dataset_partitions` is the first). A `system`
table is **computed at query time from the live in-memory source** (`Node::…()` / a
catalog), **never** a stored or hand-synced copy. Reach for a bespoke GUI-only panel
or an ad-hoc structure only when a `system` table genuinely can't express the state —
and even then, prefer a table plus a panel that renders it, so SQL and the GUI never
drift.
This **composes with "EVERYTHING is replicated state"** (above), it doesn't
contradict it: durable agreement still lives in the CRDT, and the `system` table
is its read view; **ephemeral/observed** state (a live cloud inspect, the device
registry) is a read view with **no** CRDT backing. Cloud-derived `system` tables
stay **permission-gated** (the hard rule above) — reading one must not trigger a
real config-file read or cloud call without explicit, per-action consent.

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

### Idle loop + live resize
The headed loop uses `SDL_WaitEventTimeout` (≈30fps idle cap) rather than a busy
vsync spin — it wakes immediately on input so interaction stays full-speed but
doesn't burn CPU on identical frames. The headless loop ticks at 10 Hz. Keep any
background work cheap enough to run inside those cadences (or move it off the
loop thread). One frame's worth of build+present is factored into
`App::render_frame()`; an `SDL_AddEventWatch` (`App::on_sdl_event`) calls it on
`WINDOW_RESIZED`/`PIXEL_SIZE_CHANGED`/`EXPOSED` so the UI tracks a live resize
drag instead of the OS stretching the last frame (macOS runs a modal runloop
during resize that starves the normal loop). `render_frame()` has a re-entrancy
guard; it does **not** tick the node (that stays in `run()`).

### Font + text size
The GUI font size is `kBaseFontPixels × OS-content-scale × user-text-size`. The
user multiplier is **View → Text size** (`ui_scale_`); changing it sets
`font_rebuild_requested_`, and `run()` rebuilds the font atlas via
`apply_display_scale()` **between** frames (never mid-frame, while the atlas is in
use). The atlas also rebuilds on display-scale/DPI changes. Both Geist faces +
the whole style scale together, so text-size is effectively a UI zoom.

## Pointers

- [`docs/architecture.md`](docs/architecture.md) — node, run modes, data model, module map.
- [`docs/true-hybrid.md`](docs/true-hybrid.md) — **true hybrid**: one equal-peer cluster spanning heterogeneous hardware (CPU/GPU/ASIC, OS, architecture, on-prem + cloud), the seams that make it work (`ComputeDevice` backends, the device registry, the work-stealing scheduler, collectives, the cluster, the `Provisioner`), and the cost / flexibility / resilience payoff — with a built-vs-planned support matrix.
- [`docs/projects.md`](docs/projects.md) — the AI project (`zetta::Project`) data model + lifecycle CRUD.
- [`docs/project-inputs.md`](docs/project-inputs.md) — project inputs (`zetta::ProjectInput`): raw intent/artifact ingestion + provenance (replicated).
- [`docs/n8n-import.md`](docs/n8n-import.md) — importing an n8n workflow JSON as an artifact input (raw bytes + sha256, schema-validated at the boundary).
- [`docs/n8n-compile.md`](docs/n8n-compile.md) — compiling an imported n8n workflow JSON into the ExecutionGraph IR (`zetta::compile_n8n_workflow`: nodes→typed steps, connections→edges; FEAT-2 / #308).
- [`docs/parquet-metadata.md`](docs/parquet-metadata.md) — the native (no Arrow) Parquet footer / metadata parser (`zetta::parquet::parse_parquet_metadata`, ADR-32 / #481 / #505): read a `.parquet` file's Thrift `FileMetaData` — schema (column names/types), row groups, per-column-chunk offsets/sizes/encodings/codecs — via a self-contained Thrift compact-protocol decoder; no data decode (the layout foundation the #506 decoders / #507 Snappy / #508 DDF wiring consume), non-throwing + bounds-checked, with a schema-validated JSON projection tested against a dev-time pyarrow golden (no foreign runtime in build/CI).
- [`docs/parquet-snappy.md`](docs/parquet-snappy.md) — Parquet Snappy page decompression (`zetta::parquet::snappy_decompress`, #507): a self-contained native decoder for the Snappy raw-block form Parquet uses for its default-codec data pages — non-throwing, bounds-checked at the binary boundary, with hand-built canonical spec vectors + a page-shaped golden as the byte-identical acceptance. The first slice of the native Parquet reader (ADR-32); the footer parser (#505), column-chunk decoders (#506), and reader→DDF wiring (#508) build on it. zstd is a follow-up codec.
- [`docs/parquet-column-decoders.md`](docs/parquet-column-decoders.md) — the native (no Arrow) Parquet column-chunk decoders (`zetta::parquet::decode_column` / `decode_column_chunk`, ADR-32 / #506): read a column's data pages into typed values + a validity bitmap, on top of the [footer parser](docs/parquet-metadata.md) (#505) + [Snappy codec](docs/parquet-snappy.md) (#507). Decodes **PLAIN** (fixed-width LE int32/int64/float/double, bit-packed bool, length-prefixed byte_array) + **dictionary** (PLAIN dict page + RLE/bit-packed indices), and **definition levels** (RLE/bit-packed hybrid) into per-row validity for the (mostly non-null) columns; flat columns / page v1 / uncompressed + snappy codecs (zstd/gzip rejected with a reason until their decoder lands). A `DecodedColumn` is the Arrow-native dense values + validity bitmap the [#508 DDF projection](docs/parquet-reader.md) consumes; non-throwing + bounds-checked, schema-validated JSON projection tested against a dedicated pyarrow golden fixture. No DDF/SQL/run wiring (that is #508).
- [`docs/parquet-reader.md`](docs/parquet-reader.md) — the native (no Arrow) Parquet **reader → DDF columns with column projection** (`zetta::ddf::load_parquet_data_frame` + `zetta::parquet::read_parquet_rows`, ADR-32 / #508): the capstone that assembles the [footer parser](docs/parquet-metadata.md) (#505) + [column decoders](docs/parquet-column-decoders.md) (#506) + [Snappy codec](docs/parquet-snappy.md) (#507) into a reader that materializes a **requested subset** of columns straight into a typed, chunked [DDF `DataFrame`](docs/ddf.md) (DeviceBuffers). **Column projection** decodes only the selected column chunks, so it loads the engineered numeric columns + `outcome` of the saas-sales-conversations dataset while **dropping `embedding_0..3071`** (3072 columns) without decoding them; numeric/dense boundary (nulls / byte_array / real-into-int rejected), `infer_parquet_schema` for "load every number", schema-validated golden (`parquet_data_frame_schema`). The untyped sibling `read_parquet_rows` (decode every leaf → one JSON object per row) wires Parquet into the [churn loader](docs/churn-dataset.md) (`load_churn_dataset(..., Parquet)`), removing the `ChurnDataFormat::Parquet` "not yet supported" error — all three formats round-trip identically. TC-9 (#499) parity vs a dev-time pyarrow golden (no foreign runtime in build/CI); no deploy/`ddf`-kind wiring yet, zstd a follow-up codec.
- [`docs/new-project-screen.md`](docs/new-project-screen.md) — the headed New Project screen (SCR-2): name + paste NL intent + import/drag-drop an n8n workflow artifact, in one atomic create (FEAT-1).
- [`docs/runs.md`](docs/runs.md) — the Run (`zetta::Run`) record: status/logs/outputs/lineage + JSON-Schema validation.
- [`docs/models.md`](docs/models.md) — the Model (`zetta::Model`) entity (ENT-14/FEAT-6): replicated `model:<uuid>` metadata + serialized weights as a peer-to-peer large-object body (sha256-verified) + the `system.zetta_models` registry.
- [`docs/compute-jobs.md`](docs/compute-jobs.md) — the ComputeJob (`zetta::ComputeJob`) record: a unit of native distributed accelerated compute (featurize/train/explain/score/ddf) — lifecycle, create/finish audit, JSON-Schema boundary (ARC-11 / ENT-31).
- [`docs/dataset-partitions.md`](docs/dataset-partitions.md) — the partition catalog (`zetta::DatasetPartition`, ENT-36 / TBL-13): a replicate-to-all record of where a slice of a table's data lives (table / blob body / row range / stats / owner nodes) — the `part:<uuid>` CRDT key + reconcile + the read-only `system.dataset_partitions` computed view, so placement is globally visible; JSON-Schema validated at the boundary. Storage + provenance + replication of the catalog only (locality-aware reads / re-balancing are later slices). The producer that fills it is the partitioned write path below.
- [`docs/partitioned-write-path.md`](docs/partitioned-write-path.md) — the **partitioned write path** (`zetta::write_partitioned`, **#511**): rows → partition → blob, placement recorded. For a table configured with a [`Partitioner`](docs/partitioner.md) it assigns each row to a partition (stamping the round-robin `_seq` surrogate onto the assignment record only), serializes each non-empty partition's rows to a JSON Lines blob (the `JsonlStore` `{"_zid",doc}` shape), stores the bytes as a [large object](docs/large-objects.md) (`LargeObjectStore::import_bytes`, so the body is sha256-fingerprinted + gossips peer-to-peer), and records a [`DatasetPartition`](docs/dataset-partitions.md) catalog entry per partition (`blob_ref` + the cumulative `row_range` tiling `[0, rows_written)` + `place_partition` owners + `stats`), visible in `system.dataset_partitions`. N non-empty partitions → N blobs + N catalog rows (the acceptance). Composes #510/#512–#514; no new JSON schema (the catalog entry is the existing `DatasetPartition` boundary, the blob is an LO body). A reusable library function today; a load-triggering command and `ReplicationStrategy`-driven placement are later slices.
- [`docs/partition-rebalance.md`](docs/partition-rebalance.md) — the **between-jobs partition rebalance on a membership change** (`zetta::rebalance_partitions`, **#517**; satisfies TC-10/#500): when a node joins, recompute [rendezvous (HRW)](docs/partitioner.md) placement for the sharded partitions and update each changed [`DatasetPartition`](docs/dataset-partitions.md)'s `owner_nodes` (via the capturing `set_owner_nodes` mutator that captures `part:<id>` into the CRDT), so **≈rf/N partitions move onto the new node** (whole partitions, no full rehash) and every row stays readable exactly once with `rf` owners — the **bodies move for free** over the [sharded-blob-placement](docs/sharded-blob-placement.md) (#515) seam (the new owner `LOREQ`s the body, a non-owner refuses it). `rf` is preserved from the catalog's current owner count (not the declarative `ReplicationStrategy`, since any `Partitioner` could have placed it); **replicate-to-all** partitions (owned by exactly the previous membership) are left untouched. **Auto-triggered inside `Node::tick()`** off the replicated `node:<id>` membership keys (no `Cluster` plumbing); fires on **joins** (append-only `node:` keys). No new JSON format. Out of scope: locality reads (#516), physical GC of a demoted owner's stale body, mid-job/elastic rebalance.
- [`docs/sharded-blob-placement.md`](docs/sharded-blob-placement.md) — partition-aware large-object body replication (`zetta::ReplicatedBlobStore`, #515): a `Partitioned` / `PartitionedWithReplicas` table's partition body (the [catalog](docs/dataset-partitions.md) entry's `blob_ref`) is fetched + held **only by its owner node(s)** per `owner_nodes` + the replication factor (`rf` owners → exactly `rf` copies), instead of replicate-to-all — `missing_blobs()` skips a non-owner's partition body (so it never `LOREQ`s it), `store_blob()` refuses it; a plain large object, an unplaced partition (empty `owner_nodes`), and model weights keep replicate-to-all. Owner set read from the CRDT `part:` snapshot through the validated boundary (network-thread-safe, no Node access); no new JSON format. The first slice that *acts* on a non-default [replication strategy](docs/replication-strategy.md) at the body level (the partitioner write path / locality-aware reads / rebalancing are later slices).
- [`docs/index-backend.md`](docs/index-backend.md) — the **`IndexBackend` seam** (`zetta::IndexBackend`: build over a table's partitions / query / drop, no-throw `IndexStatus`) + the **`IndexRegistry`** (resolve an `IndexSpec` — or an IR `index` step via `index_spec_from_node` — to a registered backend whose name + kind match, like `compute::DeviceRegistry`) + the in-memory **`InMemoryIndexBackend`** Search reference backend, and the top-level replicated **`Index` entity** (`zetta::Index`/`IndexValue` `{table,kind,backend,fields}`, `index:<uuid>`, `pboettch`-validated, `system.zetta_indexes` registry). Vector/semantic/feature backends are additive behind the same seam; run/deploy wiring is a later slice (ENT-39 / ENT-40, ADR-34).
- [`docs/oramacore-fulltext-index.md`](docs/oramacore-fulltext-index.md) — the **`OramaCoreFulltextIndex`** backend (`zetta::OramaCoreFulltextIndex`, ENT-39 / ENT-40, ADR-34, #527): the **first real `IndexBackend`** — a **full-text (BM25F)** Search backend (name `oramacore_fulltext`, kind `Search`) over the native **oramacore-cpp** engine (the vendored C++23 OramaCore full-text port, `modules/oramacore-cpp`). `build` indexes each row's field text into an `oramacore::index::MemoryIndex`; `query` reads `term` as a free-text query and returns row ids **ranked** by BM25F relevance (tokenize + stem + stop-word filter both sides), capped by `top_k`. The engine is held behind a **pImpl** (no oramacore-cpp type in the header) + linked **PRIVATE**, and the whole integration is **guarded on the (private) submodule's presence** (`ZETTA_WITH_ORAMACORE`) so a CPU-only checkout still builds/runs green (the backend degrades to `Unsupported`; `engine_available()` reports which). No new JSON boundary (operates over already-validated specs + stored rows). Driving a deployed `index` step through it is the [index-step-executor](docs/index-step-executor.md) slice (#528); per-field BM25F params are a later slice.
- [`docs/index-step-executor.md`](docs/index-step-executor.md) — the **run-path wiring** for an `index` step (`zetta::IndexStepExecutor` + `Node::index_registry()`, **#528**): the `Node` now owns an `IndexRegistry` (the reference backends registered — `memory_search` always, `oramacore_fulltext` when `ZETTA_WITH_ORAMACORE`), and `IndexStepExecutor` is the [`StepExecutor`](docs/execution.md) that resolves a deployed IR `index` step to its backend ([`index_spec_from_node`](docs/index-backend.md)), reads the source table's rows into partitions (`read_index_partitions`, chunked like the storage engine), **builds a real index**, records the [`Index`](docs/index-backend.md) entity, and emits an `index` Run output referencing the real id — leaving the index **queryable** for the node's lifetime (`node.index_registry().resolve(spec)->query(...)`). Other step kinds delegate to `DefaultStepExecutor`; churn's `index` step also builds a real entity index over the accounts shard (alongside the at-risk ranking). The acceptance "an index step builds a real index that is then queryable"; the vector/semantic/feature backends remain later slices. No new JSON format.
- [`docs/distributed-fulltext.md`](docs/distributed-fulltext.md) — **per-partition distributed full-text** (`zetta::DistributedFulltextIndex`, ENT-39 / ENT-40, ADR-34, **#529**, the engine half of TC-13 #503): the **approved per-partition topology** — where the [index-step-executor](docs/index-step-executor.md) builds **one consolidated** index, this builds a full-text **sub-index per LOCAL owned partition** ([compute-near-data](docs/compute-near-data.md): enumerate owned partitions → read each body from local disk → build one sub-index per partition into an owned [`IndexBackend`](docs/index-backend.md)) and answers a query by **scatter-gather + merge/rank**: query the node's own sub-indexes, **scatter** to every peer over the `fulltext.query` [signal-plane](docs/signal-plane.md) RPC (no new wire verb; request/response schema-validated like the `CollectiveMessage`), **gather** each node's ranked hits, **merge** into one global ranking (identical on every node; an unreachable peer fails the round). The pure `merge_fulltext_hits` (score desc, row id asc, dedup-by-max, `top_k`) is the core both merges run through → single-node == multi-node. The acceptance: the distributed ranking **matches a single consolidated-index oracle** (the standard sharded-search local-IDF query-then-fetch — exact for the global top-k when ranking is tf-unambiguous; a global-stats `dfs_query_then_fetch` pass is a later optimization). The seam's `IndexQueryResult` now carries per-hit `scores` (a ranked backend fills them, exact-match leaves them empty). Tested engine-agnostically (a partition-independent reference scorer over a real loopback cluster) with the real [BM25F](docs/oramacore-fulltext-index.md) path skipped when the submodule is absent. A building block alongside the consolidated path; wiring it as the default `index`-step behaviour is a later slice.
- [`docs/access-control.md`](docs/access-control.md) — key-scoped access (V1): `Principal` + `AccessKey` + `Scope` (resources) + `Capability` (rights, TTL'd), deny-by-default authorization (RULE-3 / ENT-22).
- [`docs/audit-trail.md`](docs/audit-trail.md) — the immutable, append-only `AuditEvent` (ENT-23 / FEAT-9): graph change / deploy / data-access events, replicated + JSON-Schema validated.
- [`docs/admin-panel.md`](docs/admin-panel.md) — the headed **Admin & Settings panel** (`App::draw_admin`, SCR-7 / CMP-5): read-only view of principals + keys, scope + capability grants, and the audit log.
- [`docs/jobs-panel.md`](docs/jobs-panel.md) — the headed **Compute Jobs panel** (`App::draw_jobs`, FEAT-5 / SCR): live `system.zetta_jobs` (ComputeJobs + status) + per-job objective/scaling controls previewing `ScalingPolicy::evaluate()` (set the reward, add/remove nodes); pairs with the trained-Model + metrics + ranked drivers in the Runs panel.
- [`docs/storage.md`](docs/storage.md) — on-disk format + storage-engine seam.
- [`docs/partitioner.md`](docs/partitioner.md) — the `Partitioner` seam (`zetta::Partitioner`, ADR-31 / ENT-37, #512): the pluggable, per-table policy mapping a record to a partition and a partition to its owner node(s) over a fixed partition count `P >> nodes`, so rebalancing moves whole partitions (as blobs) rather than rehashing rows. The seam + the trivial `SinglePartitioner` baseline + `Table::set_partitioner` (#512); the two **record→partition** strategies (#513): `HashPartitioner` (`hash(key columns) % P` — deterministic shard-by-key via a stable FNV-1a, balanced for a typical key) and `RoundRobinPartitioner` (`surrogate % P` — even balance regardless of key skew), with the `partitioner_from_json`/`to_json` JSON config boundary (`schemas/partitioner.schema.json`, the hand-written value-type pattern); and the **partition→node** strategy `zetta::RendezvousPlacement` (`rendezvous_placement.hpp`/`rendezvous_owners`, #514) — rendezvous / highest-random-weight (HRW) placement (FNV-1a weight per `(partition, member)`, top-`replication_factor` by weight), so adding/removing one node reassigns only ~1/N of partitions, deterministic + leaderless + order-independent. Dependent slices: the partition catalog (#510) and the partitioned write path (#511, which composes assignment + placement and owns the universal polymorphic persisted-config JSON boundary).
- [`docs/query.md`](docs/query.md) — the dialect-agnostic query layer + Postgres subset, incl. comparison `WHERE` and **`GROUP BY` aggregation routed through the [DDF engine](docs/ddf.md)** (ARC-12 / API-2, #386).
- [`docs/repl-console.md`](docs/repl-console.md) — the **REPL console** component (CMP-6 / #524): the SQL / simulated-Python console behind the headed **Data → Console** view — `zetta::repl::ConsoleModel` (the engine-agnostic REPL session in zetta-core: frontend + input + transcript + submit; no SDL/ImGui, so the same class backs the GUI component AND a future CLI) + the thin `zetta::gui::ReplConsole` ImGui renderer (frontend selector, multiline editor, run/clear, scrollback transcript, result grid). The component emits **run events** via a host-supplied handler — SQL through the [query service](docs/query.md), simulated-Python lowered to the IR + gated + executed by the governed screen below; results + errors display inline. No new JSON boundary (ephemeral session state). The governed Console/REPL screen (SCR-8, #525) hosts it.
- [`docs/governed-console.md`](docs/governed-console.md) — the **governed Console/REPL screen** (SCR-8 / #525, realizes UC-3): the headed screen (`App::draw_console`) hosting the [REPL console](docs/repl-console.md), routing every statement through `zetta::repl::GovernedConsole` (zetta-core / no-SDL): **capability-scoped** against the acting key (`api:query` SQL / `orchestration:start_run` sim-Python; empty key = unauthenticated local, still audited), **simulated-Python lowered to the IR + run through the [correctness gate](docs/gate.md)** (RULE-1) then executed via `Node::run_graph` (a real Run), and **each statement recorded in the [audit trail](docs/audit-trail.md)** as a `data_access` event (reuses the existing audit action — no schema change). SQL keeps its own validated [query](docs/query.md) executor. Tabular results for both frontends; a "Run as" key selector; no new JSON boundary; headless-tested (`tests/test_governed_console.cpp`).
- [`docs/execution-graph.md`](docs/execution-graph.md) — the ExecutionGraph IR (nodes/edges/step-kinds).
- [`docs/intent-compiler.md`](docs/intent-compiler.md) — compiling a natural-language intent into a reviewable, editable ExecutionGraph skeleton (`zetta::compile_intent`; templated churn mapping, FEAT-2 / #311).
- [`docs/python-frontend.md`](docs/python-frontend.md) — the simulated-Python language frontend (`zetta::input::LanguageFrontend` interface + `zetta::input::PythonFrontend`, ENT-38 / ADR-33 / #520) **+ its cluster/ML engine bindings** (#522): parse a Python subset (assignments, function calls, attribute/method access + chaining, literals) and lower it to the [ExecutionGraph IR](docs/execution-graph.md) — calls → typed steps (via the engine-faithful `python_step_kind` verb table; unmapped verb → Clean), variable refs + method receivers → data-flow edges; **not** a Python runtime (re-implement, don't run); non-throwing; no new JSON format (output is the validated IR JSON form). The verbs bind to real engine ops — `train`→`model` ([GBDT](docs/gbdt.md)), `score`/`predict`/`rank`→`index` ([batch scoring](docs/gbdt-score.md)), `explain`→`serve` ([TreeSHAP](docs/tree-shap.md)) all run through the [`ChurnStepExecutor`](docs/churn-step-executor.md); `cluster.add_nodes(n, provider=)`→`act` runs through `zetta::input::run_add_nodes_step` → the permanent-add control action [`zetta::add_nodes`](docs/add-nodes.md) (cloud consent-gated: the `Provisioner&` is injected). The IR-targeting analogue of the query layer's `Dialect`.
- [`docs/datasets-shim.md`](docs/datasets-shim.md) — the simulated HuggingFace **`datasets` library shim** (`zetta::input::run_load_dataset_step` + `DatasetStepExecutor` + `dataset_load_spec` + `DatasetResolver`, #521): a sim-Python `from datasets import load_dataset; load_dataset("<path>", columns=[…])` lowers to an IR `connect`/load step (parquet path + column projection), and the binding decodes the referenced Parquet bytes with the native [Parquet reader](docs/parquet-reader.md) (`read_parquet_rows`, no Apache Arrow / parquet-cpp / HF runtime / network — fidelity references only), applies the projection, and populates a (optionally [partitioned](docs/partitioned-write-path.md)) table; the `DatasetStepExecutor` runs the load inside a [`GraphExecutor`](docs/execution.md). File/network IO behind the injectable `DatasetResolver` seam (offline default = a local filesystem path; a Hub-name resolver is a host-injected later slice); **no new JSON format** (the spec rides the validated IR boundary; rows are stored as free-form table rows). The engine binding of the [new-input](.claude/skills/new-input/SKILL.md) worked example.
- [`docs/validation.md`](docs/validation.md) — the `ValidationReport` correctness-gate result type (ENT-5/FEAT-3).
- [`docs/gate.md`](docs/gate.md) — the correctness-gate runner (`zetta::CorrectnessGate`): checks → report, block deploy on failure (RULE-1).
- [`docs/gate-checks.md`](docs/gate-checks.md) — the concrete graph checks (`schema_type`, `data_leakage`): schema/type vs. source profiles + data-leakage / point-in-time for ML steps (#309).
- [`docs/entity-resolution.md`](docs/entity-resolution.md) — entity resolution in featurization (`zetta::EntityResolver`, FEAT-6 / TC-4 / #394): resolve raw comms mentions to CRM accounts division-correct (FullName) — "FedEx Freight" without conflating its siblings — vs. the coarse Brand trap, emitting the `entity_resolution` annotation the gate verifies (resolver produces, gate verifies); no new JSON format.
- [`docs/entity-resolution-check.md`](docs/entity-resolution-check.md) — the entity-resolution correctness eval (`zetta::EntityResolutionCheck`, TC-4): fail the gate on account conflation (FedEx-Freight).
- [`docs/churn-dataset.md`](docs/churn-dataset.md) — the synthetic UC-1 churn dataset fixture + loader (`zetta::ChurnDataset` / `load_churn_dataset`, FEAT-6 / #402): committed accounts + comms/events (JSONL/CSV), the FedEx entity-resolution case, schema-validated, no foreign runtime.
- [`docs/deployments.md`](docs/deployments.md) — placing a graph onto a target (`DeploymentTarget`; R1 local), recorded + queryable.
- [`docs/deployment-controller.md`](docs/deployment-controller.md) — the control plane (`zetta::DeploymentController`): reconcile a local deployment desired→actual + self-heal a killed/failed run (FEAT-4/TC-3; R1 local).
- [`docs/execution.md`](docs/execution.md) — executing a graph into a Run (`zetta::GraphExecutor` / `Node::run_graph`, `StepExecutor` seam).
- [`docs/churn-step-executor.md`](docs/churn-step-executor.md) — the **real** churn StepExecutor (`zetta::ChurnStepExecutor`, #396): real native distributed accelerated compute behind the `StepExecutor` seam (orchestrator untouched) — connect→load to DDF, clean→featurize, model→train GBDT + calibrate (registers a real `Model`), index→build a real entity index ([#528](docs/index-step-executor.md)) + score/rank, serve→explain (TreeSHAP), act→retention playbook, each heavy step a recorded `ComputeJob`; `run_graph` over the churn graph produces a Run whose outputs reference a real `Model` + ranked at-risk list + drivers (FEAT-5 / FEAT-6 / UC-1).
- [`docs/compute-device.md`](docs/compute-device.md) — the `zetta::compute::ComputeDevice` accelerator seam: typed device buffers + memory model + the operator set + the host-memory stub (ADR-27 / ARC-9; #359), plus the `zetta-compute` build target + the optional CUDA backend (`CudaDevice`, `-DZETTA_ENABLE_CUDA=ON`; #363).
- [`docs/cpu-device.md`](docs/cpu-device.md) — the `zetta::compute::CpuDevice` reference backend: every operator on host memory, multi-threaded via `runtime::parallel_for`, the numerical reference for CUDA (ADR-27 / ARC-9; #360).
- [`docs/device-registry.md`](docs/device-registry.md) — the per-node device registry (`zetta::compute::DeviceRegistry`): enumerate a node's compute devices, report each one's capability, select a backend per op (accelerator-first / CPU fallback), with the `DeviceInfo` JSON boundary (ARC-9; #362).
- [`docs/ddf.md`](docs/ddf.md) — the DDF DataFrame core (`zetta::ddf::DataFrame`): a typed, columnar, chunked frame over the storage engine, each chunk a `ComputeDevice` buffer; table→columns loader + schema inference + value/JSON boundaries — the substrate DDF ops operate on (ARC-12 / GLO-11 / ENT-33; #380).
- [`docs/ddf-ops.md`](docs/ddf-ops.md) — the local DDF DataFrame ops: `zetta::ddf::scan` / `filter` / `project` (read a table into a frame, `gather`-based row filtering, `map`/`elementwise` computed columns, with schema-validated `FilterPredicate` / `Projection` JSON boundaries; #381) and `zetta::ddf::sort` / `concat` / `slice` (key-column sort via the device `sort_by_key`, row-wise union, contiguous row range — the single-node building blocks of the distributed sort; #385) (ARC-12).
- [`docs/ddf-distributed-sort.md`](docs/ddf-distributed-sort.md) — the DDF distributed sort (`zetta::ddf::DistributedSorter`): sample-partition + local sort + merge across the cluster — splitter agreement over the [collectives](docs/collectives.md), an all-to-all row shuffle over the [signal plane](docs/signal-plane.md) (schema-validated `PartitionMessage`), returning globally range-ordered, locally-sorted partitions; a single-node cluster returns the locally-sorted shard (ARC-12; #385).
- [`docs/ddf-join.md`](docs/ddf-join.md) — the DDF distributed equi-join (`zetta::ddf::distributed_join`): a distributed DDF op — repartition two frames by key across the cluster via collectives (gather + broadcast all-to-all), then a local hash / sort-merge join per shard, so single-node and multi-node yield identical results; rides the schema-validated `CollectiveMessage` with a range-checked deserialize boundary, no new wire format (ARC-12; #382).
- [`docs/ddf-groupby.md`](docs/ddf-groupby.md) — the DDF distributed groupby-aggregate (`zetta::ddf::group_by` / `distributed_group_by`): groupby + count/sum/min/max/mean (an extensible op set) over a frame's columns, partial-per-node combined via gather/merge/broadcast over the collectives — results identical single- and multi-node; a DDF operator + schema-validated `GroupBySpec` boundary (ARC-12; #383) — and runs as a recorded `ddf` `ComputeJob` (`zetta::ddf::run_group_by_job`, `group_by_job.hpp`) through the [job coordinator](docs/job-coordinator.md), so it appears in `system.zetta_jobs` (FEAT-8; #387).
- [`docs/compute-near-data.md`](docs/compute-near-data.md) — **compute-near-data**: a DDF op executes over a node's LOCAL partitions (`zetta::ddf::load_local_partitions` + `run_near_data_group_by_job`, ARC-12 / FEAT-8; #516, the engine half of TC-11 #501). A node enumerates the partitions it **owns** from the [catalog](docs/dataset-partitions.md) (`owner_nodes` ∋ its id), reads only the partition bodies it [physically holds](docs/sharded-blob-placement.md) from its local large-object store (no `LOREQ`, no full-dataset pull), runs the op over that local shard, and exchanges only the small per-node partial over the [collectives](docs/collectives.md) — replacing the "shard handed in out-of-band" path. Instrumented (`bytes_read` near-data vs `Collectives::wire_bytes()` moved) so the acceptance — **bytes-moved ≈ partials (not ≈ dataset); result == the single-node oracle** — is directly checkable; no new JSON format (the body is the existing JsonlStore line shape, sha256-verified). Group-by is wired end to end; scan/filter/[featurize](docs/ddf-featurize.md) reuse the same loader. The TC-11 acceptance is verified as a gtest (`tests/test_ddf_local_partitions.cpp`) **and** surfaced as the runnable [test-suite](docs/test-suite.md) case `cluster.compute_near_data` (#501).
- [`docs/ddf-as-of.md`](docs/ddf-as-of.md) — the DDF window / as-of (point-in-time) op (`zetta::ddf::as_of_aggregate`): per-entity reduction of only the rows inside a time window (`time <= as_of`, optionally `>= window_start`) over a numeric DataFrame — the leakage-free featurization primitive over the DDF substrate, with a validated `AsOfAggregate` JSON boundary (ARC-12; #384).
- [`docs/ddf-featurize.md`](docs/ddf-featurize.md) — point-in-time featurization (`zetta::ddf::point_in_time_featurize` + `run_featurize_job`): the churn `churn_features.sql` shape expressed as a DDF graph (as-of window + groupby-aggregate + filter + equi-join) producing a leakage-free `FeatureTable` for training (every feature uses `time <= as_of`, dense/left-join over the account universe); distributed as a recorded `featurize` `ComputeJob` (distributed_join + distributed_group_by), proved against the synthetic churn fixture's deliberate post-t0 leakage trap, with a validated `FeaturizeSpec` JSON boundary (ARC-12 / FEAT-6; #388).
- [`docs/gbdt.md`](docs/gbdt.md) — distributed histogram GBDT training (`zetta::ml::train_gbdt` + `run_train_job`): histogram gradient-boosted decision trees with a binary-logistic objective — per-feature gradient/Hessian histograms over bins, **allreduced** across nodes each round to find splits, so single-node and multi-node grow equivalent trees; a host-side reference over device-backed feature columns; runs as a recorded `train` `ComputeJob` and **produces a `Model`** (schema-validated weights body + metrics + ranked drivers), trained on the [synthetic churn fixture](docs/churn-dataset.md) to a target AUC (FEAT-6 / ENT-14; #390).
- [`docs/gbdt-score.md`](docs/gbdt-score.md) — batch scoring / rank-at-risk over a trained `Model` (`zetta::ml::score_gbdt` + `run_score_job`): score entities through a GBDT model to a **calibrated risk** (the binary-logistic `predict_proba`) and rank them into an **at-risk list**; the distributed `score` `ComputeJob` is SPMD (per-shard predict, gather + device `sort_by_key` rank + broadcast → identical global ranking single-node == multi-node), `load_gbdt_model` re-validates the weights body at the GBDT model JSON Schema boundary; the inference counterpart to the trainer, GPU-native / CPU-fallback / distributed (FEAT-6 / CMD-10; #392).
- [`docs/calibration.md`](docs/calibration.md) — probability calibration (`zetta::ml::Calibrator` + `fit_calibrator` + `calibrate_model`): Platt scaling + isotonic regression fit on a held-out split so the GBDT's risk scores are well-calibrated (Brier / ECE improve, AUC preserved); attaches to a `GbdtModel` (predict_proba applies it, rides inside the weights body) and wires into the distributed `train` job via a sharding-invariant content-hash held-out split, schema-validated boundary (FEAT-6; #391).
- [`docs/tree-shap.md`](docs/tree-shap.md) — exact TreeSHAP explanation (`zetta::ml::tree_shap` + `run_explain_job`): exact path-dependent TreeSHAP over the trained GBDT trees (Lundberg 2018, Algorithm 2) using the per-node cover the trainer records on each `TreeNode` — per-entity per-feature attributions in raw/margin space with the efficiency invariant `base_value + Σ φ = predict_raw`, a per-entity dominant churn driver, and a distributed `explain` `ComputeJob` (per-entity SHAP over a shard + an allreduced global mean-|φ| driver ranking, single-node == multi-node) (FEAT-6; #393).
- [`docs/native-oracle.md`](docs/native-oracle.md) — the native correctness oracle (`zetta::ml::load_golden_oracle` + `tests/test_native_oracle.cpp`, RSK-8 / #395): proves the native featurize / GBDT-train / TreeSHAP reimplementations are numerically correct **without running any reference** (no Python/XGBoost/foreign runtime, in the engine, build, or CI) via three self-contained C++ layers over the [churn fixture](docs/churn-dataset.md) — committed **golden numeric fixtures** (cross-checked against a leakage-free ground truth + the SHAP efficiency identity `base_value + Σ φ = predicted`), **analytic** closed-form GBDT-split + TreeSHAP micro-tests, and single-node == 2-node == 3-node **equivalence**; the golden is a `pboettch`-validated boundary (`schemas/golden_oracle.schema.json`), regenerated offline via `ZETTA_WRITE_GOLDEN=1` (the generator is not a repo/CI dependency).
- [`docs/playbook.md`](docs/playbook.md) — the UC-1 retention playbook (`zetta::ml::generate_playbook`): turn the model's [calibrated risk](docs/gbdt-score.md) + [dominant churn driver](docs/tree-shap.md) into a **CRM-ready playbook** — a deterministic **driver→action→owner** dictionary (`PlaybookPolicy`, a hand-written JSON boundary), an **expected-retained-value priority** `P(churn) × ARR × P(save | dominant driver)` computed and **frozen in code**, and a **constrained narrator behind a seam** (`PlaybookNarrator`; default **Claude** via an injected `NarratorCompletion`, writes the narrative + picks the play within a strict schema validated at the `playbook_entry` boundary, **never invents numbers**) with a deterministic `RulesEngineNarrator` **fallback** so it runs end-to-end offline; plus an **outcome log** for the feedback loop. A pure library proven on the [churn fixture](docs/churn-dataset.md) deterministically (FEAT-6; #397).
- [`docs/uc1-end-to-end.md`](docs/uc1-end-to-end.md) — the **UC-1 end-to-end** integration test (`tests/test_uc1_end_to_end.cpp`, #399; realizes UC-1 / satisfies TP-1): drives the whole churn pipeline on the [synthetic fixture](docs/churn-dataset.md) in one test — synthetic data → [featurize](docs/ddf-featurize.md) (point-in-time, [entity-resolved](docs/entity-resolution.md)) → [train GBDT](docs/gbdt.md) → [explain (TreeSHAP)](docs/tree-shap.md) → ranked at-risk + drivers → playbook, behind the real [`ChurnStepExecutor`](docs/churn-step-executor.md) — asserting a **calibrated ranked list** + **division-correct resolution** + a **gated** ([correctness gate](docs/gate.md) incl. [`EntityResolutionCheck`](docs/entity-resolution-check.md), RULE-1) **governed** ([key-scoped access](docs/access-control.md) + [audit trail](docs/audit-trail.md)) run; a conflating resolver is shown to block the gate. Composes the already-built pieces (real StepExecutors, playbook, entity resolution, oracle parity) — no new engine code.
- [`docs/test-suite.md`](docs/test-suite.md) — the in-app **test suite** (`zetta::testkit`, #462): a **headless-runnable catalog of canned, parameterized test cases** over the real engine (`TestCase` = id + category + typed `TestOption`s + a `run(TestOptions, TestContext) -> TestResult` body; `TestSuite::builtin()`), spanning **compute/device** (CpuDevice ops; CUDA build state), **DDF** (scan/filter/group-by + the native [Parquet reader](docs/parquet-reader.md) TC-9 parity vs an embedded pyarrow golden — `ddf.parquet_reader_parity`, #499), **ML** (GBDT AUC / exact TreeSHAP efficiency identity / calibration), **distributed/cluster** over a **real multi-node loopback cluster** (`testkit::LoopbackCluster` — membership / allreduce / distributed-group-by == single-node / **compute-near-data** == single-node-oracle with only partials over the wire, TC-11; plus `cluster.partition_rebalance` — TC-10/#500 partition placement + rebalance-on-join converging `system.dataset_partitions` on every node), **governance** (key-scoped access / audit / the entity-resolution gate / the [bulk burst identity + teardown revocation](docs/burst-identity.md) lifecycle — TC-6), and the whole **UC-1 pipeline** (the [#399 end-to-end](docs/uc1-end-to-end.md)). The catalog lives in **zetta-core** (no SDL — runnable headless, `tests/test_testkit.cpp`); the headed **Operations → Test suite** panel (`App::draw_test_suite`) is a **thin renderer** that runs a selected case on a **worker thread** (so the loopback-cluster cases don't block the UI) with a per-test options modal + custom-data field. Self-contained data (`testkit::sample_churn_dataset`); `run_test_case` converts a thrown exception into a clean `Error` so a case can't crash the app; no new JSON format. The doc carries an **"adding a case" how-to** + an **inventory of candidate cases** (the backlog of engine surfaces — compute/DDF/ML/cluster/governance/provisioning/storage/query — that could become runnable cases, with effort/infra markers); extend the catalog there as new engine slices land. See [`docs/test-suite.md`](docs/test-suite.md).
- [`docs/example-scripts.md`](docs/example-scripts.md) — the **sim-Python example-script harness** (`zetta::testkit::run_example_script` + `example_scripts()`, #523): run a sim-Python **example program end to end as an integration test** — **parse** ([PythonFrontend](docs/python-frontend.md)) → **IR** → **gate** ([default_gate](docs/gate.md), RULE-1) → **run** ([Node::run_graph](docs/execution.md)) — and **assert on the outputs** (node/edge shape, step kinds, data-flow edges, produced `dataset`/`model`/`index` outputs), so *the example programs ARE the integration tests*. A green script doubles as a canonical usage example; a negative script proves a failure **surfaces the offending statement** (the parser's locator at compile, or the failing step's statement reconstructed from its `python` provenance bag). Each example is a `TestCase` under the new **`TestCategory::Examples`**, runnable headless (`tests/test_example_scripts.cpp`) and in the **Operations → Test suite** panel; non-throwing; **no new JSON format** (only the validated IR/Run boundaries). The catalog ships the green churn-pipeline + minimal + method-chain samples and the failure-surfacing negatives; the flagship TC-9/10/11/13 + UC-1-at-scale ([#531](https://github.com/oramasearch/zetta/issues/531)) scripts land as their engine bindings ([#522](https://github.com/oramasearch/zetta/issues/522) cluster+ML, parquet→DDF, partitioning, fulltext) do.
- [`docs/work-stealing-scheduler.md`](docs/work-stealing-scheduler.md) — the work-stealing scheduler (`zetta::compute::WorkStealingScheduler` local + `DistributedScheduler` cross-node): a job split into tasks, idle/faster workers/nodes steal pending work, no fixed capability model, **elastic mid-job membership** (join/leave/lost-node re-exec) (ARC-10 / #366 / #367).
- [`docs/job-coordinator.md`](docs/job-coordinator.md) — the job coordinator (`zetta::compute::JobCoordinator` / `JobRound`): the single entry the compute algorithms (DDF/GBDT/SHAP) submit work through — split a `ComputeJob` into tasks on the work-stealing scheduler, drive collectives at round boundaries (SPMD), drive the job lifecycle; owns + chains the scheduler + collectives over one signal-handler slot (ARC-11 / #372).
- [`docs/job-checkpoint.md`](docs/job-checkpoint.md) — **burst-job eviction tolerance** (`zetta::compute` checkpoint / restart, RULE-5 / RSK-9, #468): the **job-level** complement to the work-stealing scheduler's task-level lost-peer recovery — durably snapshot a long-running job's progress (`JobCheckpoint`: opaque sha256-fingerprinted state + a schema-validated boundary with a cross-field integrity check) behind a `CheckpointStore` seam (`FileCheckpointStore` reference: atomic temp-write+rename, corrupt-head fallback), so `run_resumable_rounds` **resumes from the last checkpoint** after a spot eviction — no recompute of committed rounds, graceful drain on a termination notice. Standalone seam (no Node/SQL/cluster wiring); first consumer is per-round [GBDT](docs/gbdt.md) model checkpointing, observable via `system.checkpoints`; a shared/replicated store + auto-restart on a detected eviction are later slices.
- [`docs/solution-catalog.md`](docs/solution-catalog.md) — the AI Solution Library (`zetta::SolutionCatalog`) + the headed Deploy panel: one-button deploy with a placement dial (FEAT-4 / SCR-5 / CMP-3).
- [`docs/churn-solution.md`](docs/churn-solution.md) — wiring the catalog **churn-prediction** blueprint to the **real engine** (`zetta::run_churn_solution`, FEAT-4 / UC-1 / #398): the catalog churn graph gains the `index` step to match the real pipeline, and a reusable bridge encodes a [`ChurnDataset`](docs/churn-dataset.md) → numeric DDF shards, builds the standard featurize spec + GBDT params, injects a [`ChurnStepExecutor`](docs/churn-step-executor.md), and runs the blueprint → a real trained `Model` + ranked at-risk list + churn drivers + playbook (not the stubbed `DefaultStepExecutor`); the Deploy panel's "Run on real engine" action (`demo_churn_dataset`, isolated single-node coordinator) + the acceptance test drive it; no new JSON format.
- [`docs/cloud-inspector.md`](docs/cloud-inspector.md) — the headed **Cloud Inspector** (`zetta::CloudProviderCatalog` + `zetta::CloudInspector` + `App::draw_cloud`): permission-gated inspection of deployment targets (local/on-prem/k8s/AWS/GCP/Azure/OCI) — a deny-by-default, ask-every-time consent gate that, on consent, runs the provider's read-only commands via the shared `CommandRunner`, parses each vendor's JSON at a validated boundary into identity/regions/nodes (no creds read), and surfaces it in the panel + `system.zetta_cloud_providers` / `system.zetta_cloud_status` (#445).
- [`docs/signal-plane.md`](docs/signal-plane.md) — the ephemeral signal plane (`zetta::Signal`): RPC / events / tasks over the cluster transport, addressed by node id, schema-validated at the boundary (ADR-28 / ARC-10).
- [`docs/collectives.md`](docs/collectives.md) — collective communication (`zetta::Collectives`): allreduce / broadcast / barrier / gather over the signal plane, deterministic across node order, for combining compute partials (ADR-28 / ARC-10; #365).
- [`docs/provisioner.md`](docs/provisioner.md) — the `Provisioner` seam + node capability model (`zetta::NodeCapability`): provision N nodes against a capability spec / teardown / list; cores / GPU type+throughput / memory (ARC-13 / ENT-32, #373) — and the `LocalProvisioner` given-hardware backend (uses existing cluster nodes, best-effort; no-op teardown; #374).
- [`docs/aws-provisioner.md`](docs/aws-provisioner.md) — the AWS cloud-burst Provisioner backend (`zetta::AwsProvisioner` + `AwsCliControlPlane`): burst N ephemeral AWS nodes (EC2 Auto Scaling Group / Spot shape via the `aws` CLI, no cloud SDK), mint bulk burst identities so they join the cluster, tear down + revoke; injectable control-plane + identity seams, schema-validated `AwsConfig` — the AWS mirror of the Azure reference per the [cloud-provisioner pattern](docs/cloud-provisioner-pattern.md) (ARC-13 / ADR-20, #377).
- [`docs/gcp-provisioner.md`](docs/gcp-provisioner.md) — the GCP cloud-burst `Provisioner` backend (`zetta::GcpProvisioner` + `GcpCliControlPlane`), mirroring the Azure reference via the [CLI pattern](docs/cloud-provisioner-pattern.md): burst N nodes (Managed Instance Group / Spot shape via the `gcloud` CLI, no cloud SDK), mint bulk burst identities so they join, tear down + revoke; injectable `GcpControlPlane` + identity seams, schema-validated `GcpConfig` (ARC-13 / ADR-20, #378).
- [`docs/oci-provisioner.md`](docs/oci-provisioner.md) — the OCI cloud-burst `Provisioner` backend (`zetta::OciProvisioner` + `OciCliControlPlane`), mirroring the Azure reference via the [CLI pattern](docs/cloud-provisioner-pattern.md): burst N nodes (Instance Pool / preemptible shape via the `oci` CLI, OCID-resolved by name, no cloud SDK), mint bulk burst identities so they join, tear down + revoke; injectable `OciControlPlane` + identity seams, schema-validated `OciConfig` (ARC-13 / ADR-20, #432).
- [`docs/k8s-provisioner.md`](docs/k8s-provisioner.md) — the Kubernetes cloud-burst `Provisioner` backend (`zetta::K8sProvisioner` + `K8sCliControlPlane`), mirroring the Azure reference via the [CLI pattern](docs/cloud-provisioner-pattern.md): burst N worker pods into an existing cluster (a Deployment scaled via the `kubectl` CLI, no client lib), mint bulk burst identities so they join, tear down + revoke; injectable `K8sControlPlane` + identity seams, schema-validated `K8sConfig` (ARC-13 / ADR-20, #434).
- [`docs/bare-metal-provisioner.md`](docs/bare-metal-provisioner.md) — the bare-metal (local-host) cloud-burst `Provisioner` backend (`zetta::BareMetalProvisioner` + `BareMetalCliControlPlane`), mirroring the Azure reference via the [CLI pattern](docs/cloud-provisioner-pattern.md): burst N local worker containers via the host runtime (`docker`/`podman`, no container SDK), mint bulk burst identities so they join, tear down + revoke; injectable `BareMetalControlPlane` + identity seams, schema-validated `BareMetalConfig`; complements `LocalProvisioner` by bursting (vs reporting) local nodes (ARC-13 / ADR-20, #436).
- [`docs/burst-identity.md`](docs/burst-identity.md) — bulk-mint a short-lived service `Principal` + `AccessKey` per burst node on provision, bulk-revoke on teardown, audited (`zetta::BurstIdentityManager`; reuses the identity model — ARC-13 / RSK-9, #375).
- [`docs/scaling-policy.md`](docs/scaling-policy.md) — the job objective / reward model (`zetta::ScalingPolicy`): a per-job autoscaler objective (minimize cost / runtime-latency target / data shape) with bounds (min/max nodes, cost cap) + objective evaluation over (progress, elapsed, cost) samples (ADR-29 / ENT-34, #370).
- [`docs/burst-approval-gate.md`](docs/burst-approval-gate.md) — the large-burst cost-cap approval gate (`zetta::BurstApprovalGate`): a `Provisioner` decorator that blocks an over-cap provision burst pending approval and records the decision in the audit trail (APR-3 / RSK-9, #379).
- [`docs/job-autoscaler.md`](docs/job-autoscaler.md) — the autoscaler control loop (`zetta::JobAutoscaler`): each tick observe a running `ComputeJob`'s progress, score it with the job's `ScalingPolicy`, and add/remove nodes through the `Provisioner` seam within bounds, with hysteresis (anti-thrash) + immediate cost-cap shed; reads live membership each tick (elastic) and degrades to best-effort max utilization on given hardware; schema-validated `AutoscalerConfig` (ADR-29 / ARC-11, #371).
- [`docs/add-nodes.md`](docs/add-nodes.md) — the **permanent-add control entry point** (`zetta::add_nodes`, #518): the "add 10 nodes to this cluster" action over the `Provisioner` seam — drive a provisioner (the cloud-burst `AzureProvisioner`, or any backend) to bring up N nodes that auto-join via the seed-peer handshake and persist as ordinary `node:<id>` members (the existing membership + replication path, no parallel roster). The *permanent* sibling of the [autoscaler](docs/job-autoscaler.md): provision-and-report only, **never** tearing a member down or revoking — elastic / cost-cap burst deferred; vendor-neutral; validated `AddNodesRequest` JSON boundary.
- [`docs/azure-provisioner.md`](docs/azure-provisioner.md) — the reference cloud-burst backend (`zetta::AzureProvisioner` + `AzCliControlPlane`): burst N ephemeral Azure nodes (VMSS / Spot shape via the `az` CLI, no cloud SDK), mint bulk burst identities so they join the cluster, tear down + revoke; injectable control-plane + identity seams, schema-validated `AzureConfig` (ARC-13 / ADR-20, #376; AWS #377 / GCP #378 mirror it).
- [`docs/cloud-provisioner-pattern.md`](docs/cloud-provisioner-pattern.md) — the recipe every cloud-burst backend follows (the shape `AzureProvisioner` set; AWS / GCP mirror): the four pieces, the shared `CommandRunner` + identity hooks, the lifecycle→CLI mapping (VMSS ↔ ASG ↔ MIG), the step-by-step — **CLI for now** (a REST/SDK binding is a drop-in control-plane swap later).
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
