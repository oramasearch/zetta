# Architecture

## Node and run modes

A **node** (`zetta::Node`) is one running instance of the database. It owns the
data and knows nothing about any UI, so it can run two ways:

- **headed** (default) — `zetta::gui::App` opens an SDL3 + Dear ImGui window and
  renders a live view of the node.
- **headless** — `zetta::run_headless` ticks the node on a 10 Hz heartbeat with
  no UI; for servers with no display.

`main()` parses arguments, builds the `Node`, starts it, dispatches to one run
mode, then stops it. Both modes install SIGINT/SIGTERM handlers so terminal
Ctrl-C shuts down cleanly.

The core is the static library **`zetta-core`**, which has **no SDL/ImGui
dependency** — the build-level guarantee that a node runs headless. The GUI is a
separate translation unit linking `zetta-core`.

## Data model

```
Node
 └── Database        (CRUD: create / find / drop / rename)
      └── Table       (create / find / drop)
           └── rows   (JSON documents, via a StorageEngine)
```

Ownership and lookups use `std::shared_ptr` handles, never raw pointers. Names
are unique within their parent; lookups return an empty handle on a miss.

Everything persists to disk under the node's `data_dir` and is rebuilt by
`Node::start()` on launch. See [storage.md](storage.md).

## Query layer

Text → `Dialect` (parser) → generic AST (`zetta::query::Statement`) →
`Executor` → results. The executor is dialect-neutral; new database formats are
added by writing a `Dialect` that targets the same AST. See [query.md](query.md).

## Module map

```
include/zetta/            zetta-core public API (no SDL/ImGui)
  node.hpp                Node — CRUD over databases
  database.hpp            Database — owns tables (disk-backed)
  table.hpp               Table — owns a storage engine (its rows)
  uuid.hpp                new_uuid() — v4 UUID for folder names
  headless.hpp            run_headless(Node&)
  signals.hpp             install_signal_handlers / stop_requested
  large_object.hpp        LargeObjectStore — files as blobs (Postgres LO API)
  import.hpp              import_records — JSON/JSONL files into a table
  bytes.hpp / duration.hpp human-readable size / time formatters
  storage/engine.hpp      StorageEngine interface
  storage/json_store.hpp  JsonStore — in-memory engine
  storage/jsonl_store.hpp JsonlStore — on-disk JSONL engine (default)
  query/ast.hpp           generic, dialect-neutral statement AST
  query/dialect.hpp       Dialect interface + ParseResult
  query/postgres.hpp      PostgresDialect (parser)
  query/executor.hpp      Executor — runs the AST against a Node
  query/service.hpp       QueryService — parse + execute facade
src/core/                 zetta-core implementation
src/gui/                  headed UI (App), links SDL3 + ImGui
src/main.cpp              arg parsing + mode dispatch
tests/                    GoogleTest unit tests (CTest)
```
