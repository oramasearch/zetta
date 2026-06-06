---
name: zetta-dev
description: zetta's project coding conventions and architecture. Use whenever writing, reviewing, or committing code for the zetta database — C++/CMake, storage, the query layer, the SDL3+ImGui GUI, tests, or docs. Covers C++ style (C++23, Core Guidelines, smart pointers, std::string), the zetta-core/no-SDL rule, the data model, build/test commands, commit/push rules, and the docs-after-change rule.
---

# zetta-dev — coding conventions

zetta is a C++23 database. A *node* runs **headed** (SDL3 + Dear ImGui UI) or
**headless** (no UI). Apply these conventions to all zetta work. CLAUDE.md and
`docs/` are authoritative and detailed; this is the working checklist.

## C++ style

- **C++23** (`CMAKE_CXX_STANDARD 23`, `-std=c++2b`); use modern idioms.
- **Follow the C++ Core Guidelines.**
- **Text is `std::string` / `std::string_view`** — never `char[]` / `char*`
  buffers. ImGui text fields use the `imgui_stdlib` `std::string` overloads.
- **No naked owning/handle pointers.** `std::unique_ptr` for sole ownership;
  `std::shared_ptr` for shared/handle ownership (lookups return a `shared_ptr`,
  empty on miss). References (`T&`) for non-owning params/returns. Raw pointers
  ONLY for unavoidable C-API handles (`SDL_Window*`, the ImGui font buffer),
  kept RAII-managed (owned + destroyed by `gui::App`).
- Prefer `const`, RAII, value semantics, and `enum class`.

## Architecture rules

- **`zetta-core` (the static lib) has NO SDL/ImGui dependency** — a node must
  run headless. The GUI links `zetta-core`; never the reverse.
- **Data model:** `Node` owns `Database`s, a `Database` owns `Table`s, a `Table`
  owns a `StorageEngine` (its rows); each `Database` also has a
  `LargeObjectStore`. Everything persists under `data_dir` and is rebuilt by
  `Node::start()`. On disk, ids are **UUIDv4 folder names**; human names live in
  `*.json` metadata (so rename rewrites metadata, never moves a folder).
- **Pluggable seams — extend, don't special-case:** storage formats implement
  `StorageEngine`; query inputs are dialect-agnostic (parse to the generic
  `zetta::query` AST; the `Executor` is dialect-neutral). Add a dialect/engine by
  implementing the interface, not by branching the executor. (To add a whole new
  input *language*, see the **new-input** skill.)

## Build & test

```bash
cmake -S . -B build -G Ninja && cmake --build build
./build/bin/zetta              # headed (default); --headless, --data-dir PATH
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

GoogleTest lives in `tests/`, links `zetta-core`, and exercises the **headless
core only**. **Add or extend tests whenever you touch core logic** (storage,
persistence, CRUD, parsing, formatting).

## Commits, pushing, docs

- **Conventional Commits** (`feat:`/`fix:`/`refactor:`/`docs:`/`test:`…),
  imperative, subject ≤72 chars; body explains *why*.
- **Never mention Claude/AI** and **never add Claude as author or co-author**
  (no `Co-Authored-By`, no "Generated with…" footer). The author is the user.
- **Always commit AND push to `origin/main`** after a major, verified unit of
  work — don't leave finished work unpushed.
- **After any system change, update `docs/*.md`** to match.

## Layout

```
include/zetta/   public core API (no SDL/ImGui)
src/core/        zetta-core implementation
src/gui/         App — links SDL3 + ImGui
src/main.cpp     arg parsing + run-mode dispatch
tests/           GoogleTest (link zetta-core)
docs/            markdown system docs
```
