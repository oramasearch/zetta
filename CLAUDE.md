# zetta — Claude rules

> **`zetta` is the codename/binary for the product *Orama Zetta*** — not "just a
> database." Read **[`docs/product/orama-zetta.md`](docs/product/orama-zetta.md)**
> first for what we're building and the working decisions: naming (Orama Zetta;
> "ZettaDB" retired), value-first messaging, use-case-driven development, **AI
> projects as the central item** (use cases like churn are generic AI-project
> types; manage projects to success — most AI projects fail), the first use case
> (**churn / UC-1**), and **growing the use-case catalog**. Source of truth is
> the wtm `ORAMA-ZETTA` blueprint (epic
> [`#279`](https://github.com/oramasearch/zetta/issues/279)). The full product
> set is [`docs/product/`](docs/product/README.md) — vision · personas ·
> positioning · catalog · control surfaces · the
> [experience contracts](docs/product/experience/README.md) (UX · DX · AX ·
> serving). This file is the engineering rules for the codebase.

**Technically, a zetta node is a database.** A *zetta node* is one running
instance. It runs in two modes:

- **headed** (default) — the node plus an SDL3 + Dear ImGui UI rendering it.
- **headless** — the node alone, no UI, for servers with no display.

C++26, macOS-first, single statically-linked binary.

## The first principle — native modern C++26, coroutine-based, cluster-native, distributed. ALWAYS

**Every solution zetta ships is (1) native, modern C++26, (2) coroutine-based,
(3) cluster-native, and (4) distributed. There are no exceptions, and "just for
now" is not one.**

- **Native, modern C++26.** No foreign runtime, no vendor SDK, no driver — and
  **no shelling out to a CLI to do work the engine should do** (ADR-17). If a
  capability needs a protocol, implement the protocol. If `zetta::runtime`
  lacks a primitive you need, *add it there* — never route around it with a
  subprocess. And **modern** is not decoration: coroutines over callbacks,
  ranges and algorithms over index loops, concepts over SFINAE, value semantics
  and RAII over manual lifetimes, `std::string`/`string_view`/`span` over raw
  buffers and pointer arithmetic. C-with-classes is as much a violation of this
  rule as a subprocess is. Follow the
  [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/); the full
  statement is [C++ conventions](#c-conventions) below.
- **Coroutine-based.** zetta is a coroutine-native codebase, not a threaded one
  with coroutines bolted on. Async work is an awaitable from
  `zetta::runtime` (`task<T>` / `io_task<T>`, `spawn`, `when_all`,
  `timeout`, `async_event`) — never a blocking mutex/condition-variable
  surface, never a thread-per-request, never a sleep-poll loop. Concurrency is
  expressed by composing awaitables (`when_all` over N in-flight operations),
  which is also how a single node saturates its own link while the cluster
  saturates all of them. A raw `std::mutex` is permitted only as the lowest
  cross-thread handoff *inside* a backend adapter (the one TU that names the
  backend library) — contained, commented, never in a public API.
- **Cluster-native.** A node is an equal peer in a cluster, not a server that
  happens to replicate. State is replicated state; work is *placed*, not
  centralized.
- **Distributed.** The unit of execution is the **cluster**, not the node.
  Work fans out over owned partitions and combines through the collectives,
  and the aggregate resource of N nodes — CPU, memory, disk, **and network
  bandwidth** — is the resource being spent.

**This includes AI — especially AI.** Featurization, training, scoring,
explanation and search are native distributed C++ exactly like everything else:
the histogram-GBDT trains by **gradient-histogram allreduce** across nodes,
TreeSHAP explains natively, point-in-time featurize runs **compute-near-data**
over owned partitions, and fulltext is **per-partition scatter-gather**. zetta
does **not** embed Python / XGBoost / PyTorch to train, and it does **not** call
a hosted model API to compute an answer (ADR-17). *"Just call an LLM for this
part"* is the same violation as *"just shell out to curl"* — the model is the
product, not a dependency. (The one sanctioned LLM use is the **optional,
user-managed** playbook layer that writes narrative and picks a play from
numbers the engine already computed — it never produces a number, and nothing
depends on it.)

**Anti-patterns that have actually shipped here. Do not repeat them:**

| Looks done | Why it isn't |
| --- | --- |
| Shelling out to `curl` for an HTTP request | A subprocess is not native C++. `zetta::runtime::net` already has sockets + TLS, and Boost.HTTP is vendored — use them. |
| Tiling work across N nodes, then running every tile on the coordinator | That is a parallel loop, not a distributed algorithm. It spends one node's bandwidth and N times nothing. |
| "Single-node now, distribute later" | Valid ONLY as a reference implementation with the distributed path designed behind the same API — never as the shipped answer. |
| Pulling a whole result set to one node, then sharding it | The data crosses the wire twice and the ingest is bounded by one link. Fetch where it lands. |
| `std::this_thread::sleep_for` to poll a remote operation | Blocks a thread to wait. Compose an awaitable with a timer/`timeout` instead — a coroutine that is waiting should cost nothing. |
| Embedding Python / XGBoost / PyTorch, or calling a model API, to train or infer | The model is the product. Implement it natively and distribute it (histogram-allreduce GBDT, native TreeSHAP). |

**If you cannot see how a design distributes, it is not ready to build.** Say
so and stop — do not ship the single-node version and call it done. The
detailed statement of this rule, with the precedents to copy, is
**Distributed-first — every algorithm scales at both levels** under
[Conventions](#conventions).

## How these docs work — progressive disclosure

This file holds only **identity, the load-bearing rules, build/test, and
conventions**. Everything else is disclosed progressively, in four layers:

1. **This file** — always loaded; the rules below apply to all zetta work.
2. **Directory `CLAUDE.md` rule-cards** — auto-loaded when working in that
   tree; the rules scoped to it:
   [`src/core/`](src/core/CLAUDE.md) (replication invariants, boundaries,
   seams) · [`src/gui/`](src/gui/CLAUDE.md) (thin-renderer rule, design
   system, rendering gotchas) · [`include/zetta/`](include/zetta/CLAUDE.md)
   (public-API rules) · [`tests/`](tests/CLAUDE.md) (test conventions) ·
   [`docs/`](docs/CLAUDE.md) (filing rules) ·
   [`schemas/`](schemas/CLAUDE.md) (generated, don't hand-edit).
3. **[`docs/README.md`](docs/README.md) → `docs/<domain>/README.md`** — the
   domain index. **Before working in a domain, read its README** (a short
   overview + a one-line index of its pages).
4. **`docs/<domain>/<page>.md`** — one page per shipped slice (symbols, issue
   refs, boundaries, scope notes).

**"Let's reload the full project context"** → read
[`docs/full-context.md`](docs/full-context.md) — the kept-current map of
repos, systems of record, shipped state, the active wave, and working
agreements. Update it whenever project state shifts (PR merges, plan
changes, new foundations).

For navigating the source tree, read
**[`docs/architecture/code-map.md`](docs/architecture/code-map.md)** — the
annotated file-by-file map (kept in sync by the docs discipline below).

**Keep this disclosure network current — it is a hard rule, same commit as the
change.** Every change updates the layer that owns it: a new rule or gotcha →
the directory's `CLAUDE.md` rule-card (or this file if repo-wide); a new or
changed slice → its `docs/<domain>/<page>.md` **and** the domain `README.md`
index; a file added/moved/renamed → `docs/architecture/code-map.md`; a new
domain → the `docs/README.md` table. The network is only useful while every
level can be trusted — a stale card is worse than none.

| When touching… | Read first |
|---|---|
| Product framing, naming, use cases | [`docs/product/`](docs/product/README.md), [`docs/use-cases/`](docs/use-cases/README.md) |
| Anything a user, developer, or agent experiences | [`docs/product/experience/`](docs/product/experience/README.md) → ux.md · dx.md · ax.md · serving.md — update the affected page in the same commit |
| Node, run modes, module layout, schemas-as-code | [`docs/architecture/`](docs/architecture/README.md) |
| Storage, large objects, partitioning, Parquet, SQL | [`docs/data/`](docs/data/README.md) |
| Cluster, replication/CRDT, signal plane, collectives, transport security (mTLS/join token) | [`docs/cluster/`](docs/cluster/README.md) |
| Devices, schedulers, jobs, autoscaler, near-data | [`docs/compute/`](docs/compute/README.md) |
| DataFrames + distributed ops | [`docs/ddf/`](docs/ddf/README.md) |
| GBDT, calibration, scoring, TreeSHAP, Model | [`docs/ml/`](docs/ml/README.md) |
| **Building a model / layer / autograd op / tokenizer / training or RLVR loop natively** (no foreign runtime) | [`docs/autograd/building-ai-primitives.md`](docs/autograd/building-ai-primitives.md) + the **native-ai-dev** skill · the autograd engine [`docs/autograd/`](docs/autograd/README.md) · the tokenizer [`docs/tokenizer/`](docs/tokenizer/README.md) · the RLVR agent [`docs/agent/`](docs/agent/README.md) |
| **Generating RLVR training data / a task corpus** (from a solutions catalog or a customer's data) | the **rlvr-training-data** skill · the agent domain [`docs/agent/`](docs/agent/README.md) · reference to reproduce: `homer6/blender-rl` |
| Projects, IR, compilers, gate, deploy, runs, catalog | [`docs/pipeline/`](docs/pipeline/README.md) |
| Indexes (full-text, distributed) | [`docs/indexing/`](docs/indexing/README.md) |
| MCP tools, REST routes, the capability registry | [`docs/api/`](docs/api/README.md) — registry-design.md is the settled design |
| Provisioners, cloud burst, cloud inspector | [`docs/cloud/`](docs/cloud/README.md) |
| Access control, audit, governed console | [`docs/governance/`](docs/governance/README.md) |
| Any GUI change | [`docs/gui/`](docs/gui/README.md) — design system + rendering notes are mandatory reads |
| UC-1 churn end to end | [`docs/use-cases/`](docs/use-cases/README.md) |
| Tests, the in-app test suite, runbooks | [`docs/testing/`](docs/testing/README.md) |

## Deployment under the Orama operator (One Orama)

Zetta is deployed + operated by the **orama-cloud-v1-operator** (One Orama,
[orama-bridge#130](https://github.com/oramasearch/orama-bridge/issues/130)) as an `OramaZettaCluster` —
a StatefulSet of headless nodes + a headless peer Service (:7777) + a client Service (:4800). Two facts
shape how the engine is used there:

- **One CRDT cluster spans many k8s clusters (and bare hosts).** Nodes are equal peers joined on :7777;
  they are **not** bound to one k8s cluster. Topology in practice: **N k8s clusters → 1 operator each →
  1 shared Zetta.** Cross-cluster peering needs a routable :7777 for the **wan-accessible** nodes
  (same-LAN direct; cross-cloud → a **per-node** `LoadBalancer` on 7777 so each advertises its own IP and
  sync spreads across all their uplinks — aggregate bandwidth); **dial-out-only** nodes (NAT / private
  pods) advertise no dialable address and only reach outbound. Peer traffic is **always mTLS** — one
  shared join token per cluster; there is **no plaintext mode** (the `--cluster-plaintext` escape hatch
  was removed as a security hole, [operator#92](https://github.com/oramasearch/orama-cloud-v1-operator/issues/92)).
  A dial-out-only node must **never advertise its private pod IP**, and peers must **never dial it** — else,
  when two clusters share a pod CIDR (e.g. k0s + AKS both `10.244.0.x`), the dial hits the wrong node and
  undercounts membership ([#803](https://github.com/oramasearch/zetta/issues/803)); it syncs via its one
  outbound link + the CRDT node-list ([#800](https://github.com/oramasearch/zetta/issues/800)).
- **Zetta is the operators' datastore.** Each operator provisions its own schema (`orama_control` /
  `orama_data`) over `/v1/sql` and syncs its observed state into it, namespaced by a per-operator id so a
  shared store never collides. See the operator's
  [`docs/zetta-datastore.md`](https://github.com/oramasearch/orama-cloud-v1-operator/blob/main/docs/zetta-datastore.md).
  Planned: Zetta signals the operator for elastic capacity (→ Karpenter → more nodes,
  [operator#69](https://github.com/oramasearch/orama-cloud-v1-operator/issues/69)).

## Repository shape: bridge + modules

zetta is both a buildable product **and** a bridge repo: pinned third-party
dependencies live as **git submodules under `modules/`** — **capy** (coroutines),
**corosio** (networking), **automerge-cpp** (from-scratch C++23 CRDT; nests its
own submodules, which also provide nlohmann/json), and **oramacore-cpp** (C++23
BM25F full-text port; a **private** repo, so its CMake link is guarded on the
submodule's presence via `ZETTA_WITH_ORAMACORE` — a checkout without it still
builds green). Clone with `--recurse-submodules` (the recursion matters), or
`git submodule update --init --recursive`. The GUI stack (SDL3, Dear ImGui,
GoogleTest, pboettch/json-schema-validator) comes via CMake `FetchContent`.
`modules/README.md` is the index.

## Architecture — the load-bearing rules

- **`zetta-core` (the static lib) has NO SDL/ImGui dependency.** `zetta::Node`
  knows nothing about any UI; the GUI is a thin renderer driven by a Node, and
  headless mode drives the same Node with no renderer. Enforced at the build
  level — never let SDL/ImGui types leak into `zetta-core`.

- **EVERYTHING is replicated state.** The Automerge CRDT document
  (`ReplicationManager`) is the **single source of truth** for all distributed
  state — schemas (DDL), rows, entities, large-object metadata, membership. The
  on-disk store + in-memory `Node` tree are a **projection** of it. Never keep a
  parallel copy synced by hand. New shared state = a new key/shape in the
  document; a new *kind* of agreement = a new `ReplicationPolicy` — never a
  bespoke sync path. (Known migration debt: cluster membership still lives in
  the `Cluster` peer table — move things *toward* this rule, never away.) See
  [`docs/cluster/replication.md`](docs/cluster/replication.md).

- **Capture / gossip / apply.** Local mutations are **captured** into the CRDT
  at the data-model layer (`Node`/`Database`/`Table`/stores hold a
  `ReplicationManager*`), so *every* write path replicates. Peers **gossip**
  `save()`/`merge()` blobs. `Node::tick()` **reconciles** the on-disk projection
  from the document (materialize-only, never re-capture — no feedback loop) and
  persists it. Reconcile paths never emit audit events (a peer's action arrives
  already-audited).

- **Thread safety:** the cluster's **network thread** may only touch thread-safe
  things (`ReplicationManager` is mutex-guarded; `BlobStore` works off the CRDT
  + disk). The `Node` tree / stores are **main-thread only** (capture + reconcile
  run there).

- **Multi-leader by default; leadership is a scoped exception.** No global
  leader or election (equal peers). Per-(node, resource) leadership for
  *exclusive* structures (locks) is **documented design only**, not built.
  Don't add a global leader.

- **Pluggable seams — extend, don't special-case.** New capability = a new
  implementation of an existing seam, never a special case in a consumer:
  storage formats implement `StorageEngine`; query dialects implement `Dialect`
  against the generic AST (the `Executor` is dialect-neutral); per-step run work
  implements `StepExecutor`; gate checks implement `Check`; heavy numeric ops
  target `ComputeDevice` (new hardware = a new backend); indexes implement
  `IndexBackend`; placement implements `Partitioner`; infrastructure backends
  implement `Provisioner` (+ a `<Vendor>ControlPlane` behind the shared
  `CommandRunner` — CLI, no cloud SDK); input languages implement
  `LanguageFrontend` (see the **new-input** skill). Signals ride the ephemeral
  **signal plane** (the one deliberate exception to "everything is replicated":
  a signal is a *message*, not *state*).

- **Async/networking seams:** all coroutine/IO goes through `zetta::runtime`
  (`include/zetta/runtime.hpp`); networking through `zetta::runtime::net`,
  included **only** by the two networking adapters —
  `src/core/cluster/cluster.cpp` (inbound/peer traffic) and
  `src/core/api/native_http_transport.cpp` (outbound client traffic, the
  connector transport, #827). **Never name `boost::capy` / `boost::corosio`
  directly** outside those seams — if the seam lacks a primitive, extend
  `runtime.hpp` (that is where `when_all` / `timeout` came from).

- **Coroutines first.** zetta is a coroutine-native codebase: async APIs are
  expressed as awaitables through the `zetta::runtime` seam (`task<T>`,
  `spawn`, `parallel_for`, `async_event`), never as blocking
  mutex/condition-variable surfaces. A raw `std::mutex` is permitted ONLY as
  the lowest-level cross-thread handoff inside a seam adapter (the cluster's
  per-peer outbox, a bounded telemetry ring) — contained, commented, and never
  visible in a public API. If the runtime seam lacks a primitive you need,
  extend the seam (one alias/adapter in `runtime.hpp`), don't work around it.

- **Observability first (RULE: every operation is observable).** Every
  operation captures its **intent** (what the caller asked for, verbatim),
  its **plan** (what the engine decided to do), its **performance** (spans
  with timings), and its **result shape** — as OpenTelemetry-style spans
  threaded by trace id across surfaces and nodes, surfaced as `system.*`
  read views and returned to API callers (an MCP agent sees the trace id +
  timing of every call and can read the full trace back). Errors carry the
  engine's reason; crashes carry stacktraces. See
  [`docs/architecture/observability.md`](docs/architecture/observability.md)
  — new operations MUST open a span; new failures MUST be observable.

- **No exceptions across zetta-core seams.** Boundary functions are
  non-throwing (`std::optional` / status types + a path-qualified reason).
  Untrusted input is **validated at the boundary**: top-level replicated
  entities use the embedded draft-07 schema + pboettch validator (the Run
  pattern); value-type DTOs use the hand-written non-throwing `from_json`
  mirroring their type's `*_schema()` accessor. **Schema as code:** the C++
  embedded schema is the source of truth for EVERY boundary schema
  (`zetta::schemas::boundary_schemas()` is the table); `schemas/*.json` is
  generated and drift-checked (`ZETTA_WRITE_SCHEMAS=1` regenerates). See
  [`docs/architecture/schemas.md`](docs/architecture/schemas.md).

- **Surface observable state as a `system.<table>` by default (RULE-7).** State
  exposed for inspection — nodes, deployments, models, jobs, datasets, cloud
  state — is a virtual, read-only `system` table computed at query time from the
  live source, never a stored or hand-synced copy, and the GUI renders the same
  source. Name system tables **plainly** (`system.nodes`, `system.datasets`),
  no `zetta_` prefix — the legacy `system.zetta_*` spellings were renamed in
  [`#538`](https://github.com/oramasearch/zetta/issues/538) and remain only as
  back-compat query aliases. Durable
  state's CRDT is the read view's source; ephemeral/observed state (telemetry,
  cloud inspects) is a read view with no CRDT backing. Cloud-derived tables stay
  permission-gated.

## Build + run + tests

```bash
git submodule update --init --recursive   # vendored deps under modules/
./scripts/install-deps.sh                 # system tools (macOS / Ubuntu)
cmake -S . -B build -G Ninja && cmake --build build

./build/bin/zetta              # headed (default); --headless, --data-dir PATH,
                               # --listen HOST:PORT, --peer HOST:PORT
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

Needs CMake 3.28+/Ninja and a C++26 Clang 17+ (Homebrew LLVM on macOS,
`clang++-NN` on Linux; system Apple Clang / Ubuntu GCC 13 are too old — the
build auto-selects). Default build is CPU-only; CUDA is opt-in behind
`-DZETTA_ENABLE_CUDA=ON`. Full instructions + troubleshooting:
[`docs/build.md`](docs/build.md).

Tests are GoogleTest in `tests/` (one file per area), link `zetta-core`, and
exercise the **headless core only**; new files are listed in
`tests/CMakeLists.txt`. `tests/temp_dir.hpp` gives each test an isolated
data_dir. **Add or extend tests whenever you touch core logic.** Multi-node
tests run real Corosio TCP in-process — make convergence loops **patient**
(poll seconds, not a fixed tiny count). There is also an in-app, headless-runnable
test-suite catalog (`zetta::testkit`) — see
[`docs/testing/`](docs/testing/README.md).

## Code layout (orient fast; full map in docs/architecture/code-map.md)

```
include/zetta/   zetta-core PUBLIC API (no SDL/ImGui)
  runtime.hpp / runtime/net.hpp   async + networking seams (Capy/Corosio)
  node/database/table/storage/    the data model + StorageEngine
  cluster.hpp signal.hpp collectives.hpp replication.hpp   the distributed plane
  ir/ query/ input/               ExecutionGraph IR · dialect-agnostic SQL · language frontends
  compute/ ddf/ ml/ index/        devices+schedulers · dataframes · native ML · indexes
  parquet/ repl/ testkit/ checks/ native Parquet · console · in-app tests · gate checks
src/core/        zetta-core impl (catalog.hpp = internal CRDT key scheme)
src/gui/         App — links SDL3 + ImGui; app.hpp forward-declares, only app.cpp
                 pulls heavy headers (keep it that way)
src/main.cpp     arg parsing + run-mode dispatch + wiring
schemas/         generated, drift-checked JSON schemas (source of truth is the C++)
modules/         vendored submodules: capy, corosio, automerge-cpp, oramacore-cpp
tests/           GoogleTest (link zetta-core); listed in tests/CMakeLists.txt
docs/            domain-organized system docs (see docs/README.md)
```

## Conventions

### Cloud providers — permission-gated (HARD RULE, all UI work)

**Any UI that touches a cloud provider is permission-gated. No exceptions,
ever.** This binds both the **product** and **Claude when building it**.

- **Never read a cloud provider's config or credential files without explicit
  permission first** — `~/.aws/**`, `~/.kube/config`, `~/.config/gcloud/**`,
  `~/.azure/**`, credential env vars, the instance-metadata endpoint
  (`169.254.169.254`), etc.
- **Never call a cloud provider without explicit permission first** — any
  API / SDK / CLI call (`aws`, `gcloud`, `az`, `kubectl`, `oci`, raw REST).
  Writes (deploy, create, delete) are especially sensitive — confirm intent.
- **Ask every time.** Consent is per-action, deny-by-default, never remembered
  unless the user explicitly says so. The safe default reads nothing and calls
  nothing.
- **In the product:** gate the action behind an explicit, in-UI consent step
  naming exactly what will be read or called *before* anything happens. No
  silent background reads of cloud state.
- **For Claude:** before writing code that reads cloud creds/config or calls a
  cloud, ask the developer first. When in doubt, surface what *would* be
  read/called and ask — never touch the cloud to "just check."

### Distributed-first — every algorithm scales at both levels (FOUNDATION)

**Any algorithm zetta adopts is re-implemented native (no foreign runtime,
ADR-17) and structured to exploit ALL available hardware at BOTH levels:**

- **Cluster level**: SPMD over owned partitions (compute-near-data), partials
  combined via the collectives, single-node == multi-node by construction —
  and the cluster itself is elastic (the `Provisioner` seam can add local or
  cloud nodes, consent- and cost-gated), so "available hardware" includes
  hardware zetta can provision.
- **Aggregate bandwidth counts as hardware.** Ingest and egress scale the same
  way compute does: N nodes have N uplinks. Pulling an external result set
  through one node wastes N-1 of them *and* puts the rows on the wire twice
  (once to the coordinator, once to their eventual owner). Tile the work set,
  fetch each tile on the node that will own it, and land it there — the
  write-side mirror of compute-near-data.
- **Machine level**: heavy numerics target the `ComputeDevice` seam
  (accelerator when present, CPU backend always) and data-parallel work fans
  across cores via `runtime::parallel_for` / the work-stealing scheduler.

A correct-but-single-threaded, single-node implementation is a *reference*,
not a destination — acceptable as the first slice (the GBDT/TreeSHAP
host-side-reference pattern) only with the distributed/accelerated path
designed behind the same API. Never add an algorithm that structurally can't
distribute (hidden global state, order-dependent single streams) without
recording why. Precedents to copy: distributed sort/join/group-by,
histogram-allreduce GBDT, per-partition scatter-gather fulltext.
Applies to AI ops (featurize / train / score / explain) and everything else —
search, aggregation, resolution, import.

### Design system — follow it for all GUI work

The headed app follows the **Orama design system**: light surfaces, ink text,
**Geist + Geist Mono**, coral used *surgically*, semantic green/amber/red for
status, sentence case, no em-dashes in product copy (use `·`/colons).
**Read [`docs/gui/design-system.md`](docs/gui/design-system.md) before adding
or changing GUI**, and the load-bearing rendering notes in
[`docs/gui/README.md`](docs/gui/README.md) (per-monitor scaling, render scale,
idle loop + live resize, font rebuild).

### Say what you mean — do not write "seam"

**Never use the word "seam"** — in code comments, docs, commit messages, issues,
PR text, or conversation. It reads as precise and carries no information: in
this repo alone it has been used for at least four different things, so a
sentence containing it cannot be acted on without going to read the code.

Name the actual thing instead:

| Instead of "seam", say… | When you mean | Examples here |
| --- | --- | --- |
| **interface** (or abstract base class) | a type with virtuals that implementations derive from | `StorageEngine`, `ComputeDevice`, `Partitioner`, `Provisioner`, `IndexBackend`, `Dialect`, `StepExecutor` |
| **injection point** (or "an injected callback") | a `std::function` the caller supplies, so a test can pass a fake | `CommandRunner`, `HttpTransport`, `DatasetResolver`, `FleetSource` |
| **boundary** | where untrusted input is validated, or where exceptions must not cross | the `from_json` validators; "no exceptions across the zetta-core **boundary**" |
| **module split** (or "which target owns this") | which library/TU is allowed to name a backend | only `cluster.cpp` + `native_http_transport.cpp` name Corosio; `zetta-api` owns the HTTP libraries |

The same applies to any word that sounds architectural but does not say what
changes: prefer the concrete noun (the class, the function, the target) over the
metaphor. If you cannot name the concrete thing, you do not yet understand the
change well enough to describe it.

**Legacy occurrences:** the word appears ~750 times across `docs/` and the
source comments. Do **not** do a big-bang rename — replace it with the accurate
word whenever you touch a file for another reason.

### C++ conventions

The full statement is
[`docs/architecture/coding-conventions.md`](docs/architecture/coding-conventions.md);
the load-bearing rules:

- **C++26** (`CMAKE_CXX_STANDARD 26`); use modern idioms. **Follow the
  [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/).**
- **Namespaces match the domain directories** — a type in
  `include/zetta/<domain>/` lives in `namespace zetta::<domain>` (mirrored by
  `src/core/<domain>/` and `tests/<domain>/`); no compatibility aliases.
  Root exceptions: `zetta::Node`, `zetta::runtime`, `zetta::gui`. Forward
  declarations of another domain's type go at file scope, fully qualified.
- **Full, descriptive names — legibility first.** No 1–2 letter or cryptic
  abbreviations for namespaces, types, functions, variables, or template
  parameters (`runtime` not `rt`, `error_code` not `ec`, `Value` not `T`).
  Rename existing short names when you touch them.
- **Text is `std::string` / `std::string_view`** — never `char[]` / `char*`
  buffers. ImGui text fields use the `imgui_stdlib` `std::string` overloads.
- **No naked owning/handle pointers.** `std::unique_ptr` for sole ownership;
  `std::shared_ptr` for shared/handle ownership (lookups return a `shared_ptr`,
  empty on miss). References for non-owning params/returns. Raw pointers ONLY
  for unavoidable C-API handles kept RAII-managed, and for optional non-owning
  back-references that may be null (e.g. `ReplicationManager*`).
- Prefer `const`, RAII, value semantics, `enum class`, and free functions over
  members where the Core Guidelines would.

### Commits, branches, PRs

- **Conventional Commits** (`feat:`/`fix:`/`refactor:`/`docs:`/`test:`…),
  imperative mood, subject ≤72 chars; body explains *why*. Long bodies via a
  `-F <file>` message file.
- **Never mention "Claude" / "AI" / "Codex"** in commit text or PR bodies, and
  **never add Claude as an author or co-author** (no `Co-Authored-By`, no
  "Generated with…" footer). The author is the user, full stop.
- **All work goes on a feature branch and lands via a PR targeting `dev`** —
  never push directly to `main` or `dev`; `dev` is the integration branch and
  promotes to `main`. Branch names: `<type>/<short-slug>` (e.g.
  `docs/domain-restructure`, `feat/provisioner-detection-from-cli`). Commit and
  push the branch after any major, verified unit of work — don't leave finished
  work unpushed.
- Reference issues by number (`#NNN`) within this repo, full URLs across repos;
  don't auto-close (`fixes #N`) — issues stay open through validation.

### Documentation — update it with EVERY change (not optional)

Docs are **part of the change, in the same commit** — never a follow-up. After
any change to behavior, architecture, build, CLI, layout, or conventions,
update every relevant surface:

- **The matching `docs/<domain>/<page>.md`** — one page per shipped slice, filed
  in its domain; add the page to the **domain's `README.md` index** in the same
  commit. Long-form background goes in `docs/research/` with a provenance
  header.
- **[`docs/architecture/code-map.md`](docs/architecture/code-map.md)** — when
  files are added, moved, or renamed.
- **The directory's `CLAUDE.md` rule-card** (`src/core/`, `src/gui/`,
  `include/zetta/`, `tests/`, `docs/`, `schemas/`) — when a rule, invariant, or
  gotcha scoped to that tree changes.
- **This `CLAUDE.md`** — only for repo-wide rule/convention/layout changes
  (keep it small; detail lives in the rule-cards and domain docs).
- **`README.md`** — quick start / requirements / the product overview.
- **`modules/README.md`** — when a vendored module changes.
- The relevant **`.claude/skills/*/SKILL.md`** (especially `zetta-dev`) — when
  a convention or workflow changes.

Before committing, **grep the docs for stale terms** (renamed symbols, removed
flags): if you renamed or removed something, no doc should still mention the
old name.

### Knowledge lives in git — do NOT use the agent memory store

Do not use the file-based memory system (`~/.claude/**/memory/`, `MEMORY.md`).
It lives outside the repo, so it is not reproducible. Durable knowledge goes in
git-tracked files: this `CLAUDE.md` (rules), `docs/**` (system behavior),
`README.md`, or the relevant `.claude/skills/*/SKILL.md`. If asked to
"remember" something, write it to the right git-tracked doc.

## Known gaps / behavior notes

- **Headed mode doesn't quit on terminal Ctrl-C** (macOS SDL3) — close the
  window or Cmd+Q; only headless installs SIGINT/SIGTERM handlers.
- More in [`docs/architecture/code-audit.md`](docs/architecture/code-audit.md)
  (the living health audit) and each domain README's scope notes.

## Skills

Project skills live in `.claude/skills/` (committed, shared):
- **zetta-dev** — these conventions as a working checklist + `reference.md`
  (recipes, gotchas).
- **gui-dev** — headed-GUI work: the view registry + widget grammar +
  navigation architecture, design-system discipline.
- **api-dev** — control-surface work: the capability registry, MCP/REST,
  auth/consent, the Beast2 adapter boundary.
- **mcp-journeys** — author/run simulated user tests; the promotion rule.
- **chaos-experiment** — inject one fault, assert the cluster *heals* within a
  bounded deadline (hypothesis · fault · invariant · deadline · blast radius);
  the safety rules and the prove-the-fault-landed discipline.
- **add-use-case** — build a UC-N in the engine (compose-don't-invent, the
  FedEx-test rule, the nine-step recipe).
- **native-ai-dev** — build AI natively: a model / layer / autograd op / tokenizer
  / training or RLVR loop on `zetta::autograd` + the `ComputeDevice` seam + the BPE
  tokenizer, **no foreign runtime** (ADR-17). The gradient-check discipline, the
  Qwen exemplar, checkpoint load/save, generation, and the performance playbook.
  Points at [`docs/autograd/building-ai-primitives.md`](docs/autograd/building-ai-primitives.md).
- **rlvr-training-data** — generate the verifiable-reward **training corpora** that
  train the agents, following blender-rl's *grow by generation, gate by verification*
  method: reference solutions + negative-control **discrimination** + the admission
  gate + difficulty tiers + a held-out split, synthesized from a solutions catalog
  (→ a private HF dataset) or a customer's schema (→ Postgres). Reproduces (does not
  embed) `homer6/blender-rl`'s dataset strategy.
- **new-input** — add a new input language/format (parser + executor that
  re-implements its behavior in zetta's runtime).
- **new-connector** — add a new source connector (pull rows from a warehouse /
  API into a partitioned dataset; the read-only guard, the injected transport,
  typed cells, no vendor SDK).
- **github-ops** — issues/PRs/CI via `gh` (priority `p1`–`p4` + area labels).
- **skill-writer** — author new skills.
- **perplexity** — generate a deep-research prompt from a topic.
