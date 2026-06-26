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
see [docs/project-inputs.md](docs/project-inputs.md)) and its **execution graphs**
(the `zetta::ir::ExecutionGraph` IR, persisted + replicated as JSON — see
[docs/execution-graph.md](docs/execution-graph.md)). A node can also **deploy** a
validated execution graph onto a target, recording the placement (R1 local; see
[docs/deployments.md](docs/deployments.md)), and owns **runs** (`zetta::Run`), the
record of one graph execution: status, logs, outputs, and lineage, validated
against a JSON Schema at the storage boundary and viewable in the headed app's
read-only **Runs / Outputs / Lineage** panel (see [docs/runs.md](docs/runs.md)).
Query it all with basic
Postgres-flavored SQL (DDL + INSERT/SELECT, plus the virtual `system` tables); the
query engine is dialect-agnostic, so other SQL formats can be added as frontends.
The async runtime is coroutine-native (C++
Alliance Capy/Corosio), behind the `zetta::runtime` seam. State replicates
between nodes as an Automerge CRDT. See [docs/](docs/) for details.

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
rows, [projects](docs/projects.md), [project inputs](docs/project-inputs.md), and
[runs](docs/runs.md) created on any node converge onto every
node — in memory and on disk**, conflict-free, with no leader (the CRDT document
is the source of truth; disk follows). [Large objects](docs/large-objects.md)
replicate too — their metadata
(size + SHA-256) in the CRDT, the file bodies transferred peer-to-peer and
verified against the hash. You can see **all nodes and their replication status** (entries,
version, in-sync/syncing) three ways: the headed **Cluster** panel's Nodes table,
the headless heartbeat log, or SQL — `SELECT * FROM system.zetta_nodes;`. See
[docs/cluster.md](docs/cluster.md) and [docs/replication.md](docs/replication.md).
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
