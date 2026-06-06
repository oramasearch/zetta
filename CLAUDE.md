# zetta — Claude rules

**zetta is a database.** A *zetta node* is one running instance of it. A node
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
  successor); vendored, wired into the build when zetta grows a server.

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
main()                       picks the mode, owns the Node lifecycle
  │  builds zetta::Node, node.start()
  ├── --headless ─→ zetta::run_headless(node)     # core only, no SDL
  └── --headed   ─→ zetta::gui::App(node).run()   # SDL3 + ImGui over the node
  ...then node.stop()
```

A node owns **databases**, each database owns **tables**, and each table holds
its rows through a **storage engine** (`zetta::StorageEngine`). `Node` has full
CRUD over databases (`create`/`find`/`drop`/`rename_database`, `database_names`)
and `Database` over tables (`create`/`find`/`drop_table`, `table_names`);
ownership is via `shared_ptr` so lookups hand back a safe handle, never a raw
pointer.

**Everything persists to disk** under `config_.data_dir`, and `Node::start()`
rebuilds the in-memory tree by scanning it. Two storage engines exist behind
the `StorageEngine` seam: `zetta::JsonlStore` (the default tables use — JSON
Lines on disk, chunked) and `zetta::JsonStore` (in-memory only; kept for a
future `:memory:` mode). **New engines implement `StorageEngine`; query/indexes
land behind it** — and don't let SDL/ImGui types leak into `zetta-core`.

### Async runtime (`zetta::runtime`)

zetta's async / coroutine / networking layer lives behind
`include/zetta/runtime.hpp` (namespace `zetta::runtime`). Code uses `zetta::runtime::task<T>`,
`zetta::runtime::block_on`, `zetta::runtime::thread_pool`, etc. and **never names
`boost::capy` / `boost::corosio` directly** — so the (beta, churning) C++
Alliance backend can be swapped, or replaced by Boost.Asio, by editing that one
header. Backed by Capy today (`modules/capy`); Corosio (`modules/corosio`) adds
networking. Background + migration plan:
[docs/runtime.md](docs/runtime.md) and
[docs/research/coroutines.md](docs/research/coroutines.md).

### On-disk layout

```
data_dir/
  <db-uuid>/                 ← one folder per database (UUIDv4 = its id)
    database.json            ← { id, name }
    <table-uuid>/            ← one folder per table (UUIDv4 = its id)
      table.json             ← { id, name }
      000000.jsonl           ← records 0..9999      (one JSON doc per line)
      000001.jsonl           ← records 10000..19999
      ...                    ← JsonlStore::kRecordsPerChunk = 10k per chunk
```

The folder name *is* the id; the human name lives only in `database.json` /
`table.json` (so rename rewrites that file, never moves the folder). `add_row`
appends a line to the active chunk immediately (append-open per record — simple
and crash-resilient; can hold the stream open later). `data_dir` defaults to
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
  UUIDv4. Detail in [docs/large-objects.md](docs/large-objects.md).

### Query layer

Queries are **dialect-agnostic**: text → a `Dialect` parser → a generic,
dialect-neutral AST (`zetta::query::Statement`, a `std::variant`) → the
`Executor` (which runs the AST against a `Node`). `PostgresDialect` is the first
frontend; `QueryService` is the parse+execute facade the UI calls. Supporting
another database format = writing another `Dialect` that targets the same AST —
**never** add dialect specifics to the executor. Supported today: basic DDL
(CREATE/DROP DATABASE, CREATE/DROP TABLE) and query (INSERT, SELECT + single
`WHERE` equality). Full detail in [docs/query.md](docs/query.md).

## Tech stack

- **C++26** (`-std=c++2c`) — built with **Homebrew LLVM Clang** (`brew install
  llvm`, Clang 21) on macOS; Apple's system Clang is too old. See C++ conventions below
- **CMake 3.28+ / Ninja**, GUI deps via `FetchContent`:
  - **SDL3** (release-3.2.0) — window + input + Metal-backed renderer
  - **Dear ImGui** (v1.91.5) — immediate-mode GUI, SDL3 + SDL_Renderer backend
  - **nlohmann/json** (v3.11.3) — JSON documents for the storage engine
    (linked into `zetta-core`, so headless gets it too)
- **ccache** used as the compiler launcher when present (auto-detected).

## Build + run

```bash
# Submodules (modules/capy, modules/corosio) must be present:
git submodule update --init --recursive   # or clone with --recurse-submodules

