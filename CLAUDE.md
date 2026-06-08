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
  successor); built, and backs the cluster (`src/core/cluster.cpp`).
- `modules/automerge-cpp` — from-scratch **C++23 CRDT** (no Rust); backs
  replication (`src/core/replication.cpp`). Has its **own** nested submodules
  (`upstream/json`, which also provides zetta's nlohmann/json) — so the
  `--recursive` clone matters.

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
main()                       picks the mode, owns the Node + Cluster lifecycle
  │  builds zetta::Node + zetta::Cluster, starts both
  ├── --headless ─→ zetta::run_headless(node, cluster)   # core only, no SDL
  └── --headed   ─→ zetta::gui::App(node, cluster).run() # SDL3 + ImGui over them
  ...then cluster.stop(), node.stop()
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

### Cluster (equal peers)

Nodes form a cluster of **equal peers** over TCP — no configured leader. Each
listens on all interfaces by default (`--listen HOST:PORT`, default
`0.0.0.0:7777`, so a plain node is network-reachable; set `127.0.0.1` to
restrict to loopback) and dials any `--peer HOST:PORT` (repeatable); the
handshake is symmetric so both record each other. Nodes default to the same port
and bind the first free one in a range (`--port-range`, default 16) so several
can share a host (e.g. NUMA). On startup (both modes) a node prints its id, the
address(es) it's reachable at, and the join command — a wildcard bind expands to
the host's real LAN IPs (`Cluster::listen_addresses()`), since `0.0.0.0` itself
isn't dial-able; the same reachable address is what it advertises in the
handshake. Links **self-heal**: gossip flows every ~0.5s, so a peer silent past
`peer_idle_timeout_seconds` (default 6) is dropped (clears half-open connections
with no clean EOF), nodes re-dial disconnected `--peer`s every
`reconnect_interval_seconds` (default 3), and peers are keyed by listen address
so a restarted node (new id, same address) **replaces** its stale entry instead
of leaving a ghost. `zetta::Cluster` runs networking on a background thread via
**Corosio**, behind the `zetta::runtime::net` seam (adapter:
`src/core/cluster.cpp`; public API exposes no backend types). The headed Cluster
panel shows the node id, every reachable listen address, peers, and the
**command to add another node** plus a **Nodes & replication** table; headless
logs that table. `Cluster::nodes()` returns this node + every connected peer with
its replication status (entries, version, in-sync, last-sync) — surfaced in the
headed panel, the headless log, and the SQL `zetta_nodes` system table. **Leader
election + failover are the planned next step** (`ClusterRole` exists for it,
currently always `Follower`); membership + replication gossip for now. See
[docs/cluster.md](docs/cluster.md).

### Replication (CRDT)

**Core rule: EVERYTHING is replicated state.** Replication is not a feature
bolted onto some data — it is the single source of truth for *all* distributed
state in zetta: **schemas (databases + tables / DDL), rows, cluster membership
(the node list), metadata — everything.** When you add any state that more than
one node should agree on, it goes through `ReplicationManager` (the Automerge
CRDT), period. Do **not** introduce a parallel, separately-maintained copy of
state that lives outside the replicated document and gets synced by hand — that's
exactly the anti-pattern this rule exists to prevent. The replicated document is
authoritative; in-memory structures (the `Node`'s database tree, the `Cluster`'s
peer table) should be **derived from / kept in lockstep with** it, not the other
way around. New replicated state = a new key/shape in the document, replicated by
the existing gossip; new "kinds of agreement" = a new `ReplicationPolicy`, never a
bespoke sync path.

> Current reality vs. the rule: DDL + rows now flow through replication (the CRDT
> is the source of truth; disk is its projection). The one remaining gap is
> **cluster membership**, still tracked in the `Cluster` peer table separately —
> **migration debt, not the design**: route the node list through replication
> too. Move things *toward* this rule, never away.

**Consistency model: multi-leader by default; leadership only where exclusivity
is required.** Because the base is a CRDT, every node is a writer and merges
converge conflict-free — there is **no global leader / no global election** (the
cluster is equal peers). A leader is needed only for **exclusive** structures
(locks, sequences, single-writer resources), which get a scoped, planned
mechanism: a replicated **`zetta_leadership` system table** holding, per resource,
the holder + a **lease with a timeout** — leadership by *ascension* (claim if
unheld/expired; deterministic tiebreak on concurrent claims; renew before
expiry). **Raft is a future option scoped to just those exclusive structures**,
never the whole DB. Leadership is **per-(node, resource)**: a node holds **zero to
many** roles at once (leader for one resource, follower for another), so
`ClusterRole` (a single value today, always `Follower`) is a placeholder for a
future *set* of roles, not a node-wide title. **Leadership is documented design
only — not built yet**
(`docs/replication.md` → *Consistency model*). The active build is the
multi-leader data plane: **the CRDT document is the source of truth for data
(DDL + rows); the on-disk store + in-memory tree are a materialized projection,
and the document is persisted (`save()`/`merge()`) so it survives restart.** See
`docs/replication.md` → *Data replication*.

A node's replicated state (DDL/metadata/rows) is an **Automerge CRDT**
(`modules/automerge-cpp`, from-scratch C++23, no Rust). `zetta::ReplicationManager`
exposes only JSON + bytes (`set`/`snapshot`/`save`/`merge`, plus `entry_count`/
`version` for status); Automerge is hidden in the adapter
`src/core/replication.cpp`. Peers **gossip** `save()`/`merge()` blobs over the
cluster (the `Cluster` takes a `ReplicationManager*`; per-connection `SYNC`/
`STATUS` exchange on a timer in `src/core/cluster.cpp`) and converge
**conflict-free**. `version()` fingerprints the Automerge heads, so two nodes are
"in sync" exactly when their versions match. Each node's **UUIDv4 is its actor
id** (16 bytes; distinct ids make concurrent edits merge without dropping
changes) — the same UUID as its cluster node id. The document uses **flat keys**
(`src/core/catalog.hpp`): `node:<id>` (membership), `db:<uuid>`, `tbl:<uuid>`,
`row:<uuid>` — each row owns its key by UUID so concurrent edits never collide.
`Node` is the integration point: it **captures** local DDL/row mutations into the
document (via `Node`/`Database`/`Table`, which hold the `ReplicationManager*`),
**reconciles** the on-disk projection from the document on each `tick()` (so peer
changes land on disk), and **persists** it to `data_dir/replication.automerge`.
Policies plug in via `ReplicationPolicy`; the first is `ReplicateToAllNodes`
(full copy on every node). Done + tested: CRDT core, cluster gossip + per-node
status, **and full DDL+row data replication to disk** (`tests/test_data_replication.cpp`).
See [docs/replication.md](docs/replication.md).

### On-disk layout

```
data_dir/
  replication.automerge      ← the replicated CRDT document (source of truth)
  <db-uuid>/                 ← one folder per database (UUIDv4 = its id)
    database.json            ← { id, name }
    <table-uuid>/            ← one folder per table (UUIDv4 = its id)
      table.json             ← { id, name }
      000000.jsonl           ← rows 0..9999    ({ "_zid": row-uuid, "doc": row })
      000001.jsonl           ← rows 10000..19999
      ...                    ← JsonlStore::kRecordsPerChunk = 10k per chunk
```

The folder name *is* the id; the human name lives only in `database.json` /
`table.json` (so rename rewrites that file, never moves the folder). Each JSONL
line is `{ "_zid": <row-uuid>, "doc": <row> }` — `_zid` is the row's CRDT key
(same id on every node); `doc` is what `SELECT` returns. (Legacy bare-row lines
are still read, with a fresh id assigned.) When replicated, the folder tree is a
projection of `replication.automerge`; standalone nodes read/write it directly.
`add_row` appends a line to the active chunk immediately (append-open per row —
simple and crash-resilient; can hold the stream open later). `data_dir` defaults to
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
  UUIDv4; a SHA-256 (`zetta::sha256_hex`) is recorded on import. **They
  replicate split:** the *metadata* (`lo:` key — `{db, name, size, sha256}`)
  rides in the CRDT; the *body* does **not** (it would bloat every node's
  document) — it transfers peer-to-peer (`LOREQ`/`LORES`) and is **verified
  against size + sha256** before being written. The cluster moves bodies through
  the `zetta::BlobStore` seam; `ReplicatedBlobStore` implements it from the CRDT
  metadata + on-disk paths (no Node access → safe on the network thread). Detail
  in [docs/large-objects.md](docs/large-objects.md).

### Query layer

Queries are **dialect-agnostic**: text → a `Dialect` parser → a generic,
dialect-neutral AST (`zetta::query::Statement`, a `std::variant`) → the
`Executor` (which runs the AST against a `Node`). `PostgresDialect` is the first
frontend; `QueryService` is the parse+execute facade the UI calls. Supporting
another database format = writing another `Dialect` that targets the same AST —
**never** add dialect specifics to the executor. Supported today: basic DDL
(CREATE/DROP DATABASE, CREATE/DROP TABLE) and query (INSERT, SELECT + single
`WHERE` equality). The executor also serves the read-only **`zetta_nodes`**
system table (cluster membership + replication status, à la
`pg_stat_replication`) when a `Cluster*` is wired into the `Executor`/
`QueryService` — a reserved table name, not a dialect quirk. Full detail in
[docs/query.md](docs/query.md).

## Tech stack

- **C++26** (`-std=c++2c`) — needs **Clang 17+** (or GCC 14+). Auto-selected:
  Homebrew LLVM Clang on macOS (`brew install llvm`), newest `clang++-NN` on
  Linux. System defaults (Apple Clang 15, Ubuntu GCC 13) are too old. See C++ conventions below
- **CMake 3.28+ / Ninja**, GUI deps via `FetchContent`:
  - **SDL3** (release-3.2.0) — window + input + Metal-backed renderer
  - **Dear ImGui** (v1.91.5) — immediate-mode GUI, SDL3 + SDL_Renderer backend
  - **nlohmann/json** (v3.11.3) — JSON documents for the storage engine
    (linked into `zetta-core`, so headless gets it too)
- **ccache** used as the compiler launcher when present (auto-detected).

## Build + run

```bash
# Submodules (modules/capy, corosio, automerge-cpp + their nested ones):
git submodule update --init --recursive   # or clone with --recurse-submodules
./scripts/install-deps.sh                  # system build tools (macOS / Ubuntu)

cmake -S . -B build -G Ninja
cmake --build build

./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta                                  # cluster node (binds :7777)
./build/bin/zetta --peer 127.0.0.1:7777            # 2nd node here (auto :7778), joins it
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
scripts/install-deps.sh     install system build tools (macOS / Ubuntu)
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the node; CRUD over databases
  database.hpp              zetta::Database — owns tables (disk-backed)
  table.hpp                 zetta::Table — owns a storage engine (its rows)
  large_object.hpp          zetta::LargeObjectStore — files as blobs (per database)
  import.hpp                zetta::import_records — JSON/JSONL files → table rows
  uuid.hpp                  zetta::new_uuid() — v4 UUID for folder names
  sha256.hpp                zetta::sha256_hex — large-object body fingerprint
  bytes.hpp / duration.hpp  human-readable size / time formatters
  runtime.hpp               zetta::runtime — async/coroutine seam over Capy
  runtime/net.hpp           networking slice of the seam (Corosio; adapter-only)
  cluster.hpp               zetta::Cluster — equal-peer membership over TCP
  replication.hpp           zetta::ReplicationManager — Automerge CRDT state
  blob_store.hpp            zetta::BlobStore — large-object body transfer seam
  replicated_blob_store.hpp zetta::ReplicatedBlobStore — BlobStore from CRDT + disk
  headless.hpp              zetta::run_headless(Node&, Cluster&)
  signals.hpp               install_signal_handlers / stop_requested
  storage/
    engine.hpp              zetta::StorageEngine interface
    json_store.hpp          zetta::JsonStore — in-memory vector<json> (header-only)
    jsonl_store.hpp         zetta::JsonlStore — JSONL on disk, 10k/chunk
src/core/                   zetta-core impl
  catalog.hpp               flat-key scheme for the replicated catalog (internal)
  node.cpp                  load_databases() + replication capture/reconcile/persist
  database.cpp
  table.cpp
  jsonl_store.cpp
  uuid.cpp
  sha256.cpp                self-contained SHA-256 (no crypto dependency)
  replicated_blob_store.cpp large-object body transfer (CRDT metadata + disk)
  cluster.cpp               cluster networking adapter (the one Corosio user)
  replication.cpp           replication CRDT adapter (the one automerge-cpp user)
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
  build auto-selects a C++26 compiler in the pre-`project()` block in
  CMakeLists.txt — Homebrew Clang on macOS, `clang++-NN` (≥17) on Linux. System
  defaults (Apple Clang 15, Ubuntu GCC 13) can't do C++26 or build Capy/Corosio.
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

### Documentation — update it with EVERY change (not optional)
Docs are **part of the change, in the same commit** — never a follow-up. After
any change to behavior, architecture, build, CLI, layout, or conventions, update
**every relevant doc surface**:

- **This `CLAUDE.md`** — architecture, code layout, run commands, conventions.
- **`README.md`** — quick start / requirements / feature list.
- **`docs/*.md`** — the matching page(s): `architecture`, `storage`, `query`,
  `large-objects`, `runtime`, `cluster`, `build` — and keep `docs/README.md`
  (the index) in sync. Long-form background/research goes in `docs/research/`
  with a provenance header (source + date + status).
- **`modules/README.md`** — when a vendored module changes.
- The relevant **`.claude/skills/*/SKILL.md`** (especially `zetta-dev`) — when a
  convention or workflow changes.

Before committing, **grep the docs for stale terms** (renamed symbols, removed
flags): if you renamed or removed something, no doc should still mention the old
name. "Update the docs" means *all* of the above, not just `docs/`.

### Committing + pushing
- **Always commit and push after any major work.** When a meaningful unit of
  work is done and verified, `git commit` it and `git push` to `origin/main` —
  don't leave finished work uncommitted or unpushed. Push directly to `main`
  (this is a personal repo; no PR ceremony).

### Knowledge lives in git — do NOT use the agent memory store
- **Do not use the file-based memory system** (the `~/.claude/.../memory/`
  directory, `MEMORY.md`, "save this to memory", etc.). It lives **outside the
  repo**, so it is **not reproducible** — a fresh clone, another machine, or
  another contributor never sees it. Anything worth remembering about this
  project goes into **git-tracked files**: `CLAUDE.md` (conventions,
  architecture, rules), `docs/*.md`, `README.md`, or the relevant
  `.claude/skills/*/SKILL.md` (those *are* committed). If asked to "remember"
  something, write it to the right git-tracked doc instead — never to memory.

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
