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
`DataFrameSchema`/`ScalingPolicy` pattern). The `ComputeJob` `ddf` kind that schedules
these ops is a later issue. See [docs/ddf-as-of.md](docs/ddf-as-of.md).

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
(`src/core/catalog.hpp`): `node:<id>` (membership), `db:<uuid>`, `tbl:<uuid>`,
`row:<uuid>`, `lo:<uuid>`, `project:<uuid>`, `input:<uuid>` (a project input —
metadata only; its payload body stays on disk), `graph:<uuid>`,
`deployment:<uuid>`, `run:<uuid>`, `model:<uuid>` (a trained model — metadata
only; its serialized weights body stays on disk like an `lo:` body),
`job:<uuid>` (a compute job — ARC-11/ENT-31),
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
  <job-uuid>/                ← one folder per compute job (UUIDv4 = its id)
    job.json                 ← { id, kind, status, project, graph, run,
                                  scaling_policy, created, updated, detail }
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
sourced from `Node::models()`; qualified name only) and **`system.zetta_jobs`**
(the compute-job registry — one row per unit of accelerated compute + its
lifecycle, sourced from `Node::jobs()`; qualified name only). `system` is
reserved (can't be created/dropped/written; no real db may be named it) and is
where future system tables (e.g. `system.zetta_leadership`) live. Full detail in
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
what an inspect would touch — its `cli`, the `config_paths` it would **read**
(read-only), and the read-only `inspect_commands` it would **run** (never
provision/deploy/teardown). It performs **no cloud access of any kind** — no SDK,
CLI, file read, or network call; `requires_consent()` is false only for Local
(which reads nothing — it is this node), true for every other backend. Because
there is no untrusted input there is no JSON boundary to validate (unlike the
solution catalog). The panel (list + detail, like Runs/Admin) tags each provider
`[open]`/`[lock]`, resets consent when the selection changes, and gates the
"Allow inspection of `<name>`" action behind the named files/commands. **This
slice is scaffold + gate only: no real config read or cloud call happens even
after consent** — wiring the gate to the real read-only inspect (invoking the CLI
control planes) is the next slice. See [docs/cloud-inspector.md](docs/cloud-inspector.md).

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
CMakeLists.txt              build graph: imgui lib, zetta-compute lib (+ optional CUDA), zetta-core lib, zetta exe
cmake/embed_font.cmake      bakes a .ttf into a C++ header at configure time
assets/fonts/               embedded Geist + Geist Mono TTFs (SIL OFL 1.1) + README
scripts/install-deps.sh     install system build tools (macOS / Ubuntu)
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the node; CRUD over databases + projects + deployments + runs + models + jobs + principals; run_graph; the audit sink
  database.hpp              zetta::Database — owns tables (disk-backed)
  project.hpp               zetta::Project — an AI project; owns inputs + execution graphs
  project_input.hpp         zetta::ProjectInput — raw intent/artifact input + provenance
  project_graph.hpp         zetta::ProjectGraph — a named ExecutionGraph under a project
  deployment_target.hpp     zetta::DeploymentTarget — a placement (ENT-6; R1 local)
  deployment.hpp            zetta::Deployment — a graph placed onto a target
  deployment_controller.hpp zetta::DeploymentController — reconcile local deployments desired→actual + self-heal (FEAT-4/TC-3)
  node_capability.hpp       zetta::NodeCapability/Gpu/GpuKind — node compute capability model (cores/GPU/memory) + satisfies (ENT-32)
  provisioner.hpp           zetta::Provisioner/ProvisionedNode — provision N nodes against a capability spec / teardown / list (ARC-13; #373)
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
  cloud_provider.hpp        zetta::CloudProvider/CloudProviderCatalog — pure-metadata catalog of deployment-target backends for the permission-gated Cloud Inspector (no cloud access)
  run.hpp                   zetta::Run — one graph execution (status/logs/outputs/lineage) + Run JSON Schema
  model.hpp                 zetta::Model — a trained model (metadata + weights fingerprint) + Model JSON Schema (ENT-14/FEAT-6)
  compute_job.hpp           zetta::ComputeJob — one unit of accelerated compute (kind/lifecycle) + ComputeJob JSON Schema (ARC-11/ENT-31)
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
  runtime.hpp               zetta::runtime — async/coroutine seam over Capy (task/block_on/thread_pool + parallel_for data-parallel fan-out)
  runtime/net.hpp           networking slice of the seam (Corosio; adapter-only)
  cluster.hpp               zetta::Cluster — equal-peer membership over TCP + the signal-plane API
  signal.hpp                zetta::Signal/SignalHandler — ephemeral RPC/event/task signals + envelope encode/decode (ADR-28)
  collectives.hpp           zetta::Collectives — allreduce/broadcast/barrier/gather over the signal plane (#365)
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
  ddf/
    data_frame.hpp          zetta::ddf::DataFrame/Column/DataFrameSchema — typed columnar frame over a table, chunked, ComputeDevice-backed + loader/infer/JSON boundary (ARC-12/ENT-33; #380)
    ops.hpp                 zetta::ddf::scan/filter/project — DDF ops over a DataFrame, built on the ComputeDevice seam (gather-based filter, map/elementwise computed columns) + FilterPredicate/Projection JSON boundaries (ARC-12; #381)
    join.hpp                zetta::ddf::distributed_join — DDF distributed inner equi-join: repartition by key via collectives (gather+broadcast) then local hash/sort-merge join per shard (ARC-12; #382)
    data_frame_ops.hpp      zetta::ddf::sort/concat/slice — frame-shaping DDF ops: order by a key column (device sort_by_key), row-wise union, contiguous row range (ARC-12; #385)
    distributed_sort.hpp    zetta::ddf::DistributedSorter/PartitionMessage — distributed sort across the cluster (sample-partition + local sort + merge) over collectives + the signal-plane all-to-all shuffle (ARC-12; #385)
    group_by.hpp            zetta::ddf::group_by/distributed_group_by — DDF groupby-aggregate (count/sum/min/max/mean), partial-per-node + gather/merge/broadcast combine + GroupBySpec JSON boundary (ARC-12; #383)
    as_of.hpp               zetta::ddf::as_of_aggregate/AsOfAggregate — DDF window / as-of (point-in-time) op: per-entity reduce of rows in a time window (time <= as_of), the leakage-free featurization primitive + JSON boundary (ARC-12; #384)
    group_by_job.hpp        zetta::ddf::run_group_by_job — run the distributed groupby-aggregate as a recorded ComputeJob (kind ddf) through the job coordinator (drives distributed_group_by on the JobRound; result on every node) (FEAT-8; #387)
src/core/                   zetta-core impl
  catalog.hpp               flat-key scheme for the replicated catalog (internal)
  node.cpp                  load_databases()/load_projects()/load_deployments()/load_runs()/load_jobs() + replication capture/reconcile/persist
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
  run.cpp                   zetta::Run — value, mutators, JSON-Schema boundary validation (parse_run_value)
  model.cpp                 zetta::Model — value, mutators, weights body I/O, JSON-Schema boundary validation (parse_model_value)
  compute_job.cpp           zetta::ComputeJob — value, set_status (+ finish audit), JSON-Schema boundary validation (parse_compute_job_value)
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
  compute/device_buffer.cpp DeviceBuffer lifetime (free via device) + ComputeDevice::make_buffer (→ zetta-compute lib)
  compute/host_stub_device.cpp  HostStubDevice — host-memory allocate/transfer; operators unsupported (→ zetta-compute)
  compute/cuda_device.cu    CudaDevice — CUDA runtime memory model + Thrust/custom operator kernels, CpuDevice-conformant (→ zetta-compute; CUDA build only, #363/#361)
  compute/cpu_device.cpp    CpuDevice — every operator on host memory, multi-threaded via runtime::parallel_for (zetta-core; #360)
  compute/device_registry.cpp  DeviceRegistry — enumerate/report/select devices + DeviceInfo JSON boundary (zetta-core; #362)
  compute/work_stealing_scheduler.cpp  WorkStealingScheduler — worker threads, per-worker deques + the steal loop (zetta-core; #366)
  compute/distributed_scheduler.cpp  DistributedScheduler — the cross-node steal protocol + its JSON-Schema boundary + elastic membership re-queue (zetta-core; #366/#367)
  compute/job_coordinator.cpp  JobCoordinator — lifecycle driving (Running→Succeeded|Failed) + the shared signal-handler chain over Collectives + DistributedScheduler (zetta-core; #372)
  ddf/data_frame.cpp        DataFrame loader (table→typed columns, chunked, device-backed), schema inference + DataFrameSchema JSON boundary (zetta-core; #380)
  ddf/ops.cpp               DDF scan/filter/project ops over a DataFrame (gather-based filter + repack, map/elementwise computed columns) + FilterPredicate/Projection JSON boundaries (zetta-core; #381)
  ddf/join.cpp              distributed_join + to_host_relation + hash/sort-merge join + collective repartition (gather/broadcast all-to-all) + deserialize boundary check (zetta-core; #382)
  ddf/data_frame_ops.cpp    zetta::ddf::sort/concat/slice + the internal HostFrame host-staging (data_frame_internal.hpp) shared with the distributed sort (zetta-core; #385)
  ddf/distributed_sort.cpp  zetta::ddf::DistributedSorter — sample-sort across the cluster + the ddf.shuffle all-to-all + PartitionMessage JSON-Schema boundary (zetta-core; #385)
  ddf/group_by.cpp          group_by/local_partial/GroupByPartial — groupby-aggregate accumulate/merge/finalize, the partial wire form + GroupBySpec JSON boundary (zetta-core; #383)
  ddf/as_of.cpp             as_of_aggregate — host-side per-entity windowed/point-in-time reduction (group key + time mask) + AsOfAggregate JSON boundary (zetta-core; #384)
  ddf/group_by_job.cpp      run_group_by_job — drive the distributed groupby-aggregate as a recorded ddf ComputeJob through the coordinator (calls distributed_group_by on the JobRound) (zetta-core; #387)
  runtime.cpp               zetta::runtime::parallel_for / parallel_chunks — data-parallel fan-out over the shared Capy thread pool
  cluster.cpp               cluster networking adapter (the one Corosio user) + signal-plane transport
  signal.cpp                zetta::Signal — envelope JSON form + JSON-Schema boundary (encode/decode)
  collectives.cpp           zetta::Collectives — collective rounds (star reduce/broadcast) + message JSON-Schema boundary
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
- [`docs/new-project-screen.md`](docs/new-project-screen.md) — the headed New Project screen (SCR-2): name + paste NL intent + import/drag-drop an n8n workflow artifact, in one atomic create (FEAT-1).
- [`docs/runs.md`](docs/runs.md) — the Run (`zetta::Run`) record: status/logs/outputs/lineage + JSON-Schema validation.
- [`docs/models.md`](docs/models.md) — the Model (`zetta::Model`) entity (ENT-14/FEAT-6): replicated `model:<uuid>` metadata + serialized weights as a peer-to-peer large-object body (sha256-verified) + the `system.zetta_models` registry.
- [`docs/compute-jobs.md`](docs/compute-jobs.md) — the ComputeJob (`zetta::ComputeJob`) record: a unit of native distributed accelerated compute (featurize/train/explain/score/ddf) — lifecycle, create/finish audit, JSON-Schema boundary (ARC-11 / ENT-31).
- [`docs/access-control.md`](docs/access-control.md) — key-scoped access (V1): `Principal` + `AccessKey` + `Scope` (resources) + `Capability` (rights, TTL'd), deny-by-default authorization (RULE-3 / ENT-22).
- [`docs/audit-trail.md`](docs/audit-trail.md) — the immutable, append-only `AuditEvent` (ENT-23 / FEAT-9): graph change / deploy / data-access events, replicated + JSON-Schema validated.
- [`docs/admin-panel.md`](docs/admin-panel.md) — the headed **Admin & Settings panel** (`App::draw_admin`, SCR-7 / CMP-5): read-only view of principals + keys, scope + capability grants, and the audit log.
- [`docs/jobs-panel.md`](docs/jobs-panel.md) — the headed **Compute Jobs panel** (`App::draw_jobs`, FEAT-5 / SCR): live `system.zetta_jobs` (ComputeJobs + status) + per-job objective/scaling controls previewing `ScalingPolicy::evaluate()` (set the reward, add/remove nodes); pairs with the trained-Model + metrics + ranked drivers in the Runs panel.
- [`docs/storage.md`](docs/storage.md) — on-disk format + storage-engine seam.
- [`docs/query.md`](docs/query.md) — the dialect-agnostic query layer + Postgres subset, incl. comparison `WHERE` and **`GROUP BY` aggregation routed through the [DDF engine](docs/ddf.md)** (ARC-12 / API-2, #386).
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
- [`docs/compute-device.md`](docs/compute-device.md) — the `zetta::compute::ComputeDevice` accelerator seam: typed device buffers + memory model + the operator set + the host-memory stub (ADR-27 / ARC-9; #359), plus the `zetta-compute` build target + the optional CUDA backend (`CudaDevice`, `-DZETTA_ENABLE_CUDA=ON`; #363).
- [`docs/cpu-device.md`](docs/cpu-device.md) — the `zetta::compute::CpuDevice` reference backend: every operator on host memory, multi-threaded via `runtime::parallel_for`, the numerical reference for CUDA (ADR-27 / ARC-9; #360).
- [`docs/device-registry.md`](docs/device-registry.md) — the per-node device registry (`zetta::compute::DeviceRegistry`): enumerate a node's compute devices, report each one's capability, select a backend per op (accelerator-first / CPU fallback), with the `DeviceInfo` JSON boundary (ARC-9; #362).
- [`docs/ddf.md`](docs/ddf.md) — the DDF DataFrame core (`zetta::ddf::DataFrame`): a typed, columnar, chunked frame over the storage engine, each chunk a `ComputeDevice` buffer; table→columns loader + schema inference + value/JSON boundaries — the substrate DDF ops operate on (ARC-12 / GLO-11 / ENT-33; #380).
- [`docs/ddf-ops.md`](docs/ddf-ops.md) — the local DDF DataFrame ops: `zetta::ddf::scan` / `filter` / `project` (read a table into a frame, `gather`-based row filtering, `map`/`elementwise` computed columns, with schema-validated `FilterPredicate` / `Projection` JSON boundaries; #381) and `zetta::ddf::sort` / `concat` / `slice` (key-column sort via the device `sort_by_key`, row-wise union, contiguous row range — the single-node building blocks of the distributed sort; #385) (ARC-12).
- [`docs/ddf-distributed-sort.md`](docs/ddf-distributed-sort.md) — the DDF distributed sort (`zetta::ddf::DistributedSorter`): sample-partition + local sort + merge across the cluster — splitter agreement over the [collectives](docs/collectives.md), an all-to-all row shuffle over the [signal plane](docs/signal-plane.md) (schema-validated `PartitionMessage`), returning globally range-ordered, locally-sorted partitions; a single-node cluster returns the locally-sorted shard (ARC-12; #385).
- [`docs/ddf-join.md`](docs/ddf-join.md) — the DDF distributed equi-join (`zetta::ddf::distributed_join`): a distributed DDF op — repartition two frames by key across the cluster via collectives (gather + broadcast all-to-all), then a local hash / sort-merge join per shard, so single-node and multi-node yield identical results; rides the schema-validated `CollectiveMessage` with a range-checked deserialize boundary, no new wire format (ARC-12; #382).
- [`docs/ddf-groupby.md`](docs/ddf-groupby.md) — the DDF distributed groupby-aggregate (`zetta::ddf::group_by` / `distributed_group_by`): groupby + count/sum/min/max/mean (an extensible op set) over a frame's columns, partial-per-node combined via gather/merge/broadcast over the collectives — results identical single- and multi-node; a DDF operator + schema-validated `GroupBySpec` boundary (ARC-12; #383) — and runs as a recorded `ddf` `ComputeJob` (`zetta::ddf::run_group_by_job`, `group_by_job.hpp`) through the [job coordinator](docs/job-coordinator.md), so it appears in `system.zetta_jobs` (FEAT-8; #387).
- [`docs/ddf-as-of.md`](docs/ddf-as-of.md) — the DDF window / as-of (point-in-time) op (`zetta::ddf::as_of_aggregate`): per-entity reduction of only the rows inside a time window (`time <= as_of`, optionally `>= window_start`) over a numeric DataFrame — the leakage-free featurization primitive over the DDF substrate, with a validated `AsOfAggregate` JSON boundary (ARC-12; #384).
- [`docs/work-stealing-scheduler.md`](docs/work-stealing-scheduler.md) — the work-stealing scheduler (`zetta::compute::WorkStealingScheduler` local + `DistributedScheduler` cross-node): a job split into tasks, idle/faster workers/nodes steal pending work, no fixed capability model, **elastic mid-job membership** (join/leave/lost-node re-exec) (ARC-10 / #366 / #367).
- [`docs/job-coordinator.md`](docs/job-coordinator.md) — the job coordinator (`zetta::compute::JobCoordinator` / `JobRound`): the single entry the compute algorithms (DDF/GBDT/SHAP) submit work through — split a `ComputeJob` into tasks on the work-stealing scheduler, drive collectives at round boundaries (SPMD), drive the job lifecycle; owns + chains the scheduler + collectives over one signal-handler slot (ARC-11 / #372).
- [`docs/solution-catalog.md`](docs/solution-catalog.md) — the AI Solution Library (`zetta::SolutionCatalog`) + the headed Deploy panel: one-button deploy with a placement dial (FEAT-4 / SCR-5 / CMP-3).
- [`docs/cloud-inspector.md`](docs/cloud-inspector.md) — the headed **Cloud Inspector** (`zetta::CloudProviderCatalog` + `App::draw_cloud`): permission-gated inspection of deployment targets (local/on-prem/k8s/AWS/GCP/Azure/OCI) — a deny-by-default, ask-every-time consent gate naming what an inspect would read/call; scaffold + gate, no real cloud access yet.
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
