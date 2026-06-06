# zetta

A database. A *zetta node* is one running instance; it runs **headed** (default
— with an SDL3 + Dear ImGui UI) or **headless** (no UI, for servers with no
display). C++26, macOS-first, single statically-linked binary.

Data is organized as databases → tables → JSON rows, persisted to disk. Query it
with basic Postgres-flavored SQL (DDL + INSERT/SELECT); the query engine is
dialect-agnostic, so other SQL formats can be added as frontends. The async
runtime is coroutine-native (C++ Alliance Capy/Corosio), behind the `zetta::runtime`
seam. State replicates between nodes as an Automerge CRDT. See [docs/](docs/) for
details.

zetta is also a **bridge repo**: pinned dependencies it builds against are
vendored as git submodules under [`modules/`](modules/) (`capy`, `corosio`, and
`automerge-cpp`).

## Build + run

Full instructions — prerequisites, supported compilers, submodules,
troubleshooting — are in **[docs/build.md](docs/build.md)**. The short version:

```bash
# Clone with submodules (or run the submodule step in an existing checkout):
git clone --recurse-submodules git@github.com:homer6/zetta.git
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

Run multiple nodes as a cluster of **equal peers**. Each node listens on all
interfaces by default (`0.0.0.0:7777`) and dials any `--peer` you give it.
Several nodes can share a host — each binds the first free port in a range
(default 16). At startup a node prints the address(es) it's reachable at — the
wildcard bind expands to the machine's real LAN IPs — and the exact command to
add another node.

```bash
zetta                                # first node — prints its LAN address to share
zetta --peer 192.168.1.10:7777       # another machine joins it
zetta --peer 127.0.0.1:7777          # second node on this host (auto-binds 7778)
zetta --listen 127.0.0.1:7777        # restrict to loopback (not network-reachable)
```

State replicates between nodes as an [Automerge](docs/replication.md) CRDT
(replicate-to-all policy; each node has a UUIDv4 actor id). Peers gossip their
state and converge conflict-free. You can see **all nodes and their replication
status** (entries, version, in-sync/syncing) three ways: the headed **Cluster**
panel's Nodes table, the headless heartbeat log, or SQL —
`SELECT * FROM zetta_nodes;`. See [docs/cluster.md](docs/cluster.md) and
[docs/replication.md](docs/replication.md). Membership + replication for now —
**leader election and failover are the planned next step**.

The first configure clones SDL3, Dear ImGui, and GoogleTest via CMake
`FetchContent` (a few minutes); the Capy/Corosio coroutine stack and
automerge-cpp (which vendors nlohmann/json) come from the `modules/` submodules.
Subsequent builds are fast (ccache-backed when available).

## Tests

```bash
cmake --build build --target zetta-tests
ctest --test-dir build --output-on-failure
```

## Requirements

- CMake 3.28+ and Ninja
- Git submodules initialized recursively (`git clone --recurse-submodules`, or
  `git submodule update --init --recursive`) — the build needs
  `modules/{capy,corosio,automerge-cpp}`, and automerge-cpp nests its own
  submodules (nlohmann/json)
- A C++26 Clang 17+ — Homebrew LLVM on macOS (`brew install llvm`, Clang 21),
  `clang++-NN` on Linux; C++26 and Capy/Corosio need Clang 17+, so Apple's
  system Clang and Ubuntu's GCC 13 are too old. The build auto-selects a
  suitable one. `./scripts/install-deps.sh` installs everything.
- macOS for the headed UI (SDL3 uses a Metal-backed renderer); the headless
  core is portable (Linux builds and runs).

Full details and troubleshooting: **[docs/build.md](docs/build.md)**.
