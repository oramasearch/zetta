# Architecture

## Node and run modes

A **node** (`zetta::Node`) is one running instance of the database. It owns the
data and knows nothing about any UI, so it can run two ways:

- **headed** (default) — `zetta::gui::App` opens an SDL3 + Dear ImGui window and
  renders a live view of the node.
- **headless** — `zetta::run_headless` ticks the node on a 10 Hz heartbeat with
  no UI; for servers with no display.

`main()` parses arguments, builds the `Node` and a `Cluster` (cluster networking
runs on its own background thread), starts them, dispatches to a run mode, then
stops them. Both modes install SIGINT/SIGTERM handlers so terminal Ctrl-C shuts
down cleanly.

The core is the static library **`zetta-core`**, which has **no SDL/ImGui
dependency** — the build-level guarantee that a node runs headless. The GUI is a
separate translation unit linking `zetta-core`.

## Data model

```
Node
 ├── Database        (CRUD: create / find / drop / rename)
 │    └── Table       (create / find / drop)
 │         └── rows   (JSON documents, via a StorageEngine)
 ├── Project         (CRUD: create / find / drop / rename)
 │    │               the central AI-project artifact — see projects.md
 │    ├── ProjectInput (create / find / drop) — raw intent/artifact + provenance
 │    └── ProjectGraph  (create / find / drop — a named ExecutionGraph IR)
 ├── Deployment      (deploy_graph / find / list)
 │                    a graph placed onto a target — see deployments.md
 ├── Run             (CRUD: create / find / drop, keyed by id)
 │                    one graph execution: status/logs/outputs/lineage — see runs.md
 └── Principal       (CRUD: create / find / drop / rename)
      │               an authenticated actor — see access-control.md
      └── AccessKey   (create / find / drop) — a scoped login credential
```

Ownership and lookups use `std::shared_ptr` handles, never raw pointers. Names
are unique within their parent; lookups return an empty handle on a miss.
A **project** is a top-level entity (no parent) holding id + name +
created/updated + an owner key; see [projects.md](projects.md). A project owns
its **inputs** (`ProjectInput`) — raw intent/artifact payloads captured with
provenance (kind, source, content hash, created-by/at) — and its named
**execution graphs** (`ProjectGraph`, persisted + replicated as JSON), the same
way a database owns its tables; see [project-inputs.md](project-inputs.md) and
[projects.md](projects.md). A **deployment** is also top-level: a validated
[execution graph](execution-graph.md) placed onto a `DeploymentTarget` (R1
local) and recorded; see [deployments.md](deployments.md). A **run** is another
top-level entity — the record of one execution of a deployed graph (status,
logs, outputs, lineage), keyed by id and validated against a JSON Schema at the
storage boundary; see [runs.md](runs.md). A **principal** is also a top-level
entity — an authenticated actor (user / service / node) that owns its **access
keys** (`AccessKey`). A key carries a **scope**, and a request inherits the scope
of the key it logged in with: V1 **key-scoped access**, deny-by-default
(RULE-3); see [access-control.md](access-control.md).

Everything persists to disk under the node's `data_dir` and is rebuilt by
`Node::start()` on launch. See [storage.md](storage.md).

## Query layer

Text → `Dialect` (parser) → generic AST (`zetta::query::Statement`) →
`Executor` → results. The executor is dialect-neutral; new database formats are
added by writing a `Dialect` that targets the same AST. See [query.md](query.md).

## Execution graph IR

The **ExecutionGraph IR** (`zetta::ir`) is the in-memory model of a data/AI
system as a directed graph of typed steps, tagged by the
connect→clean→model→index→serve→act taxonomy. It is the keystone the compilers,
the gate, deploy, and run build on. It has a canonical **JSON form**
(`to_json`/`from_json`, schema-validated at the boundary) — how a graph persists
and replicates under a Project; the dual-form code/columnar representation and
semantic validation (the gate) are later slices. See
[execution-graph.md](execution-graph.md).

**Deploy** has its first slice: `Node::deploy_graph` places a validated graph onto
a `DeploymentTarget` (R1 local) and records the placement (replicated + queryable).
See [deployments.md](deployments.md). **Run** has its first slice too:
`zetta::GraphExecutor` / `Node::run_graph` executes a graph step-by-step in
dependency order, producing a [Run](runs.md) with outputs + lineage (a pluggable
`StepExecutor` seam for per-step work). See [execution.md](execution.md).

