# Orama Zetta

**Orama Zetta turns a question into running, deployed intelligence.** Describe a data
or AI problem in plain language — or hand over an artifact you already have (an n8n
workflow, a SQL script, a notebook) — and Orama Zetta architects a correct pipeline,
deploys it onto infrastructure **you** control (your k8s / cloud / on-prem), runs it,
and **learns from your data**. *The data engineer, the AI engineer, and the platform
engineer — in a bottle.* It compresses the path from **intent → deployed, correct
intelligence** from quarters to hours — **Intelligence Velocity for Big Thinkers.**

> *Zetta*, as in *zettabyte* — one billion terabytes. We build for the ceiling, so
> your scale is free headroom: solve very large data problems and the small ones come
> for free.

**Full vision + blueprint:** the [Orama Zetta docs site](https://github.com/oramasearch/zetta-docs)
— strategy, architecture, domain model, roadmap.

## Under the hood

This repo is Orama Zetta's core. Today that's a fast, equal-peer **CRDT-replicated**
store with clustering — JSONL on-disk storage, large objects, and a Postgres-SQL
subset. A *zetta node* is one running instance; it runs **headed** (default — an SDL3
+ Dear ImGui desktop UI, so the desktop is itself a cluster member) or **headless**
(no UI, for servers with no display). C++26, macOS-first, single statically-linked
binary. (`zetta` is the binary + codename; **Orama Zetta** is the product name — the
older name *"ZettaDB"* is retired.) The broader product surface — GUI control plane,
one-button deploy onto k8s/cloud, tiered storage, compute-near-data, the learning
loop — is in flight.

Data is organized as databases → tables → JSON rows, persisted to disk. A node
also owns **AI projects** (`zetta::Project`) — the central, user-managed artifact
(see [docs/projects.md](docs/projects.md)); each project owns its **inputs**
(`zetta::ProjectInput`: raw intent/artifact payloads captured with provenance —
see [docs/project-inputs.md](docs/project-inputs.md); an
[n8n workflow JSON](docs/n8n-import.md) is the first typed artifact, schema-
validated at the boundary and stored byte-for-byte, and
[compiles](docs/n8n-compile.md) into the IR — nodes → typed steps, connections →
edges). In headed mode the **New Project** screen is the front door for all of
this — name a project, paste a natural-language intent, and import / drag-drop an
n8n workflow artifact in one atomic create (see
[docs/new-project-screen.md](docs/new-project-screen.md)). Each project also owns
its **execution graphs**
(the `zetta::ir::ExecutionGraph` IR, persisted + replicated as JSON — see
[docs/execution-graph.md](docs/execution-graph.md)). A stored intent **compiles**
into a reviewable graph skeleton — `zetta::compile_intent` turns the natural-
language text into a templated `connect→clean→model→index→serve→act` pipeline
(the churn shape today), editable in the workspace rather than a black box (see
[docs/intent-compiler.md](docs/intent-compiler.md)). In headed mode the **Graph
Workspace** panel renders a project's graph as an editable node/edge canvas
(steps in columns by stage, edges between them) and persists edits back to the IR
(see [docs/projects.md](docs/projects.md#graph-workspace)). A node can also **deploy** a
validated execution graph onto a target, recording the placement (R1 local; see
[docs/deployments.md](docs/deployments.md)) — the headed **Deploy panel** does this
as one-button deploy: pick a blueprint from the **AI Solution Library**, choose a
placement on the dial, pass the correctness gate, and see its status (see
[docs/solution-catalog.md](docs/solution-catalog.md)). A **control plane**
reconciles each local deployment desired→actual and **self-heals** a killed or
failed run (see [docs/deployment-controller.md](docs/deployment-controller.md)).
It owns **runs** (`zetta::Run`), the
record of one graph execution: status, logs, outputs, and lineage, validated
against a JSON Schema at the storage boundary and viewable in the headed app's
read-only **Runs / Outputs / Lineage** panel (see [docs/runs.md](docs/runs.md)).
It can **execute** a graph step-by-step into one of those runs
(`zetta::GraphExecutor` / `Node::run_graph` — walk the steps in dependency order,
produce outputs + lineage; see [docs/execution.md](docs/execution.md)).
It owns **models** (`zetta::Model`) — trained models registered on the node:
metadata (kind, feature schema, metrics) replicated as `model:<uuid>`, with the
serialized **weights** stored as a large-object body that transfers peer-to-peer
and is SHA-256-verified; a `system.zetta_models` registry table lists them (see
[docs/models.md](docs/models.md)).
The **`Provisioner`** seam (`zetta::Provisioner`) plus a node **capability model**
(`zetta::NodeCapability` — cores / GPU type+throughput / memory) open the native
distributed accelerated compute pass: provision N nodes against a capability spec,
tear them down, and list them — the abstraction the autoscaler consumes and the
Local / cloud backends implement. The first backend, **`zetta::LocalProvisioner`**
(given hardware), uses the nodes already in the cluster best-effort — it adds no
capacity and its teardown is a no-op (see
[docs/provisioner.md](docs/provisioner.md)).
The first **cloud** backend, **`zetta::AzureProvisioner`** (the reference cloud
burst; AWS / GCP mirror it), bursts N ephemeral Azure nodes, mints bulk burst
identities so they join the cluster, runs cluster-parallel, and tears them down —
behind an injectable control-plane seam with **no cloud SDK** (the reference
`AzCliControlPlane` borrows the VMSS / Spot shape via the `az` CLI), driven by a
schema-validated `AzureConfig` (see
[docs/azure-provisioner.md](docs/azure-provisioner.md)).
On a burst, the **`BurstIdentityManager`** (`zetta::BurstIdentityManager`)
bulk-mints a short-lived, job-scoped service `Principal` + `AccessKey` per burst
node and bulk-revokes them on teardown — reusing the identity model, audited
(see [docs/burst-identity.md](docs/burst-identity.md)).
A **large-burst cost-cap approval gate** (`zetta::BurstApprovalGate`) decorates the
seam: a provision burst over a configured cap is blocked pending approval and
recorded in the audit trail, guarding against runaway burst cost (see
[docs/burst-approval-gate.md](docs/burst-approval-gate.md)).
It owns **compute jobs** (`zetta::ComputeJob`), the foundational record of one
unit of native distributed accelerated compute — featurize / train / explain /
score / a DDF op — with a lifecycle (queued → running → succeeded | failed),
audited at create + finish and JSON-Schema validated (see
[docs/compute-jobs.md](docs/compute-jobs.md)). A job carries a **`ScalingPolicy`**
(`zetta::ScalingPolicy`) — the per-job autoscaler reward objective (minimize cost
/ runtime-latency target / data shape) inside bounds (min/max nodes, cost cap),
with an `evaluate()` over (progress, elapsed, cost) samples (see
[docs/scaling-policy.md](docs/scaling-policy.md)).
It also owns **principals** (`zetta::Principal`) and their **access keys**
(`zetta::AccessKey`): a request inherits the scope of the key it logged in with —
V1 **key-scoped access**, deny-by-default (see
[docs/access-control.md](docs/access-control.md)) — and an immutable,
append-only **audit trail** (`zetta::AuditEvent`) that records every graph
change, deploy, and data-access decision, replicated and JSON-Schema validated
(see [docs/audit-trail.md](docs/audit-trail.md)). The headed app surfaces all of
this read-only in an **Admin & Settings** panel — principals + keys, their scope
+ capability grants, and the audit log (see
[docs/admin-panel.md](docs/admin-panel.md)). Query it all with basic
Postgres-flavored SQL (DDL + INSERT/SELECT, plus the virtual `system` tables); the
query engine is dialect-agnostic, so other SQL formats can be added as frontends.
The async runtime is coroutine-native (C++
Alliance Capy/Corosio), behind the `zetta::runtime` seam (which also exposes a
`parallel_for` data-parallel primitive over a shared thread pool). Heavy numeric
work targets the `zetta::compute::ComputeDevice` seam — a typed device-buffer
memory model plus a coarse-grained operator set (map/reduce/scan, histogram,
gemm/axpy, sort, gather/scatter) that CPU and (optional) CUDA backends implement,
so new hardware is a new backend and consumers are unchanged. The backends build
into a `zetta-compute` library; the default build is CPU-only (no GPU toolkit
needed), with the CUDA backend opt-in behind `-DZETTA_ENABLE_CUDA=ON` (see
[docs/compute-device.md](docs/compute-device.md)). The always-built reference
backend is `zetta::compute::CpuDevice`, which runs every operator on host memory,
multi-threaded via `runtime::parallel_for` (see
[docs/cpu-device.md](docs/cpu-device.md)); a per-node
`zetta::compute::DeviceRegistry` enumerates those backends, reports each one's
capability, and selects one per op — GPU when present, else CPU (see
[docs/device-registry.md](docs/device-registry.md)). State replicates between
nodes as an Automerge CRDT. See [docs/](docs/) for details.

zetta **vendors its build dependencies** as git submodules under
[`modules/`](modules/) (`capy`, `corosio`, and `automerge-cpp`).

## Build + run

Full instructions — prerequisites, supported compilers, submodules,
troubleshooting — are in **[docs/build.md](docs/build.md)**. The short version:

```bash
# Clone with submodules (or run the submodule step in an existing checkout):
git clone --recurse-submodules git@github.com:oramasearch/zetta.git
#   …existing checkout: git submodule update --init --recursive

./scripts/install-deps.sh                # system tools (macOS / Ubuntu)

cmake -S . -B build -G Ninja
cmake --build build

./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta --help
```

## Clustering

Run multiple nodes as a cluster of **equal peers**. Each node listens on all
interfaces by default (`0.0.0.0:7777`) and dials any `--peer` you give it.
Several nodes can share a host — each binds the first free port in a range
(default 16). At startup a node prints the address(es) it's reachable at — the
wildcard bind expands to the machine's real LAN IPs — and the exact command to
add another node.

```bash
zetta                                # first node — prints its LAN address to share
zetta --peer 192.168.1.10:7777       # another machine joins it
zetta --peer 127.0.0.1:7777          # second node on this host (auto-binds 7778)
zetta --listen 127.0.0.1:7777        # restrict to loopback (not network-reachable)
```

Data replicates between nodes as an [Automerge](docs/replication.md) CRDT
(replicate-to-all policy; each node has a UUIDv4 actor id): **databases, tables,
rows, [projects](docs/projects.md), [project inputs](docs/project-inputs.md),
[runs](docs/runs.md), [models](docs/models.md),
[compute jobs](docs/compute-jobs.md),
[principals + access keys](docs/access-control.md), and the
[audit trail](docs/audit-trail.md)
created on any node converge onto every
node — in memory and on disk**, conflict-free, with no leader (the CRDT document
is the source of truth; disk follows). [Large objects](docs/large-objects.md)
replicate too — their metadata
(size + SHA-256) in the CRDT, the file bodies transferred peer-to-peer and
verified against the hash. You can see **all nodes and their replication status** (entries,
version, in-sync/syncing) three ways: the headed **Cluster** panel's Nodes table,
the headless heartbeat log, or SQL — `SELECT * FROM system.zetta_nodes;`. See
[docs/cluster.md](docs/cluster.md) and [docs/replication.md](docs/replication.md).

The same peer connections also carry a **[signal plane](docs/signal-plane.md)** —
ephemeral, typed messages addressed by node id: request/response **RPC**,
fire-and-forget **events** (unicast or broadcast), and **task** envelopes. Unlike
replicated state, signals aren't routed through the CRDT; they're the substrate
for distributed compute (collectives, work-stealing, job dispatch), schema-
validated at the boundary. On top of it ride
**[collectives](docs/collectives.md)** — `allreduce` / `broadcast` / `barrier` /
`gather` for combining per-node compute partials, deterministic regardless of node
order.
zetta is **multi-leader** (no global election); leadership applies only to future
exclusive structures (locks) — see the consistency model in the replication doc.

The first configure clones SDL3, Dear ImGui, and GoogleTest via CMake
`FetchContent` (a few minutes); the Capy/Corosio coroutine stack and
automerge-cpp (which vendors nlohmann/json) come from the `modules/` submodules.
Subsequent builds are fast (ccache-backed when available).

## Tests

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

## Requirements

- CMake 3.28+ and Ninja
- Git submodules initialized recursively (`git clone --recurse-submodules`, or
  `git submodule update --init --recursive`) — the build needs
  `modules/{capy,corosio,automerge-cpp}`, and automerge-cpp nests its own
  submodules (nlohmann/json)
- A C++26 Clang 17+ — Homebrew LLVM on macOS (`brew install llvm`, Clang 21),
  `clang++-NN` on Linux; C++26 and Capy/Corosio need Clang 17+, so Apple's
  system Clang and Ubuntu's GCC 13 are too old. The build auto-selects a
  suitable one. `./scripts/install-deps.sh` installs everything.
- macOS for the headed UI (SDL3 uses a Metal-backed renderer); the headless
  core is portable (Linux builds and runs).

Full details and troubleshooting: **[docs/build.md](docs/build.md)**.
