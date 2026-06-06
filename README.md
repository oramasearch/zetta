# zetta

A database. A *zetta node* is one running instance; it runs **headed** (default
— with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers with no
display). C++26, macOS-first, single statically-linked binary.

Data is organized as databases → tables → JSON rows, persisted to disk. Query it
with basic Postgres-flavored SQL (DDL + INSERT/SELECT); the query engine is
dialect-agnostic, so other SQL formats can be added as frontends. The async
runtime is coroutine-native (C++ Alliance Capy/Corosio), behind the `zetta::runtime`
seam. See [docs/](docs/) for details.

zetta is also a **bridge repo**: pinned dependencies it builds against are
vendored as git submodules under [`modules/`](modules/) (currently `capy` and
`corosio`).

## Build + run

```bash
# Clone with submodules (or run the submodule step in an existing checkout):
git clone --recurse-submodules <repo-url>
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

Run multiple nodes as a cluster of **equal peers**. Each node listens (default
`127.0.0.1:7777`) and dials any `--peer` you give it. Several nodes can share a
host — each binds the first free port in a range (default 16). In headed mode
the **Cluster** panel shows the command to add another node and the connected
peers; in headless mode the heartbeat logs the peer count.

```bash
zetta                                # first node (binds 127.0.0.1:7777)
zetta --peer 127.0.0.1:7777          # second node here (auto-binds 7778), joins it
```

See [docs/cluster.md](docs/cluster.md). Membership only for now — **leader
election and failover are the planned next step**.

The first configure clones SDL3, Dear ImGui, nlohmann/json, and GoogleTest via
CMake `FetchContent` (a few minutes); the Capy/Corosio coroutine stack comes
from the `modules/` submodules. Subsequent builds are fast (ccache-backed when
available).

## Tests

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

## Requirements

- CMake 3.28+ and Ninja
- Git submodules initialized (`--recurse-submodules`) — the build needs
  `modules/capy`
- Homebrew LLVM Clang on macOS (`brew install llvm`, Clang 21) — C++26 and
  Capy/Corosio need Clang 17+; Apple's system Clang is too old. The build
  auto-selects it.
- macOS (SDL3 uses a Metal-backed renderer); other platforms should work but
  are untested