## Async runtime & cluster

The coroutine / IO / networking backend (C++ Alliance Capy + Corosio, vendored
in `modules/`) lives behind the `zetta::runtime` seam — see
[runtime.md](runtime.md). Its first user is the **cluster**: nodes form a mesh
of equal peers over TCP via `zetta::Cluster` — see [cluster.md](cluster.md).

**Replication** (`zetta::ReplicationManager`) holds a node's DDL/metadata/rows
as an Automerge CRDT (`modules/automerge-cpp`) and merges peers' state
conflict-free — see [replication.md](replication.md).

**Accelerated compute** (`zetta::compute::ComputeDevice`) is the seam every heavy
numeric operator targets — a typed device-buffer memory model plus a
coarse-grained operator set (map/elementwise, reduce/scan, histogram, gemm/axpy,
sort, gather/scatter) that CPU and (optional) CUDA backends implement, the same
pure-virtual seam pattern as `StepExecutor` / `StorageEngine`. No backend or
SDL/CUDA type lives in zetta-core; new hardware is a new backend, consumers
unchanged — see [compute-device.md](compute-device.md).

## Module map

```
include/zetta/            zetta-core public API (no SDL/ImGui)
  node.hpp                Node — CRUD over databases + projects + deployments + runs
  database.hpp            Database — owns tables (disk-backed)
  project.hpp             Project — an AI project; owns inputs + execution graphs
  project_input.hpp       ProjectInput — raw intent/artifact input + provenance
  project_graph.hpp       ProjectGraph — a named ExecutionGraph under a project
  deployment_target.hpp   DeploymentTarget — a placement (ENT-6; R1 local)
  deployment.hpp          Deployment — a graph placed onto a target
  run.hpp                 Run — one graph execution (status/logs/outputs/lineage) + JSON Schema
  executor.hpp            GraphExecutor / StepExecutor — execute a graph into a Run
  table.hpp               Table — owns a storage engine (its rows)
  uuid.hpp                new_uuid() — v4 UUID for folder names
  runtime.hpp             zetta::runtime — async/coroutine seam over Capy
  runtime/net.hpp         networking slice of the seam (Corosio; adapter-only)
  cluster.hpp             Cluster — equal-peer membership over TCP
  replication.hpp         ReplicationManager — Automerge CRDT replicated state
  headless.hpp            run_headless(Node&, Cluster&)
  signals.hpp             install_signal_handlers / stop_requested
  large_object.hpp        LargeObjectStore — files as blobs (Postgres LO API)
  import.hpp              import_records — JSON/JSONL files into a table
  bytes.hpp / duration.hpp human-readable size / time formatters
  storage/engine.hpp      StorageEngine interface
  storage/json_store.hpp  JsonStore — in-memory engine
  storage/jsonl_store.hpp JsonlStore — on-disk JSONL engine (default)
  query/ast.hpp           generic, dialect-neutral statement AST
  query/dialect.hpp       Dialect interface + ParseResult
  query/postgres.hpp      PostgresDialect (parser)
  query/executor.hpp      Executor — runs the AST against a Node
  query/service.hpp       QueryService — parse + execute facade
  ir/step_kind.hpp        StepKind — connect→clean→model→index→serve→act taxonomy
  ir/execution_graph.hpp  ExecutionGraph IR — typed step nodes + edges
  ir/serialization.hpp    to_json/from_json — the IR's JSON form + validation
  compute/compute_device.hpp  ComputeDevice — the accelerator seam (memory + operators)
  compute/device_buffer.hpp   DeviceBuffer — typed, owning, move-only device memory
  compute/host_stub_device.hpp HostStubDevice — host-memory transfer stub (no backend)
src/core/                 zetta-core implementation (incl. cluster.cpp adapter)
src/gui/                  headed UI (App), links SDL3 + ImGui
src/main.cpp              arg parsing + mode dispatch
modules/                  vendored deps (git submodules: capy, corosio, automerge-cpp)
tests/                    GoogleTest unit tests (CTest)
```
