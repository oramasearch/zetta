# modules — vendored dependencies (git submodules)

zetta is a bridge-style repo: dependencies we want **pinned and locally
readable** (rather than fetched opaquely) live here as git submodules, each
pointing at its own upstream repo. This is distinct from the FetchContent deps
(SDL3, Dear ImGui, nlohmann/json, GoogleTest), which are pulled at configure
time and not vendored.

## Modules

| Module | Upstream | Branch | Role | Built? |
|---|---|---|---|---|
| `capy` | [cppalliance/capy](https://github.com/cppalliance/capy) | `develop` | C++ Alliance **Capy** — coroutine foundation (`task<T>`, executors, thread pools). Backs the `zetta::runtime` seam. | yes |
| `corosio` | [cppalliance/corosio](https://github.com/cppalliance/corosio) | `develop` | **Corosio** — coroutine-native networking (the Boost.Asio successor). | yes (backs the cluster, `src/core/cluster.cpp`) |
| `automerge-cpp` | [homer6/automerge-cpp](https://github.com/homer6/automerge-cpp) | `main` | From-scratch **C++23 CRDT** (no Rust). Has its own nested submodules (`upstream/json` + `upstream/automerge`). | yes (backs replication, `src/core/replication.cpp`) |
| `oramacore-cpp` | [oramasearch/oramacore-cpp](https://github.com/oramasearch/oramacore-cpp) (**private**) | `main` | **C++23 port of OramaCore's full-text engine** (BM25F first). Backs the `OramaCoreFulltextIndex` search backend. | yes (CPU-only, `src/core/index/oramacore_fulltext_index.cpp`) — **guarded** |

`capy` / `corosio` are **beta** (C++ Alliance "rts"/Beast2 family, ~2026) and, with
`automerge-cpp`, require **Clang 17+** — which is why zetta builds with Homebrew
Clang, not Apple Clang. zetta only ever touches the coroutine/networking pair
through `include/zetta/runtime.hpp` (`zetta::runtime`), so the backend can be swapped
or replaced (e.g. with Boost.Asio) by editing that one header. See
[`docs/runtime.md`](../docs/runtime.md) and
[`docs/research/coroutines.md`](../docs/research/coroutines.md).

`oramacore-cpp` is a **private** repo and its CMake link is **guarded** on the
submodule's presence (`ZETTA_WITH_ORAMACORE`): a checkout that did not (or could
not) initialize it still configures, builds, and runs green — the full-text backend
just degrades to `Unsupported`. Initialize it with
`git submodule update --init modules/oramacore-cpp`. See
[`docs/oramacore-fulltext-index.md`](../docs/oramacore-fulltext-index.md).

## Working with submodules

```bash
# Populate after a fresh clone (or clone with --recurse-submodules):
git submodule update --init --recursive

# Update a module to the latest commit on its tracked branch, then commit:
git submodule update --remote modules/capy
git add modules/capy && git commit -m "chore: bump capy"

# Add a new module:
git submodule add -b <branch> <url> modules/<name>
```

CMake builds `capy` via `add_subdirectory(modules/capy EXCLUDE_FROM_ALL)` with
its tests/examples/bench disabled; only the `Boost::capy` library is consumed.
