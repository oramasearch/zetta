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
— strategy, architecture, domain model, roadmap. **This repo's docs:**
[docs/README.md](docs/README.md) (domain-organized). The product set lives in
[docs/product/](docs/product/README.md) — [orama-zetta.md](docs/product/orama-zetta.md)
(read-first framing), [vision](docs/product/vision.md) ·
[personas](docs/product/personas.md) · [positioning](docs/product/positioning.md) ·
[catalog](docs/product/catalog.md) · [control surfaces](docs/product/control-surfaces.md),
and the per-audience [experience contracts](docs/product/experience/README.md)
(UX · DX · AX · serving).

## Under the hood

This repo is Orama Zetta's core: a fast, equal-peer **CRDT-replicated** store
with clustering, distributed compute, and a native ML pipeline. A *zetta node*
is one running instance — **headed** (default: an SDL3 + Dear ImGui desktop UI
styled to the Orama design system; the desktop is itself a cluster member) or
**headless** (no UI, for servers). C++26, macOS-first, single statically-linked
binary. (`zetta` is the binary + codename; **Orama Zetta** is the product name —
the older name *"ZettaDB"* is retired.) It is designed as **true hybrid** — one
equal-peer cluster spanning every vendor and architecture; the seams are built,
the breadth of backends is in flight
([docs/architecture/true-hybrid.md](docs/architecture/true-hybrid.md)).

What's inside, by domain (each links to that domain's docs):

- **[AI projects + the pipeline](docs/pipeline/README.md)** — the central,
  user-managed **AI project** (`zetta::pipeline::Project`) owns its raw **inputs** (NL
  intent, imported artifacts such as n8n workflows) and its **execution
  graphs** (the `zetta::ir::ExecutionGraph` IR: a typed
  `connect→clean→model→index→serve→act` step graph, persisted + replicated as
  JSON). Three compilers produce graphs — NL intent, n8n workflow JSON, and a
  simulated-Python frontend — every graph passes the **correctness gate**
  (schema/type, data-leakage/point-in-time, entity-resolution checks) before it
  can deploy, and a **deployment controller** reconciles desired→actual and
  self-heals failed runs. Runs record status, logs, outputs, and lineage. The
  **AI Solution Library** ships ready-to-deploy blueprints behind a one-button
  Deploy panel.
- **[Data + storage](docs/data/README.md)** — databases → tables → JSON rows on
  a pluggable `StorageEngine` (JSONL on disk), large objects with peer-to-peer
  body transfer, **partitioned tables** (pluggable `Partitioner`, rendezvous
  placement, sharded blob placement, rebalance-on-join), a **native Parquet
  reader** (no Arrow), dataset ingest (parquet/jsonl/json/csv), and a
  Postgres-subset SQL layer that is **dialect-agnostic** with virtual
  `system.*` observability tables.
- **[Cluster + replication](docs/cluster/README.md)** — equal peers over TCP,
  no leader; **everything replicates** as an Automerge CRDT (the document is
  the source of truth, disk is its projection); an ephemeral **signal plane**
  (RPC / events / tasks) carries **collectives** (allreduce / broadcast /
  barrier / gather) and cluster-wide telemetry.
- **[Distributed compute](docs/compute/README.md)** — the `ComputeDevice`
  accelerator seam (CPU always; CUDA opt-in), a **work-stealing scheduler**
  (local + cross-node, elastic mid-job), the **job coordinator** (SPMD job
  spine every algorithm submits through), recorded **compute jobs**, a
  reward-driven **job autoscaler**, spot-eviction **checkpoint/restart**, and
  **compute-near-data** execution over owned partitions.
- **[DDF — distributed dataframes](docs/ddf/README.md)** — typed, chunked,
  device-backed frames with local ops (scan/filter/project/sort) and
  distributed ops (sort, equi-join, group-by, as-of/point-in-time windows)
  composing into **leakage-free featurization**; single-node == multi-node.
- **[Native ML](docs/ml/README.md)** — distributed **histogram GBDT** training
  (allreduced histograms), probability **calibration**, batch **scoring /
  rank-at-risk**, **exact TreeSHAP** explanation, a registered **Model** entity
  (weights as verified large-object bodies), all proven by a **native
  correctness oracle** (no Python/XGBoost anywhere).
  The [methodology registry](docs/ml/methodologies.md) lists all 21 portfolio
  approaches: M-1 and M-14 run today, M-10 has partial retrieval support, and the others
  name their implementation issues. Intent compilation and run dispatch use
  this registry and refuse unsupported model work.
- **[Indexing](docs/indexing/README.md)** — the pluggable `IndexBackend` seam;
  the first real backend is **oramacore-cpp full-text (BM25F)**, with
  per-partition **distributed full-text** answered by scatter-gather.
- **[Cloud + provisioning](docs/cloud/README.md)** — the `Provisioner` seam
  with **seven backends** (local, Azure reference, AWS, GCP, OCI, Kubernetes,
  bare-metal containers) driven through vendor CLIs (no SDKs), bulk **burst
  identity**, a **cost-cap approval gate**, and a **permission-gated Cloud
  Inspector** (deny-by-default, ask-every-time consent).
