---
name: zetta-dev
description: zetta's project coding conventions and architecture. Use whenever writing, reviewing, or committing code for the zetta database — C++/CMake, storage, the query layer, the cluster, replication/CRDT, large objects, the SDL3+ImGui GUI, tests, or docs. Covers C++ style (C++26, Core Guidelines, smart pointers, std::string, full-word names), the bridge/modules layout (Capy/Corosio/automerge-cpp), the zetta-core/no-SDL rule, the data model, the "EVERYTHING is replicated" CRDT rule (document = source of truth, disk = projection), the equal-peer self-healing cluster, the runtime/net seams, build/test commands, commit+push, the no-memory rule, and the docs-after-change rule. Pairs with reference.md (a codebase map + how-to recipes).
---

# zetta-dev — coding conventions & architecture checklist

zetta is a C++26 database. A *node* runs **headed** (SDL3 + Dear ImGui UI) or
**headless** (no UI). Apply these to all zetta work. **CLAUDE.md and `docs/` are
authoritative and detailed; this is the working checklist.** For a file-by-file
codebase map, how-to recipes, and gotchas, see **[reference.md](reference.md)**.

## C++ style

- **C++26** (`CMAKE_CXX_STANDARD 26`, `-std=c++2c`); use modern idioms. Built
  with Homebrew Clang on macOS / `clang++-NN` (≥17) on Linux, auto-selected in
  CMakeLists.txt's pre-`project()` block; Apple Clang / Ubuntu GCC 13 are too old.
- **Follow the C++ Core Guidelines.**
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
  sync path. (See `docs/replication.md`.)
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
  leader. (See `docs/replication.md` → Consistency model.)
- **Pluggable seams — extend, don't special-case:** storage formats implement
  `StorageEngine`; query inputs are dialect-agnostic (parse to the generic
  `zetta::query` AST; the `Executor` is dialect-neutral — system tables like
  `zetta_nodes` are matched there by reserved name, not by dialect). Add a
  dialect/engine by implementing the interface, not by branching the executor.
  (To add a whole new input *language*, see the **new-input** skill.)
- **Async/networking seams:** all coroutine/IO goes through `zetta::runtime`
  (`include/zetta/runtime.hpp`); networking goes through `zetta::runtime::net`
  (`include/zetta/runtime/net.hpp`), included **only** by the adapter
  `src/core/cluster.cpp`. **Never name `boost::capy` / `boost::corosio`
  directly** outside those seams — the beta backend must stay swappable.
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
- **Always commit AND push to `origin/main`** after a major, verified unit of
  work — don't leave finished work unpushed.
- **After any system change, update ALL relevant docs in the same commit** —
  CLAUDE.md, README.md, `docs/*.md` (+ `docs/README.md` index), `modules/README.md`,
  and the affected skill. Grep for stale terms before committing. Not just `docs/`.
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
docs/            markdown system docs (+ docs/research/ background)
```
