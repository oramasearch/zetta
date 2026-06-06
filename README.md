# zetta

A database. A *zetta node* is one running instance; it runs **headed** (default
— with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers with no
display). C++26, macOS-first, single statically-linked binary.

Data is organized as databases → tables → JSON rows, persisted to disk. Query it
with basic Postgres-flavored SQL (DDL + INSERT/SELECT); the query engine is
dialect-agnostic, so other SQL formats can be added as frontends. See
[docs/](docs/) for details.

## Build + run

```bash
cmake -S . -B build -G Ninja
cmake --build build

./build/bin/zetta                       # headed (default)
./build/bin/zetta --headless            # no UI, Ctrl-C to stop
./build/bin/zetta --data-dir /path/db   # override the database directory
./build/bin/zetta --help
```

The first configure clones SDL3 and Dear ImGui via CMake `FetchContent` (a few
minutes); subsequent builds are fast (ccache-backed when available). No
`brew install` step is needed for the GUI stack.

## Tests

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

## Requirements

- CMake 3.28+ and Ninja
- Homebrew LLVM Clang on macOS (`brew install llvm`, Clang 21) — C++26 needs
  Clang 17+; Apple's system Clang is too old. The build auto-selects it.
- macOS (SDL3 uses a Metal-backed renderer); other platforms should work but
  are untested
