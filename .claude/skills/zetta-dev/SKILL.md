---
name: zetta-dev
description: zetta's project coding conventions and architecture. Use whenever writing, reviewing, or committing code for the zetta database — C++/CMake, storage, the query layer, the cluster, replication/CRDT, large objects, the SDL3+ImGui GUI, tests, or docs. Covers C++ style (C++26, Core Guidelines, smart pointers, std::string, full-word names), the bridge/modules layout (Capy/Corosio/automerge-cpp), the zetta-core/no-SDL rule, the data model, the "EVERYTHING is replicated" CRDT rule (document = source of truth, disk = projection), the equal-peer self-healing cluster, the runtime/net seams, build/test commands, the feature-branch+PR flow, the no-memory rule, and the docs-after-change rule. Pairs with reference.md (a codebase map + how-to recipes).
---

# zetta-dev — coding conventions & architecture checklist

zetta is a C++26 database. A *node* runs **headed** (SDL3 + Dear ImGui UI) or
**headless** (no UI). Apply these to all zetta work. **The root CLAUDE.md holds
the repo-wide rules; directory CLAUDE.md rule-cards** (`src/core/`, `src/gui/`,
`include/zetta/`, `tests/`, `docs/`, `schemas/`) **carry tree-scoped rules; and
`docs/` is domain-organized for progressive disclosure** (`docs/README.md` →
`docs/<domain>/README.md` → the page) — **read a domain's README before working
in it**, and keep every level current in the same commit as the change. This is
the working checklist. For the annotated file-by-file codebase map see
`docs/architecture/code-map.md`; for how-to recipes and gotchas, see
**[reference.md](reference.md)**.

## C++ style

- **C++26** (`CMAKE_CXX_STANDARD 26`, `-std=c++2c`); use modern idioms. Built
  with Homebrew Clang on macOS / `clang++-NN` (≥17) on Linux, auto-selected in
  CMakeLists.txt's pre-`project()` block; Apple Clang / Ubuntu GCC 13 are too old.
- **Follow the C++ Core Guidelines.**
- **Namespaces match the domain directories** (`include/zetta/<d>/` +
  `src/core/<d>/` + `tests/<d>/` ↔ `namespace zetta::<d>`; no aliases; root
  exceptions Node/runtime/gui). Full statement:
  `docs/architecture/coding-conventions.md`.
- **Full, descriptive names — legibility first.** No 1–2 letter or cryptic
  abbreviations for namespaces, types, functions, variables, or template
  parameters (`runtime` not `rt`, `error_code` not `ec`, `Value` not `T`).
- **Text is `std::string` / `std::string_view`** — never `char[]` / `char*`
  buffers. ImGui text fields use the `imgui_stdlib` `std::string` overloads.
- **No naked owning/handle pointers.** `std::unique_ptr` for sole ownership;
  `std::shared_ptr` for shared/handle ownership (lookups return a `shared_ptr`,
  empty on miss). References (`T&`) for non-owning params/returns. Raw pointers
  ONLY for unavoidable C-API handles (`SDL_Window*`, the ImGui font buffer) kept
  RAII-managed, and for **optional, non-owning back-references** that may be null
  (e.g. `ReplicationManager*` threaded into `Node`/`Database`/`Table`).
- Prefer `const`, RAII, value semantics, and `enum class`.

## Architecture rules (the load-bearing ones)

- **`zetta-core` (the static lib) has NO SDL/ImGui dependency** — a node must run
  headless. The GUI links `zetta-core`; never the reverse.
- **EVERYTHING is replicated state.** The Automerge CRDT document
  (`ReplicationManager`) is the **single source of truth** for all distributed
  state — schemas (DDL), rows, large-object metadata, node membership. The
  on-disk store + in-memory `Node` tree are a **projection** of it. Never keep a
  parallel copy synced by hand. New shared state = a new key/shape in the
  document; new *kind* of agreement = a new `ReplicationPolicy` — never a bespoke
  sync path. (See `docs/cluster/replication.md`.)
- **Capture / gossip / apply (the replication flow):** local mutations are
  **captured** into the CRDT at the *data-model layer* (`Node`/`Database`/`Table`/
  `LargeObjectStore` hold a `ReplicationManager*`), so *every* write path (query
  executor, file import, GUI) replicates. Peers **gossip** `save()`/`merge()`
  blobs. `Node::tick()` **reconciles** the on-disk projection from the document
  (materialize-only, never re-capture → no feedback loop) and persists it.
- **Thread safety:** the cluster's **network thread** may only touch
  thread-safe things (`ReplicationManager` is mutex-guarded; `BlobStore` works
  off the CRDT + disk). The `Node` tree / stores are **main-thread only**
  (capture + reconcile run there). Don't touch `Node`/`Database`/`Table` from the
  network thread.
- **Multi-leader by default; leadership is a scoped exception.** No global leader
  or election (equal peers). Per-(node, resource) leadership for *exclusive*
  structures (locks) is **documented design only**, not built. Don't add a global
  leader. (See `docs/cluster/replication.md` → Consistency model.)
