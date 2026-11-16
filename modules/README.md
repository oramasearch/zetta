# modules — vendored dependencies (git submodules)

zetta is a bridge-style repo: dependencies we want **pinned and locally
readable** (rather than fetched opaquely) live here as git submodules, each
pointing at its own upstream repo. This is distinct from the FetchContent deps
(SDL3, Dear ImGui, nlohmann/json, GoogleTest), which are pulled at configure
time and not vendored.

## Modules

| Module | Upstream | Branch | Role | Built? |
|---|---|---|---|---|
| `capy` | [cppalliance/capy](https://github.com/cppalliance/capy) | `develop` | C++ Alliance **Capy** — coroutine foundation (`task<T>`, executors, thread pools). Backs the `zetta::runtime` interface. | yes |
| `corosio` | [oramasearch/corosio](https://github.com/oramasearch/corosio) (Orama's fork of [cppalliance/corosio](https://github.com/cppalliance/corosio)) | `orama-main` | **Corosio** — coroutine-native networking (the Boost.Asio successor). Vendored via a private fork carrying the epoll `EPOLLHUP`/`EPOLLRDHUP` translation fix ([zetta#740](https://github.com/oramasearch/zetta/issues/740)) — upstream spun the reactor at 100% CPU on a hung-up fd, starving new connections — ALPN negotiation (corosio#4), and `tls_stream::peer_certificate_der()`, which the cluster uses to pin a peer's `HELLO` node id to its certificate ([zetta#669](https://github.com/oramasearch/zetta/issues/669), [corosio#15](https://github.com/oramasearch/corosio/pull/15)), the reactor lifetime fixes (corosio#6, #8, #11, #13), and full-duplex `openssl_stream` reads and writes ([corosio#16](https://github.com/oramasearch/corosio/pull/16), [zetta#835](https://github.com/oramasearch/zetta/issues/835)). Pinned to `orama-main` at `be514224`. | yes (backs the cluster, `src/core/cluster.cpp`) |
| `automerge-cpp` | [homer6/automerge-cpp](https://github.com/homer6/automerge-cpp) | `main` | From-scratch **C++23 CRDT** (no Rust). Has its own nested submodules (`upstream/json` + `upstream/automerge`). | yes (backs replication, `src/core/replication.cpp`) |
| `oramacore-cpp` | [oramasearch/oramacore-cpp](https://github.com/oramasearch/oramacore-cpp) (**private**) | `dev` | **C++23 port of OramaCore's full-text engine** (BM25F first). Backs the `OramaCoreFulltextIndex` search backend. | yes (CPU-only, `src/core/index/oramacore_fulltext_index.cpp`) — **guarded** |

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

Audit catalog retirement (#1164) uses the pinned automerge-cpp document/change
codecs only inside `src/core/cluster/replication.cpp`. The adapter preserves
non-audit operation identities and actor clocks while discarding audit history;
no submodule files or pointers change. Codec upgrades must rerun the migration,
concurrent-removal and restart regressions documented in
[audit replication](../docs/governance/audit-trail.md).

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

## http + beast2 — the API surface's HTTP stack

- **`modules/http`** — cppalliance/http (sans-I/O HTTP/1 + router + json_sink),
  pinned to the **oramasearch/http** fork's `zetta-patches` branch (a portable
  file-time conversion; libc++ lacks `std::chrono::clock_cast`).
- **`modules/beast2`** — cppalliance/beast2, the coroutine-native HTTP/1
  server on the same capy/corosio runtime as the cluster.

Both are consumed ONLY by the `zetta-api` adapter TU
(`src/core/api/http_server.cpp`). Their remaining Boost deps come from the
official boost-cmake release via FetchContent (see the root CMakeLists block
and docs/research/beast2-http.md).