- **[Governance](docs/governance/README.md)** — V1 **key-scoped access**
  (deny-by-default scopes + TTL'd capabilities), an immutable, append-only
  **audit trail**, and a **governed console** (every statement
  capability-scoped, gated, and audited).
- **[The programmatic control surface](docs/api/README.md)** — one **capability
  registry** where each entry renders three ways at once: an **MCP tool**, a
  **REST route**, and a palette action. An embedded Beast2 server (both run
  modes, default `127.0.0.1:4800`) answers **`POST /mcp`** (JSON-RPC 2.0), plain
  **REST** (`GET/POST /v1/<name>`, `GET /v1/openapi.json`), the
  **[llms.txt](docs/api/llms-endpoints.md)** progressive-disclosure pages, and a
  **spec index** (`list_specs`/`get_spec`) — every call key-scoped,
  consent-gated where it touches cloud, and threaded as an observability span.
- **[The headed GUI](docs/gui/README.md)** — project-first Home, New Project,
  Graph Workspace (editable canvas + graph-over-cluster plan view), Deploy,
  Runs/Models/Jobs, a spatial **Cluster view** with live per-node gauges and a
  distributed-algorithm-at-work animation, Datasets, Console, Cloud Inspector,
  Admin, and an in-app **Test suite** panel.
- **[UC-1 churn](docs/use-cases/README.md)** — the first catalog use case, end
  to end and at scale: synthetic fixture, division-correct entity resolution,
  the real `ChurnStepExecutor` (each heavy step a recorded compute job), the
  retention playbook with a constrained narrator, and the sim-Python
  **UC-1-at-scale** capstone over a real multi-node cluster.

zetta **vendors its build dependencies** as git submodules under
[`modules/`](modules/) (`capy`, `corosio`, `automerge-cpp`, and the private
`oramacore-cpp`).

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
(default 16). All inter-node traffic runs **mutual TLS**, and joining needs the
cluster's **join token** (generated by the first node; see
[docs/cluster/security.md](docs/cluster/security.md)). At startup a node prints
the address(es) it's reachable at and the exact command — token included — to
add another node.

```bash
zetta                                          # first node — prints the join command
zetta --peer 192.168.1.10:7777 \
      --join-token <64-hex>                    # another machine joins it
zetta --peer 127.0.0.1:7777 --join-token ...   # second node on this host (auto-binds 7778)
zetta --listen 127.0.0.1:7777                  # restrict to loopback (not network-reachable)
```

A **provisioned / burst node** can auto-join with no manual flags: the binary
reads `ZETTA_SEED_PEER` (→ `--peer`) and `ZETTA_CLUSTER_TOKEN` (→ `--join-token`),
plus `ZETTA_DATA_DIR` (→ `--data-dir`), from its environment on boot, as defaults
that any explicit flag overrides. So a node started with those env vars set joins
the secure cluster automatically. See
[docs/cloud/node-bootstrap-env.md](docs/cloud/node-bootstrap-env.md).

Data replicates between nodes as an Automerge CRDT — databases, tables, rows,
projects, runs, models, jobs, principals, and the audit trail created on any
node converge onto every node, in memory and on disk, conflict-free, with no
leader. Large-object bodies transfer peer-to-peer and are SHA-256-verified;
partitioned tables place their bodies on owner nodes only. See all nodes and
their replication status in the headed **Cluster** panel, the headless
heartbeat log, or SQL (`SELECT * FROM system.nodes;`). Detail:
[docs/cluster/](docs/cluster/README.md).

The first configure clones SDL3, Dear ImGui, and GoogleTest via CMake
`FetchContent` (a few minutes); the Capy/Corosio coroutine stack and
automerge-cpp come from the `modules/` submodules. Subsequent builds are fast
(ccache-backed when available).

## Tests

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

There is also an in-app, headless-runnable test-suite catalog (including the
distributed UC-1-at-scale capstone) — see [docs/testing/](docs/testing/README.md).

## Requirements

- CMake 3.28+ and Ninja
- Git submodules initialized recursively (`git clone --recurse-submodules`, or
  `git submodule update --init --recursive`) — the build needs
  `modules/{capy,corosio,automerge-cpp}`, and automerge-cpp nests its own
  submodules (nlohmann/json). The private `modules/oramacore-cpp` is optional —
  without it the full-text backend degrades gracefully and the build stays
  green.
- A C++26 Clang 17+ — Homebrew LLVM on macOS (`brew install llvm`),
  `clang++-NN` on Linux; Apple's system Clang and Ubuntu's GCC 13 are too old.
  The build auto-selects a suitable one. `./scripts/install-deps.sh` installs
  everything.
- macOS for the headed UI (SDL3 uses a Metal-backed renderer); the headless
  core is portable (Linux builds and runs).

Full details and troubleshooting: **[docs/build.md](docs/build.md)**.
