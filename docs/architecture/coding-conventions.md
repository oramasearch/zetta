# Coding conventions

The canonical reference for how zetta code is written. The root `CLAUDE.md`
carries the always-loaded distillation and the directory rule-cards carry the
tree-scoped slices; this page is the full statement. When a convention changes,
change it here **and** in the affected rule-card/checklist in the same commit.

## Language and style

- **C++26** (`CMAKE_CXX_STANDARD 26`, `-std=c++2c`), built with Clang 17+
  (Homebrew LLVM on macOS, `clang++-NN` on Linux — auto-selected by CMake).
  Use modern idioms; don't reach for an older construct when a newer one is
  clearer.
- **Follow the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/).**
- **Full, descriptive names — legibility first.** No 1–2 letter or cryptic
  abbreviations for namespaces, types, functions, variables, or template
  parameters: `runtime` not `rt`, `error_code` not `ec`, `index` not `i`,
  `Value`/`Result` not `T`. Rename existing short names when you touch them.
- **Text is `std::string` / `std::string_view`** — never `char[]` / `char*`
  buffers. ImGui text fields bind `std::string` via the `imgui_stdlib`
  overloads.
- **No naked owning or handle pointers.** `std::unique_ptr` for sole ownership;
  `std::shared_ptr` for shared/handle ownership — lookups return a `shared_ptr`
  that is empty on a miss, never a raw pointer. References (`T&`) for
  non-owning parameters/returns. Raw pointers only for (a) unavoidable C-API
  handles (`SDL_Window*`, the ImGui font buffer), kept RAII-managed, and (b)
  optional, non-owning back-references that may be null
  (`cluster::ReplicationManager*`, `governance::AuditSink*`).
- Prefer `const`, RAII, value semantics, `enum class`, and free functions over
  members where the Core Guidelines would.

## Layout and namespaces — the domain rule

**Directories and namespaces match the docs domains, and each other.** For a
domain `d`, the public headers live in `include/zetta/<d>/`, the implementation
in `src/core/<d>/`, the tests in `tests/<d>/`, the docs in `docs/<d>/`, and the
symbols in `namespace zetta::<d>`:

| Domain | Namespace | Holds |
|---|---|---|
| `cluster/` | `zetta::cluster` | Cluster, ReplicationManager, Signal, Collectives, telemetry |
| `data/` | `zetta::data` | Database, Table, partitioning, large objects, datasets, blob stores |
| `storage/` | `zetta::storage` | StorageEngine, JsonlStore, JsonStore |
| `query/` | `zetta::query` | the dialect-agnostic SQL layer |
| `parquet/` | `zetta::parquet` | the native Parquet reader |
| `ir/` | `zetta::ir` | the ExecutionGraph IR |
| `input/` | `zetta::input` | language frontends (sim-Python, …) |
| `pipeline/` | `zetta::pipeline` | Project, Run, Deployment, executor, gate, compilers, catalog |
| `compute/` | `zetta::compute` | devices, schedulers, coordinator, ComputeJob, autoscaler |
| `ddf/` | `zetta::ddf` | the distributed dataframe + its ops |
| `ml/` | `zetta::ml` | GBDT, calibration, scoring, TreeSHAP, Model |
| `index/` | `zetta::index` | the IndexBackend seam + backends + Index entity |
| `cloud/` | `zetta::cloud` | Provisioner + the vendor backends, burst identity, inspector |
| `governance/` | `zetta::governance` | Principal, AccessKey, Scope, Capability, AuditEvent |
| `usecases/churn/` | `zetta::usecases::churn` | the UC-1 pieces |
| `repl/` | `zetta::repl` | console model + governed console |
| `testkit/` | `zetta::testkit` | the in-app test-suite catalog |
| `util/` | `zetta::util` | uuid, sha256, timestamps, formatters |

- **Root exceptions:** `zetta::Node` (it spans every domain), `zetta::runtime`
  (the async seam), `zetta::gui` (the headed app, under `src/gui/`), and the
  root headers `version.hpp` / `headless.hpp` / `signals.hpp`.
- **No compatibility aliases.** A type has one name; call sites use the
  domain-qualified form (`zetta::pipeline::Project`). Inside a `zetta::x`
  namespace, sibling domains are reached relatively (`data::Table`,
  `ir::ExecutionGraph`) — enclosing-namespace lookup makes that resolve.
- **Forward declarations** of another domain's type go at **file scope**, fully
  qualified: `namespace zetta::cluster { class ReplicationManager; }` — never
  inside your own namespace block (that declares a shadow type). A class's own
  nested/pImpl declarations (`struct State;`, `struct Impl;`) stay nested.
- **New file = the domain's four homes.** A new slice lands its header, impl,
  tests, and doc page in the same domain, and updates the domain `README.md`
  index + `docs/architecture/code-map.md` in the same commit.

## Concurrency — coroutines first