cmake -S . -B build -G Ninja
cmake --build build

./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
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
CMakeLists.txt              build graph: imgui lib, zetta-core lib, zetta exe
cmake/embed_font.cmake      bakes a .ttf into a C++ header at configure time
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the node; CRUD over databases
  database.hpp              zetta::Database — owns tables (disk-backed)
  table.hpp                 zetta::Table — owns a storage engine (its rows)
  large_object.hpp          zetta::LargeObjectStore — files as blobs (per database)
  imporuntime.hpp                zetta::import_records — JSON/JSONL files → table rows
  uuid.hpp                  zetta::new_uuid() — v4 UUID for folder names
  bytes.hpp / duration.hpp  human-readable size / time formatters
  runtime.hpp                    zetta::runtime — async/coroutine seam over Capy
  headless.hpp              zetta::run_headless(Node&)
  signals.hpp               install_signal_handlers / stop_requested
  storage/
    engine.hpp              zetta::StorageEngine interface
    json_store.hpp          zetta::JsonStore — in-memory vector<json> (header-only)
    jsonl_store.hpp         zetta::JsonlStore — JSONL on disk, 10k/chunk
src/core/                   zetta-core impl
  node.cpp                  + load_databases() (scan data_dir on start)
  database.cpp
  table.cpp
  jsonl_store.cpp
  uuid.cpp
  headless.cpp              signal-driven headless loop
  signals.cpp               SIGINT/SIGTERM stop flag
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

The font (DroidSans.ttf from the ImGui tree) is embedded via
`cmake/embed_font.cmake` into `namespace zetta`, so the binary has no runtime
asset dependency.

## Known gaps / behavior notes

- **Headed mode doesn't quit on terminal Ctrl-C** — SDL3 on macOS doesn't turn
  SIGINT into a quit event here, so close the window or use Cmd+Q. Only headless
  mode installs SIGINT/SIGTERM handlers. (Wiring a handler into headed mode is a
  reasonable future tweak if it becomes annoying.)

## Conventions

### C++ conventions
- **C++26** (`set(CMAKE_CXX_STANDARD 26)`, `cxx_std_26`). Use modern
  facilities; don't reach for older idioms when a newer one is clearer. The
  build auto-selects Homebrew Clang on macOS (see the pre-`project()` block in
  CMakeLists.txt); Apple Clang 15 can't do C++26 or build Capy/Corosio.
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

### Documentation
**After any system change, update `docs/*.md` to match** (architecture, storage,
query, build). Docs are markdown and are the human-facing description of the
system — treat them as part of the change, not an afterthought. Long-form
background/research (deep dives, design investigations) goes in `docs/research/`
with a provenance header (source + date + status); index it in `docs/README.md`.

### Committing + pushing
- **Always commit and push after any major work.** When a meaningful unit of
  work is done and verified, `git commit` it and `git push` to `origin/main` —
  don't leave finished work uncommitted or unpushed. Push directly to `main`
  (this is a personal repo; no PR ceremony).

### Idle loop
The headed loop uses `SDL_WaitEventTimeout` (≈30fps idle cap) rather than a busy
vsync spin — it wakes immediately on input so interaction stays full-speed but
doesn't burn CPU on identical frames. The headless loop ticks at 10 Hz. Keep any
background work cheap enough to run inside those cadences (or move it off the
loop thread).

## Pointers

- [`docs/architecture.md`](docs/architecture.md) — node, run modes, data model, module map.
- [`docs/storage.md`](docs/storage.md) — on-disk format + storage-engine seam.
- [`docs/query.md`](docs/query.md) — the dialect-agnostic query layer + Postgres subset.
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