- **Pluggable seams — extend, don't special-case:** storage formats implement
  `StorageEngine`; query inputs are dialect-agnostic (parse to the generic
  `zetta::query` AST; the `Executor` is dialect-neutral — the virtual read-only
  `system` database, e.g. `system.nodes`, is resolved there, not in a
  dialect). Add a dialect/engine by implementing the interface, not the executor.
  (To add a whole new input *language*, see the **new-input** skill.)
- **Surface observable state as a `system.zetta_*` table by default (RULE-7).**
  State you expose for inspection/observability — cluster nodes, deployments,
  models, jobs, **cloud/provider state** — defaults to a virtual, read-only
  `system.zetta_*` table computed at query time from the live source
  (`Node::…()` / a catalog), not a bespoke GUI-only panel or a hand-synced copy.
  Composes with "EVERYTHING is replicated": durable state's CRDT is the read
  view's source; ephemeral/observed state (a live cloud inspect) is a read view
  with no CRDT backing. Cloud-derived tables stay permission-gated. (See
  `CLAUDE.md` → Conventions → *System tables*.)
- **Async/networking seams:** all coroutine/IO goes through `zetta::runtime`
  (`include/zetta/runtime.hpp`); networking goes through `zetta::runtime::net`
  (`include/zetta/runtime/net.hpp`, including its TLS surface), included
  **only** by the adapter `src/core/cluster/cluster.cpp`. **Never name
  `boost::capy` / `boost::corosio` directly** outside those seams — the beta
  backend must stay swappable. Likewise OpenSSL libcrypto is named **only** by
  `src/core/cluster/security.cpp` (join-token/CA/identity minting, #466).
- **Bridge/modules:** pinned third-party deps are git submodules under `modules/`
  — **capy** (coroutines), **corosio** (networking), **automerge-cpp** (CRDT;
  also provides nlohmann/json and has nested submodules). Built via
  `add_subdirectory`, not FetchContent. Clone with `--recurse-submodules`
  (recursive matters for automerge-cpp). See `modules/README.md`.

## Build & test

```bash
git submodule update --init --recursive   # vendored deps under modules/
cmake -S . -B build -G Ninja && cmake --build build
./build/bin/zetta              # headed (default); --headless, --data-dir PATH,
                               # --listen HOST:PORT, --peer HOST:PORT
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

GoogleTest lives in `tests/`, links `zetta-core`, exercises the **headless core
only**, and is listed explicitly in `tests/CMakeLists.txt` (add new files there).
`tests/temp_dir.hpp` gives each test an isolated data_dir. **Add or extend tests
whenever you touch core logic** (storage, persistence, CRUD, parsing, cluster,
replication). Multi-node tests run real Corosio TCP in-process; make convergence
loops **patient** (poll seconds, not a fixed tiny count) — they're timing-bound.

## Commits, pushing, docs, memory

- **Conventional Commits** (`feat:`/`fix:`/`refactor:`/`docs:`/`test:`…),
  imperative, subject ≤72 chars; body explains *why*. Long bodies via a
  `-F <file>` message file (shell quoting mangles inline multi-line messages).
- **Never mention Claude/AI** and **never add Claude as author or co-author**
  (no `Co-Authored-By`, no "Generated with…" footer). The author is the user.
- **All work goes on a feature branch and lands via a PR targeting `dev`**
  (`<type>/<short-slug>`, e.g. `docs/domain-restructure`) — never push directly
  to `main` or `dev` (`dev` integrates, then promotes to `main`). Commit and
  push the branch after a major, verified unit of work — don't leave finished
  work unpushed.
- **After any system change, update ALL relevant docs in the same commit** —
  the matching `docs/<domain>/<page>.md` **and its domain `README.md` index**,
  `docs/architecture/code-map.md` (on file adds/moves/renames), CLAUDE.md
  (rules/layout only), README.md, `modules/README.md`, and the affected skill.
  Grep for stale terms before committing. Not just `docs/`.
- **`docs/full-context.md` is the reload map** ("let's reload the full
  project context") — refresh its Current state section in the same commit
  when project state shifts (PR merges, plan changes, waves/foundations
  shipping).
- **Do NOT use the agent memory store.** It's outside git and not reproducible.
  Durable knowledge goes in git-tracked files (CLAUDE.md, docs, README, skills).

## Layout (orient fast; full map in reference.md)

```
include/zetta/   public core API (no SDL/ImGui)
  runtime.hpp / runtime/net.hpp   async + networking seams (Capy/Corosio)
  node/database/table/storage/    the data model + StorageEngine
  cluster.hpp replication.hpp     equal-peer cluster + Automerge CRDT
  blob_store.hpp + replicated_*   large-object body transfer seam
  query/                          dialect-agnostic query layer
src/core/        zetta-core impl  (catalog.hpp = internal CRDT key scheme)
src/gui/         App — links SDL3 + ImGui
src/main.cpp     arg parsing + run-mode dispatch + wiring
modules/         vendored submodules: capy, corosio, automerge-cpp
tests/           GoogleTest (link zetta-core); listed in tests/CMakeLists.txt
docs/            domain-organized system docs (docs/README.md → docs/<domain>/)
```