Async APIs are awaitables via the `zetta::runtime` seam (`task<T>`, `spawn`,
`async_event`, `parallel_for`) — a public API never exposes blocking
mutex/condvar semantics. Raw `std::mutex` appears only as the contained
cross-thread handoff inside a seam adapter (outbox/ring patterns), commented
as such. Need a primitive the seam lacks? Extend `runtime.hpp` (one
alias/adapter over capy), never name `boost::capy`/`boost::corosio` outside
the seam. Ownership stays with loops (Node: owning loop; network thread:
thread-safe types only) — cross-thread work is marshaled, not locked.

## Distributed-first — every algorithm scales at both levels

Re-implement adopted algorithms natively (ADR-17) and structure them to
exploit all available hardware: **cluster level** (SPMD over owned partitions,
collectives for partials, single-node == multi-node; the Provisioner seam
makes the fleet elastic, so provisionable cloud hardware counts) and
**machine level** (`ComputeDevice` for heavy numerics — accelerator first,
CPU always; `runtime::parallel_for` / the work-stealing scheduler for
data-parallel fan-out). A single-threaded single-node version is a reference
slice, not a destination — the distributed/accelerated path must be designed
behind the same API from day one. Precedents: distributed sort/join/group-by,
histogram-allreduce GBDT, per-partition scatter-gather fulltext.

## Observability — every operation is observable

New operations open a span (intent · plan · performance · result shape) with
trace ids threaded across calls, surfaces, and nodes
([observability.md](observability.md)); failures carry the engine's reason,
crashes carry stacktraces. An unobservable operation is incomplete.

## Errors and boundaries

- **No exceptions across zetta-core seams** (the library builds with
  `-fexperimental-library`; nothing may unwind across it). Boundary functions
  are non-throwing and return `std::optional` / a status type with a
  path-qualified reason. `runtime::parallel_for` bodies must not throw.
- **Validate untrusted input at the boundary.** Top-level replicated entities
  (Run, Model, ComputeJob, AuditEvent, Index, DatasetPartition, …) validate
  against the embedded draft-07 schema via the pboettch validator (the
  `parse_run_value` pattern). Value-type DTOs (configs, specs, policies) use a
  hand-written non-throwing `from_json` that mirrors their type's
  `*_schema()` accessor (the generated `schemas/*.json` reflects it).
  Binary formats (Parquet, Snappy, checkpoints) are bounds-checked and never
  trusted.
- **Schema as code:** the C++ embedded schema (a `*_schema()` accessor) is the
  source of truth; `schemas/*.json` is generated (`ZETTA_WRITE_SCHEMAS=1`) and
  drift-checked by `tests/test_schemas.cpp`. Prefer **no new JSON format** —
  ride an existing validated boundary when possible. See
  [schemas.md](schemas.md).
- **ADL:** a type's `to_json`/`from_json` live in the type's own namespace and
  move with it — that is what keeps the nlohmann boundary resolving.

## Seams — extend, don't special-case

New capability = a new implementation of an existing seam, never a special case
in a consumer: `storage::StorageEngine`, `query::Dialect` (the `Executor` stays
dialect-neutral), `pipeline::StepExecutor`, `pipeline::Check`,
`compute::ComputeDevice` (new hardware = a new backend),
`index::IndexBackend`, `data::Partitioner`, `cloud::Provisioner` (+ a
`<Vendor>ControlPlane` behind the shared `CommandRunner` — CLI, no cloud SDK),
`input::LanguageFrontend`, `cluster::ReplicationPolicy`. Dependencies zetta-core
must not carry (HTTP, LLM SDKs, cloud SDKs, file/network IO in engine paths)
are **injected** as callables/seams (`FleetSource`, `IdentityHooks`,
`NarratorCompletion`, `DatasetResolver`) so everything is unit-testable against
fakes. Coroutines/IO go through `zetta::runtime`; only the adapters name
Capy/Corosio/automerge.

## The distributed invariants

The replication rules (CRDT document as source of truth, capture/gossip/apply,
thread ownership, multi-leader) are stated in
[../../src/core/CLAUDE.md](../../src/core/CLAUDE.md) and
[../cluster/replication.md](../cluster/replication.md). Observable state
defaults to a `system.<table>` read view (RULE-7), named plainly — no `zetta_`
prefix for new tables (legacy ones await
[#538](https://github.com/oramasearch/zetta/issues/538)).

## GUI, tests, commits

- GUI work follows the **Orama design system** and the thin-renderer rule —
  [../gui/design-system.md](../gui/design-system.md) +
  [../../src/gui/CLAUDE.md](../../src/gui/CLAUDE.md). Product copy: sentence
  case, no em-dashes (use `·`/colons), coral used surgically.
- Tests: one file per area under the domain's `tests/<d>/`, listed explicitly
  in `tests/CMakeLists.txt`; patient convergence loops for multi-node tests;
  no foreign runtime in build/CI — see
  [../../tests/CLAUDE.md](../../tests/CLAUDE.md).
- Commits: Conventional Commits, imperative, ≤72-char subject, body explains
  *why*; never mention Claude/AI/Codex or add an AI author. **All work goes on
  a feature branch and lands via a PR targeting `dev`.** Docs land in the same
  commit as the change — see the docs discipline in the root
  [CLAUDE.md](../../CLAUDE.md).
